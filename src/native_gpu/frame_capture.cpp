// skate1 - native D3D layer: one-frame capture (see docs/native_d3d.md)
//
// native_gpu_capture_seconds = N records the frame that starts N seconds
// after the first Swap into native_gpu_capture_path: every draw, clear,
// resolve, render target, viewport and tiling call with its arguments, the
// D3D device shadow (registers, constants, fetch
// constants, bound shaders), the shaders' cache keys, the guest memory the
// pointer arguments point at (all taken after the call), the inline vertices of BeginVertices /
// EndVertices, and at the end the whole physical memory (<path>.phys, sparse)
// for vertex/index buffers and textures. tools/d3d/capture.py reads it; the
// native renderer is developed against it offline (milestone B).
//
// Format (host little-endian): "SK1C", version, device address, snapshot
// size; then per event "EVNT", function address, r3..r10, result, lr,
// vertex and pixel shader keys, blob count, the device snapshot, and each
// blob as address, size, bytes; "DONE" at the end.

#include "frame_capture.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "d3d_trace.h"
#include "generated/default/skate1_init.h"
#include "shader_cache.h"
#include "shader_registry.h"

REXCVAR_DEFINE_INT32(native_gpu_capture_seconds, 0, "Skate1",
                     "Capture the frame that starts this many seconds after the first Swap "
                     "(0 = off)");
REXCVAR_DEFINE_STRING(native_gpu_capture_path, "frame.sk1c", "Skate1",
                      "Frame capture file (physical memory goes to <path>.phys)");

namespace {

constexpr uint32_t kDevice = 0x40002200;
constexpr uint32_t kSnapshotSize = 0x3400;  // through the bound shaders at +12684/+12688
constexpr uint32_t kPointerBlobSize = 256;
constexpr uint32_t kPhysicalSize = 0x20000000;

rex::memory::Memory* g_memory = nullptr;

enum class State { kIdle, kCapturing, kDone };
State g_state = State::kIdle;
std::mutex g_mutex;
FILE* g_file = nullptr;
uint32_t g_events = 0;
// BeginVertices' result and size, for the blob at EndVertices.
uint32_t g_inline_vertices = 0;
uint32_t g_inline_size = 0;

struct Blob {
  uint32_t address;
  uint32_t size;
};

void Write(const void* data, size_t size) { std::fwrite(data, 1, size, g_file); }
void Write32(uint32_t value) { Write(&value, 4); }

uint64_t ShaderKey(uint8_t* base, uint32_t device_offset, bool pixel) {
  const ShaderCacheEntry* entry = FindShaderForObject(REX_LOAD_U32(kDevice + device_offset), pixel);
  return entry ? entry->hash : 0;
}

bool IsPointer(uint32_t address) { return address >= 0x40000000 && address < 0xA0000000; }

void RecordEvent(uint32_t function, const PPCContext& ctx, uint8_t* base, uint32_t result,
                 const std::vector<Blob>& extra) {
  std::vector<Blob> blobs = extra;
  const uint32_t args[] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32};
  for (uint32_t arg : args) {
    if (IsPointer(arg)) {
      blobs.push_back({arg, kPointerBlobSize});
    }
  }
  Write("EVNT", 4);
  Write32(function);
  const uint32_t regs[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                           ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  Write(regs, sizeof(regs));
  Write32(result);
  Write32(uint32_t(ctx.lr));
  const uint64_t vs = ShaderKey(base, 12688, false);
  const uint64_t ps = ShaderKey(base, 12684, true);
  Write(&vs, 8);
  Write(&ps, 8);
  Write32(uint32_t(blobs.size()));
  Write(base + kDevice, kSnapshotSize);
  for (const Blob& blob : blobs) {
    Write32(blob.address);
    Write32(blob.size);
    Write(base + blob.address, blob.size);
  }
  ++g_events;
}

void WritePhysical(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("native_gpu: cannot write {}", path);
    return;
  }
  // Sparse: skip untouched (all-zero) 64 KB blocks.
  constexpr size_t kBlock = 0x10000;
  static const std::vector<uint8_t> zero(kBlock);
  const uint8_t* physical = g_memory->TranslatePhysical(0);
  for (size_t offset = 0; offset < kPhysicalSize; offset += kBlock) {
    if (std::memcmp(physical + offset, zero.data(), kBlock)) {
      std::fseek(f, long(offset), SEEK_SET);
      std::fwrite(physical + offset, 1, kBlock, f);
    }
  }
  std::fflush(f);
  if (ftruncate(fileno(f), kPhysicalSize)) {
    REXLOG_WARN("native_gpu: cannot size {}", path);
  }
  std::fclose(f);
}

}  // namespace

void StartFrameCapture(rex::memory::Memory* memory) { g_memory = memory; }

void FrameCaptureOnSwap(const PPCContext& ctx, uint8_t* base) {
  const int32_t seconds = REXCVAR_GET(native_gpu_capture_seconds);
  if (!g_memory || seconds <= 0) {
    return;
  }
  static const auto first_swap = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state == State::kIdle &&
      std::chrono::steady_clock::now() - first_swap >= std::chrono::seconds(seconds)) {
    const std::string path = REXCVAR_GET(native_gpu_capture_path);
    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) {
      REXLOG_ERROR("native_gpu: cannot write {}", path);
      g_state = State::kDone;
      return;
    }
    Write("SK1C", 4);
    Write32(1);
    Write32(kDevice);
    Write32(kSnapshotSize);
    g_state = State::kCapturing;
    REXLOG_INFO("native_gpu: capturing a frame into {}", path);
  } else if (g_state == State::kCapturing) {
    RecordEvent(0x82CCBE98, ctx, base, 0, {});
    Write("DONE", 4);
    std::fclose(g_file);
    g_file = nullptr;
    const std::string path = REXCVAR_GET(native_gpu_capture_path);
    WritePhysical(path + ".phys");
    g_state = State::kDone;
    REXLOG_INFO("native_gpu: captured {} events into {}", g_events, path);
  }
}

namespace {

// Runs one of the hooked calls and records it, with the device shadow as the
// call left it and its result (BeginVertices' vertex pointer).
void Capture(uint32_t function, uint32_t trace_index, PPCContext& ctx, uint8_t* base,
             void (*original)(PPCContext&, uint8_t*)) {
  D3DTraceEnter(trace_index, ctx, base);
  const PPCContext before = ctx;
  original(ctx, base);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state == State::kCapturing) {
    std::vector<Blob> blobs;
    if (function == 0x82CB29D8) {  // BeginVertices(device, prim, count, stride)
      g_inline_vertices = ctx.r3.u32;
      g_inline_size = before.r5.u32 * before.r6.u32;
    } else if (function == 0x82CB2E78 && g_inline_vertices) {  // EndVertices
      blobs.push_back({g_inline_vertices, g_inline_size});
      g_inline_vertices = 0;
    }
    RecordEvent(function, before, base, ctx.r3.u32, blobs);
  }
  D3DTraceLeave(trace_index);
}

}  // namespace

#define CAPTURE_HOOK(address, name)                                                    \
  REX_HOOK_RAW(sub_##address) {                                                        \
    Capture(0x##address, kD3DTrace##name##Index, ctx, base, __imp__sub_##address);     \
  }

CAPTURE_HOOK(82CB38E8, DrawIndexedVertices)
CAPTURE_HOOK(82CB3500, DrawVertices)
CAPTURE_HOOK(82CB29D8, BeginVertices)
CAPTURE_HOOK(82CB2E78, EndVertices)
CAPTURE_HOOK(82CC7510, ClearF)
CAPTURE_HOOK(82CC7FF0, Resolve)
CAPTURE_HOOK(82CBA6B0, SetRenderTarget)
CAPTURE_HOOK(82CBA070, SetDepthStencilSurface)
CAPTURE_HOOK(82CC4FF0, BeginTiling)
CAPTURE_HOOK(82CC5578, EndTiling)
CAPTURE_HOOK(82CC4E78, SetPredication)
CAPTURE_HOOK(82CB9FB8, SetViewport)
CAPTURE_HOOK(82CB9270, Sub_82CB9270)
