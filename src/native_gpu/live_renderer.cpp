#include <unistd.h>
// skate1 - native D3D layer: live rendering (see docs/native_d3d.md)
//
// native_gpu_render feeds the game's draws, clears, resolves and tiling
// calls to the native renderer as they happen, next to the emulated GPU (which
// still presents): a step towards replacing it, checked by writing the
// native front buffer to native_gpu_dump_path native_gpu_dump_seconds after
// the first frame. Runs on the render thread, inside the D3D hooks of
// frame_capture.cpp.

#include "live_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <climits>
#include <cstdio>
#include <map>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <link.h>

#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "generated/default/skate1_init.h"
#include "renderer.h"
#include "shader_cache.h"
#include "shader_registry.h"

REXCVAR_DEFINE_BOOL(native_gpu_render, false, "Skate1",
                    "Render through the native D3D layer next to the emulated GPU (offscreen, "
                    "work in progress)");
REXCVAR_DEFINE_BOOL(native_gpu_present, false, "Skate1",
                    "Show the native renderer's frames (with native_gpu_render; pair with "
                    "gpu_skip_guest_draws so the emulated GPU does not render them too)");
REXCVAR_DEFINE_INT32(native_gpu_stats_seconds, 10, "Skate1",
                     "Log frame rate and native renderer time every this many seconds (0 = off)");
REXCVAR_DEFINE_INT32(native_gpu_dump_seconds, 0, "Skate1",
                     "Write the native front buffer this many seconds after the first frame "
                     "(0 = never)");
REXCVAR_DEFINE_STRING(native_gpu_dump_path, "native_frame.ppm", "Skate1",
                      "Where native_gpu_dump_seconds writes the front buffer (PPM); creating "
                      "<path>.trigger also writes the next frame");

namespace {

constexpr uint32_t kDevice = 0x40002200;

rex::memory::Memory* g_memory = nullptr;
std::unique_ptr<native_gpu::Renderer> g_renderer;

// Write watching through the physical memory access callbacks (the emulated
// GPU's shared memory uses the same ones): one dirty bit per 4 KB page.
class PhysicalWriteWatcher : public native_gpu::WriteWatcher {
 public:
  explicit PhysicalWriteWatcher(rex::memory::Memory* memory) : memory_(memory) {
    handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  }
  ~PhysicalWriteWatcher() override { memory_->UnregisterPhysicalMemoryInvalidationCallback(handle_); }

  uint32_t Watch(uint32_t address, uint32_t size) override {
    // Writes before this stamp are in what the caller reads next; later ones
    // fault (the pages are protected first) and get a newer sequence.
    const uint32_t stamp = sequence_.load(std::memory_order_acquire);
    memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    return stamp;
  }

  bool Dirty(uint32_t address, uint32_t size, uint32_t stamp) override {
    if (!size) return false;
    const uint32_t first = (address & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((address & 0x1FFFFFFF) + size - 1) >> 12);
    for (uint32_t page = first; page <= last;) {
      // Blocks of 64 pages keep their latest write, to skip unwritten ones.
      if (blocks_[page >> 6].load(std::memory_order_relaxed) <= stamp) {
        page = (page | 63) + 1;
        continue;
      }
      if (pages_[page].load(std::memory_order_relaxed) > stamp) return true;
      ++page;
    }
    return false;
  }

 private:
  static constexpr uint32_t kPages = 0x20000000 >> 12;

  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<PhysicalWriteWatcher*>(context);
    // What to stop watching: (0, UINT32_MAX) would unwatch the surrounding 4 MB
    // and with it everything else converted there.
    if (!length) return {start, 0};
    const uint32_t sequence = self->sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t first = (start & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((start & 0x1FFFFFFF) + length - 1) >> 12);
    for (uint32_t page = first; page <= last; ++page) {
      self->pages_[page].store(sequence, std::memory_order_relaxed);
      self->blocks_[page >> 6].store(sequence, std::memory_order_relaxed);
    }
    return {start, length};
  }

  rex::memory::Memory* memory_;
  void* handle_ = nullptr;
  std::atomic<uint32_t> sequence_{1};
  // The sequence of the last write to each page, and to each 64 pages.
  std::array<std::atomic<uint32_t>, kPages> pages_{};
  std::array<std::atomic<uint32_t>, kPages / 64> blocks_{};
};
std::unique_ptr<PhysicalWriteWatcher> g_watcher;

// The last native frame for the presenter (A2B10G10R10, the guest output
// format), handed from the render thread to the GPU command thread.
std::mutex g_frame_mutex;
std::vector<uint32_t> g_frame;
uint32_t g_frame_width = 0, g_frame_height = 0;
// Presenter-side upload buffer and the guest output images already written.
rex::graphics::nrhi::Buffer* g_upload = nullptr;
uint64_t g_upload_size = 0;
std::unordered_set<rex::graphics::nrhi::Texture*> g_written_outputs;

// Native guest output renderer (rex/graphics/native_guest_renderer.h): copies
// the last native frame into the presenter's guest output image.
bool PresentNativeFrame(const rex::graphics::NativeGuestOutputRenderContext& context, void*);

// The registry is in the runtime-loaded GPU plugin: find its export among
// the loaded libraries once the plugin is up (first swap).
bool RegisterPresenter() {
  using SetRenderer = void (*)(rex::graphics::NativeGuestOutputRenderer, void*);
  SetRenderer set = nullptr;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        if (!info->dlpi_name || !std::strstr(info->dlpi_name, "rexgpu")) return 0;
        if (void* handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD)) {
          *static_cast<SetRenderer*>(data) =
              reinterpret_cast<SetRenderer>(dlsym(handle, "rex_gpu_set_native_guest_output_renderer"));
          dlclose(handle);
        }
        return *static_cast<SetRenderer*>(data) ? 1 : 0;
      },
      &set);
  if (!set) {
    REXLOG_ERROR("native_gpu: the GPU plugin has no native guest output entry point");
    return false;
  }
  set(&PresentNativeFrame, nullptr);
  REXLOG_INFO("native_gpu: presenting native frames");
  return true;
}

bool PresentNativeFrame(const rex::graphics::NativeGuestOutputRenderContext& context, void*) {
  namespace nrhi = rex::graphics::nrhi;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame.empty() || !context.guest_output) return false;
  const uint32_t width = std::min(g_frame_width, context.guest_output->width());
  const uint32_t height = std::min(g_frame_height, context.guest_output->height());
  const uint32_t row_pitch = (width * 4 + nrhi::kRowPitchAlignment - 1) & ~(nrhi::kRowPitchAlignment - 1);
  const uint64_t size = uint64_t(row_pitch) * height;
  if (g_upload_size < size) {
    if (g_upload) context.device->DestroyDeferred(g_upload);
    nrhi::BufferDesc desc;
    desc.size = size;
    desc.heap = nrhi::HeapKind::kUpload;
    desc.bind_class = nrhi::BufferBindClass::kCopySrc;
    g_upload = context.device->CreateBuffer(desc);
    g_upload_size = size;
  }
  auto* dst = static_cast<uint8_t*>(context.device->Map(g_upload));
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + size_t(y) * row_pitch, &g_frame[size_t(y) * g_frame_width], width * 4);
  }
  context.device->Unmap(g_upload);
  const nrhi::ResourceState initial = g_written_outputs.insert(context.guest_output).second
                                          ? nrhi::ResourceState::kCommon
                                          : nrhi::ResourceState::kGuestOutput;
  context.cmd->Barrier(context.guest_output, initial, nrhi::ResourceState::kCopyDest);
  context.cmd->FlushBarriers();
  context.cmd->CopyBufferToTexture(context.guest_output, 0, 0, g_upload, 0, row_pitch, width, height, 1);
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kCopyDest, nrhi::ResourceState::kGuestOutput);
  context.cmd->FlushBarriers();
  return true;
}
uint32_t g_inline_vertices = 0;
uint32_t g_inline_primitive = 0;
uint32_t g_inline_count = 0;
uint32_t g_inline_stride = 0;

bool ReadRect(uint8_t* base, uint32_t address, int32_t rect[4]) {
  if (!address) return false;
  for (int i = 0; i < 4; ++i) rect[i] = int32_t(REX_LOAD_U32(address + 4 * i));
  return true;
}

void ReadColor(uint8_t* base, uint32_t address, float color[4]) {
  for (int c = 0; c < 4 && address; ++c) {
    const uint32_t bits = REX_LOAD_U32(address + 4 * c);
    std::memcpy(&color[c], &bits, 4);
  }
}

void WritePpm(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  // Resolved 8_8_8_8 textures are in memory order (blue first).
  for (size_t p = 0; p < size_t(width) * height; ++p) {
    const uint8_t rgb[3] = {rgba[p * 4 + 2], rgba[p * 4 + 1], rgba[p * 4]};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}

}  // namespace

void RendererThread();

void StartLiveRenderer(plume::RenderDevice* device, rex::memory::Memory* memory) {
  if (!REXCVAR_GET(native_gpu_render)) return;
  g_memory = memory;
  g_watcher = std::make_unique<PhysicalWriteWatcher>(memory);
  g_renderer = std::make_unique<native_gpu::Renderer>();
  if (!g_renderer->Initialize(device, memory->TranslatePhysical(0), FindShaderContainer, g_watcher.get())) {
    REXLOG_ERROR("native_gpu: renderer initialization failed");
    g_renderer.reset();
    return;
  }
  std::thread(RendererThread).detach();
  REXLOG_INFO("native_gpu: live rendering on{}", REXCVAR_GET(native_gpu_present) ? ", presenting" : "");
}

// Time spent in the renderer on the game's render thread, for the stats line.
std::chrono::steady_clock::duration g_renderer_time{};

// The frame's calls, rendered at Swap: on the console the GPU only reads
// vertices, indices and textures when it runs the frame, and the game may
// fill them after issuing a draw (the skater's cloth). Each call keeps a copy
// of the device state it was issued with (registers and constants are set
// at that point).
constexpr uint32_t kStateSize = 0x3400;
struct DeferredCall {
  enum class Kind { kDraw, kClear, kResolve } kind;
  size_t state;  // offset in the frame's arena
  native_gpu::DrawCall draw;
  native_gpu::ClearCall clear;
  native_gpu::ResolveCall resolve;
  size_t inline_vertices = SIZE_MAX;  // copied into the arena (the command buffer is reused)
  // Copied too: D3D patches it for each vertex layout during the frame.
  size_t vs_physical = SIZE_MAX;
};
// A recorded frame, rendered by the renderer thread.
struct Frame {
  std::vector<DeferredCall> calls;
  std::vector<uint8_t> arena;
  uint32_t front_buffer = 0;
  bool dump = false;
};
Frame g_recording;

size_t SnapshotState(uint8_t* base) {
  std::vector<uint8_t>& arena = g_recording.arena;
  const size_t offset = arena.size();
  arena.insert(arena.end(), base + kDevice, base + kDevice + kStateSize);
  return offset;
}

void RenderDeferredCalls(Frame& frame) {
  for (DeferredCall& call : frame.calls) {
    const xenos::DeviceState state{frame.arena.data() + call.state};
    switch (call.kind) {
      case DeferredCall::Kind::kDraw:
        call.draw.state = state;
        if (call.inline_vertices != SIZE_MAX) {
          call.draw.inline_vertices = frame.arena.data() + call.inline_vertices;
        }
        if (call.vs_physical != SIZE_MAX) call.draw.vs_physical = frame.arena.data() + call.vs_physical;
        g_renderer->Draw(call.draw);
        break;
      case DeferredCall::Kind::kClear:
        call.clear.state = state;
        g_renderer->Clear(call.clear);
        break;
      case DeferredCall::Kind::kResolve:
        call.resolve.state = state;
        g_renderer->Resolve(call.resolve);
        break;
    }
  }
}

// The renderer thread: renders, submits and reads back each recorded frame
// while the game records the next one (as the console's GPU runs a frame
// behind the CPU). One frame is handed over at a time.
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::optional<Frame> g_queued;
bool g_busy = false;
std::chrono::steady_clock::duration g_thread_time{};
bool g_present = false;

void RenderFrame(Frame& frame) {
  const auto start = std::chrono::steady_clock::now();
  g_renderer->BeginFrame();
  RenderDeferredCalls(frame);
  g_renderer->EndFrame(g_present ? frame.front_buffer : 0);
  if (const uint32_t* pixels = g_present ? g_renderer->presented() : nullptr) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame_width = g_renderer->presented_width();
    g_frame_height = g_renderer->presented_height();
    g_frame.assign(pixels, pixels + size_t(g_frame_width) * g_frame_height);
  }
  if (frame.dump) {
    g_renderer->WaitIdle();
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    if (g_renderer->ReadResolved(frame.front_buffer, rgba, width, height)) {
      WritePpm(REXCVAR_GET(native_gpu_dump_path), rgba, width, height);
      REXLOG_INFO("native_gpu: front buffer {:08X} written to {}", frame.front_buffer,
                  REXCVAR_GET(native_gpu_dump_path));
    }
    for (const auto& [what, count] : g_renderer->stats()) {
      REXLOG_INFO("native_gpu: {} {}", count, what);
    }
  }
  g_thread_time += std::chrono::steady_clock::now() - start;
}

// Logs what the renderer could not do, first time and when it grows tenfold,
// so a player's log tells which draws went missing.
void LogRendererIssues() {
  static std::map<std::string, uint32_t> logged;
  for (const auto& [what, count] : g_renderer->stats()) {
    if (what.find("skipped") == std::string::npos && what.find("vertex element") == std::string::npos &&
        what.find("heap full") == std::string::npos) {
      continue;
    }
    uint32_t& last = logged[what];
    if (count >= last * 10 + 1) {
      last = count;
      REXLOG_WARN("native_gpu: {} ({}x)", what, count);
    }
  }
}

void RendererThread() {
  for (;;) {
    Frame frame;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return g_queued.has_value(); });
      frame = std::move(*g_queued);
      g_queued.reset();
      g_busy = true;
    }
    RenderFrame(frame);
    static uint32_t rendered = 0;
    if (++rendered % 300 == 0) LogRendererIssues();
    {
      std::lock_guard<std::mutex> lock(g_queue_mutex);
      g_busy = false;
    }
    g_queue_cv.notify_all();
  }
}


void LiveRendererOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base) {
  if (!g_renderer) return;
  const auto start = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point start;
    ~Timer() { g_renderer_time += std::chrono::steady_clock::now() - start; }
  } timer{start};
  const xenos::DeviceState state{base + kDevice};
  switch (function) {
    case 0x82CB29D8:  // BeginVertices(device, prim, count, stride) -> vertex pointer
      g_inline_vertices = result;
      g_inline_primitive = before.r4.u32;
      g_inline_count = before.r5.u32;
      g_inline_stride = before.r6.u32;
      break;
    case 0x82CB38E8:  // DrawIndexedVertices(device, prim, base vertex, start index, count)
    case 0x82CB3500:  // DrawVertices(device, prim, start vertex, count)
    case 0x82CB2E78: {  // EndVertices(device)
      native_gpu::DrawCall call;
      call.state = state;
      const uint32_t vs = REX_LOAD_U32(kDevice + xenos::device::kVertexShader);
      const uint32_t ps = REX_LOAD_U32(kDevice + xenos::device::kPixelShader);
      const ShaderObject vertex = FindShaderObject(vs, false);
      const ShaderObject pixel = FindShaderObject(ps, true);
      call.vs_key = vertex.entry ? vertex.entry->hash : 0;
      call.ps_key = pixel.entry ? pixel.entry->hash : 0;
      if (vs) {
        call.vs_physical = base + REX_LOAD_U32(vs + 32);
        call.vs_physical_size = vertex.physical_size;
      }
      if (function == 0x82CB2E78) {
        if (!g_inline_vertices) break;
        call.primitive = g_inline_primitive;
        call.count = g_inline_count;
        call.inline_vertices = base + g_inline_vertices;
        call.inline_size = g_inline_count * g_inline_stride;
        g_inline_vertices = 0;
      } else if (function == 0x82CB38E8) {
        call.primitive = before.r4.u32;
        call.base_vertex = before.r5.u32;
        call.start = before.r6.u32;
        call.count = before.r7.u32;
        // Index buffer object: +0 bit 31 = 32-bit indices, +24 data, +28 size.
        const uint32_t indices = REX_LOAD_U32(kDevice + xenos::device::kIndexBuffer);
        if (!indices) break;
        call.indices_32bit = REX_LOAD_U32(indices) >> 31;
        call.indices_address = g_memory->GetPhysicalAddress(REX_LOAD_U32(indices + 24));
        call.indices_size = REX_LOAD_U32(indices + 28);
      } else {
        call.primitive = before.r4.u32;
        call.start = before.r5.u32;
        call.count = before.r6.u32;
      }
      {
        DeferredCall deferred{DeferredCall::Kind::kDraw, SnapshotState(base), call, {}, {}};
        if (call.inline_vertices) {
          std::vector<uint8_t>& arena = g_recording.arena;
          deferred.inline_vertices = arena.size();
          arena.insert(arena.end(), call.inline_vertices, call.inline_vertices + call.inline_size);
        }
        const auto copy = [](const uint8_t* data, size_t size) {
          std::vector<uint8_t>& arena = g_recording.arena;
          const size_t offset = arena.size();
          arena.insert(arena.end(), data, data + size);
          return offset;
        };
        if (call.vs_physical) deferred.vs_physical = copy(call.vs_physical, call.vs_physical_size);
        g_recording.calls.push_back(deferred);
      }
      break;
    }
    case 0x82CC7510: {  // ClearF(device, flags, rect, color)
      native_gpu::ClearCall call;
      call.state = state;
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      ReadColor(base, before.r6.u32, call.color);
      call.depth = float(before.f1.f64);
      call.stencil = before.r7.u32;
      g_recording.calls.push_back({DeferredCall::Kind::kClear, SnapshotState(base), {}, call, {}});
      break;
    }
    case 0x82CC4FF0: {  // BeginTiling(device, flags, count, tile rects, clear color, ...)
      // Rendered untiled: one clear of all the tiles.
      if (!before.r6.u32 || !before.r5.u32) break;
      native_gpu::ClearCall call;
      call.state = state;
      call.flags = 0x30;
      call.stencil = before.r8.u32;
      call.has_rect = true;
      call.rect[0] = call.rect[1] = INT32_MAX;
      call.rect[2] = call.rect[3] = INT32_MIN;
      for (uint32_t t = 0; t < before.r5.u32; ++t) {
        int32_t rect[4];
        ReadRect(base, before.r6.u32 + 16 * t, rect);
        for (int c = 0; c < 4; ++c) {
          call.rect[c] = c < 2 ? std::min(call.rect[c], rect[c]) : std::max(call.rect[c], rect[c]);
        }
      }
      if (before.r7.u32) {
        call.flags |= 0xF;
        ReadColor(base, before.r7.u32, call.color);
      }
      call.depth = float(before.f1.f64);  // ClearZ
      g_recording.calls.push_back({DeferredCall::Kind::kClear, SnapshotState(base), {}, call, {}});
      break;
    }
    case 0x82CC7FF0: {  // Resolve(device, flags, source rect, texture, dest point, ...)
      native_gpu::ResolveCall call;
      call.state = state;
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      if (before.r7.u32) {
        call.has_dest_point = true;
        call.dest_x = int32_t(REX_LOAD_U32(before.r7.u32));
        call.dest_y = int32_t(REX_LOAD_U32(before.r7.u32 + 4));
      }
      call.clear_depth = float(before.f1.f64);  // ClearZ
      g_recording.calls.push_back({DeferredCall::Kind::kResolve, SnapshotState(base), {}, {}, call});
      if (!(call.flags & 4)) g_recording.front_buffer = state.Reg(0x2319);  // RB_COPY_DEST_BASE
      break;
    }
    default:
      break;
  }
}

void LiveRendererOnSwap(uint8_t* base) {
  (void)base;
  if (!g_renderer) return;
  const auto swap_start = std::chrono::steady_clock::now();
  struct Timer {
    std::chrono::steady_clock::time_point start;
    ~Timer() { g_renderer_time += std::chrono::steady_clock::now() - start; }
  } timer{swap_start};
  // Periodic stats: frames per second, average frame time and the renderer's
  // share of it on the render thread.
  static auto stats_start = swap_start;
  static uint32_t stats_frames = 0;
  ++stats_frames;
  const int32_t stats_seconds = REXCVAR_GET(native_gpu_stats_seconds);
  const auto elapsed = swap_start - stats_start;
  if (stats_seconds > 0 && elapsed >= std::chrono::seconds(stats_seconds)) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double renderer_ms = std::chrono::duration<double, std::milli>(g_renderer_time).count();
    const double thread_ms = std::chrono::duration<double, std::milli>(g_thread_time).count();
    REXLOG_INFO("native_gpu: {:.1f} FPS, {:.1f} ms/frame, {:.1f} ms/frame on the render thread, "
                "{:.1f} ms/frame on the renderer thread",
                stats_frames / seconds, 1000.0 * seconds / stats_frames, renderer_ms / stats_frames,
                thread_ms / stats_frames);
    g_thread_time = {};
    stats_start = swap_start;
    stats_frames = 0;
    g_renderer_time = {};
  }
  static const bool registered = REXCVAR_GET(native_gpu_present) && RegisterPresenter();
  g_present = registered && REXCVAR_GET(native_gpu_present);
  static const auto first_frame = std::chrono::steady_clock::now();
  static bool dumped = false;
  const int32_t seconds = REXCVAR_GET(native_gpu_dump_seconds);
  const std::string trigger = REXCVAR_GET(native_gpu_dump_path) + ".trigger";
  std::error_code error;
  const bool triggered = std::filesystem::remove(trigger, error);
  if (triggered || (!dumped && seconds > 0 &&
                    std::chrono::steady_clock::now() - first_frame >= std::chrono::seconds(seconds))) {
    dumped = true;
    g_recording.dump = true;
  }
  const size_t recorded_calls = g_recording.calls.size();
  const size_t recorded_bytes = g_recording.arena.size();
  // Hand the frame over once the renderer thread has taken the previous one
  // (at most one frame queued behind the one being rendered).
  {
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    g_queue_cv.wait(lock, [] { return !g_queued.has_value(); });
    g_queued = std::move(g_recording);
  }
  g_queue_cv.notify_all();
  g_recording = Frame{};
  // Frames look alike: growing the arena from nothing copies it again and again.
  g_recording.calls.reserve(recorded_calls + recorded_calls / 4);
  g_recording.arena.reserve(recorded_bytes + recorded_bytes / 4);
}
