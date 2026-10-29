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
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
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

  void Watch(uint32_t address, uint32_t size) override {
    ForPages(address, size, [&](uint32_t word, uint64_t mask) {
      dirty_[word].fetch_and(~mask, std::memory_order_relaxed);
    });
    memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  }

  bool Dirty(uint32_t address, uint32_t size) override {
    bool dirty = false;
    ForPages(address, size, [&](uint32_t word, uint64_t mask) {
      dirty |= (dirty_[word].load(std::memory_order_relaxed) & mask) != 0;
    });
    return dirty;
  }

 private:
  static constexpr uint32_t kPages = 0x20000000 >> 12;

  template <typename F>
  static void ForPages(uint32_t address, uint32_t size, F&& f) {
    if (!size) return;
    const uint32_t first = (address & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((address & 0x1FFFFFFF) + size - 1) >> 12);
    for (uint32_t page = first; page <= last;) {
      const uint32_t word = page >> 6, bit = page & 63;
      const uint32_t count = std::min(64 - bit, last - page + 1);
      f(word, (count == 64 ? ~0ull : ((1ull << count) - 1)) << bit);
      page += count;
    }
  }

  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<PhysicalWriteWatcher*>(context);
    ForPages(start, length, [&](uint32_t word, uint64_t mask) {
      self->dirty_[word].fetch_or(mask, std::memory_order_relaxed);
    });
    return {0, UINT32_MAX};
  }

  rex::memory::Memory* memory_;
  void* handle_ = nullptr;
  std::array<std::atomic<uint64_t>, kPages / 64> dirty_{};
};
std::unique_ptr<PhysicalWriteWatcher> g_watcher;
uint32_t g_front_buffer = 0;
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
  for (size_t p = 0; p < size_t(width) * height; ++p) std::fwrite(&rgba[p * 4], 1, 3, f);
  std::fclose(f);
}

}  // namespace

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
  g_renderer->BeginFrame();
  REXLOG_INFO("native_gpu: live rendering on");
}

void LiveRendererOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base) {
  if (!g_renderer) return;
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
      g_renderer->Draw(call);
      break;
    }
    case 0x82CC7510: {  // ClearF(device, flags, rect, color)
      native_gpu::ClearCall call;
      call.state = state;
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      ReadColor(base, before.r6.u32, call.color);
      g_renderer->Clear(call);
      break;
    }
    case 0x82CC4FF0: {  // BeginTiling(device, flags, count, tile rects, clear color, ...)
      // Rendered untiled: one clear of all the tiles.
      if (!before.r6.u32 || !before.r5.u32) break;
      native_gpu::ClearCall call;
      call.state = state;
      call.flags = 0x30;
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
      g_renderer->Clear(call);
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
      g_renderer->Resolve(call);
      if (!(call.flags & 4)) g_front_buffer = state.Reg(0x2319);  // RB_COPY_DEST_BASE
      break;
    }
    default:
      break;
  }
}

void LiveRendererOnSwap(uint8_t* base) {
  (void)base;
  if (!g_renderer) return;
  g_renderer->EndFrame();
  static const auto first_frame = std::chrono::steady_clock::now();
  static bool dumped = false;
  const int32_t seconds = REXCVAR_GET(native_gpu_dump_seconds);
  const std::string trigger = REXCVAR_GET(native_gpu_dump_path) + ".trigger";
  std::error_code error;
  const bool triggered = std::filesystem::remove(trigger, error);
  if (triggered || (!dumped && seconds > 0 &&
                    std::chrono::steady_clock::now() - first_frame >= std::chrono::seconds(seconds))) {
    dumped = true;
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    if (g_renderer->ReadResolved(g_front_buffer, rgba, width, height)) {
      WritePpm(REXCVAR_GET(native_gpu_dump_path), rgba, width, height);
      REXLOG_INFO("native_gpu: front buffer {:08X} written to {}", g_front_buffer,
                  REXCVAR_GET(native_gpu_dump_path));
    }
    for (const auto& [what, count] : g_renderer->stats()) {
      REXLOG_INFO("native_gpu: {} {}", count, what);
    }
  }
  g_renderer->BeginFrame();
}
