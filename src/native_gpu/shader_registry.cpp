// skate1 - native D3D layer: shader registry (see docs/native_d3d.md)
//
// Skate builds its shader objects in place with XGSetVertexShaderHeader-like
// helpers, sub_82CC1038 (vertex) and sub_82CC0EB0 (pixel) (object, size,
// &info, info[0] = the shader container). At that point the container is
// intact and its microcode is the one the archives hold (D3D later patches
// the vertex fetches in the copy the GPU runs), so each object is keyed there
// by the XXH3 of the microcode (shader->size bytes at virtualSize +
// physicalOffset, XenosRecomp's XENOS_RECOMP_MICROCODE_KEY) and remembered by
// address for SetVertexShader / SetPixelShader. Some vertex shaders are
// derived at runtime from others (sub_82B0E1F0); native_gpu_shader_miss_dir
// collects the containers the cache misses so they can be added to it.

#include "shader_registry.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "d3d_trace.h"
#include "generated/default/skate1_init.h"
#include "shader_cache.h"

REXCVAR_DEFINE_BOOL(native_gpu_shader_log, false, "Skate1",
                    "Log each shader object the game sets and whether the shader cache has it");
REXCVAR_DEFINE_STRING(native_gpu_shader_miss_dir, "", "Skate1",
                      "Write the shader containers the shader cache misses here "
                      "(tools/d3d/build_shader_cache.sh takes them)");

namespace {

rex::memory::Memory* g_memory = nullptr;

std::mutex g_mutex;
std::unordered_map<uint32_t, ShaderObject> g_objects;
// Containers by cache key (vertex elements and original microcode for the
// renderer), copied at creation since the game frees them afterwards.
std::unordered_map<uint64_t, std::vector<uint8_t>> g_containers;
uint32_t g_misses = 0;

const ShaderCacheEntry* FindEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry* entry = std::lower_bound(
      begin, end, hash,
      [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return entry != end && entry->hash == hash ? entry : nullptr;
}

void OnCreate(uint8_t* base, uint32_t object, uint32_t container) {
  if (!g_memory || !object || !container ||
      (REX_LOAD_U32(container) & 0xFFFFFFFE) != 0x102A1100) {
    return;
  }
  const bool pixel = !(REX_LOAD_U32(container) & 1);
  const uint32_t virtual_size = REX_LOAD_U32(container + 4);
  const uint32_t shader = container + REX_LOAD_U32(container + 0x18);
  const uint32_t size = REX_LOAD_U32(shader + 4);
  const uint64_t hash = XXH3_64bits(base + container + virtual_size + REX_LOAD_U32(shader), size);
  const ShaderCacheEntry* entry = FindEntry(hash);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_objects[object] = {entry, REX_LOAD_U32(container + 8)};
  if (entry && !g_containers.count(hash)) {
    // Virtual and physical parts (the renderer reads the original vertex
    // fetches' destination registers from the microcode).
    g_containers.emplace(hash, std::vector<uint8_t>(base + container, base + container + virtual_size +
                                                                          REX_LOAD_U32(container + 8)));
  }
  if (entry) {
    if (REXCVAR_GET(native_gpu_shader_log)) {
      REXLOG_INFO("native_gpu: {} shader {:016X} object {:08X}", pixel ? "pixel" : "vertex",
                  hash, object);
    }
    return;
  }
  if (g_misses++ < 64) {
    REXLOG_WARN("native_gpu: {} shader {:016X} not in the shader cache (object {:08X})",
                pixel ? "pixel" : "vertex", hash, object);
  }
  const std::string dir = REXCVAR_GET(native_gpu_shader_miss_dir);
  if (!dir.empty()) {
    const std::string path = fmt::format("{}/{:016X}_{}.bin", dir, hash, pixel ? "ps" : "vs");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(base + container, 1, virtual_size + REX_LOAD_U32(container + 8), f);
      std::fclose(f);
    }
  }
}

ShaderObject OnSet(uint32_t object, bool pixel) {
  if (!g_memory || !object) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  auto [it, inserted] = g_objects.try_emplace(object);
  if (inserted) {
    REXLOG_WARN("native_gpu: {} shader object {:08X} was not created through the hooked "
                "helpers", pixel ? "pixel" : "vertex", object);
  }
  return it->second;
}

}  // namespace

void StartShaderRegistry(rex::memory::Memory* memory) {
  g_memory = memory;
  REXLOG_INFO("native_gpu: shader cache has {} shaders", g_shaderCacheEntryCount);
}

ShaderObject FindShaderObject(uint32_t object, bool pixel) { return OnSet(object, pixel); }

const uint8_t* FindShaderContainer(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_containers.find(key);
  return it == g_containers.end() ? nullptr : it->second.data();
}

// Shader object helpers (object, size, &info), info[0] = container.
REX_HOOK_RAW(sub_82CC1038) {
  D3DTraceEnter(kD3DTraceCreateVertexShaderIndex, ctx, base);
  const uint32_t object = ctx.r3.u32;
  const uint32_t container = REX_LOAD_U32(ctx.r5.u32);
  __imp__sub_82CC1038(ctx, base);
  OnCreate(base, object, container);
  D3DTraceLeave(kD3DTraceCreateVertexShaderIndex);
}

REX_HOOK_RAW(sub_82CC0EB0) {
  D3DTraceEnter(kD3DTraceCreatePixelShaderIndex, ctx, base);
  const uint32_t object = ctx.r3.u32;
  const uint32_t container = REX_LOAD_U32(ctx.r5.u32);
  __imp__sub_82CC0EB0(ctx, base);
  OnCreate(base, object, container);
  D3DTraceLeave(kD3DTraceCreatePixelShaderIndex);
}

// D3DDevice_SetPixelShader(device, object)
REX_HOOK_RAW(sub_82CBDFE8) {
  D3DTraceEnter(kD3DTraceSetPixelShaderIndex, ctx, base);
  OnSet(ctx.r4.u32, true);
  __imp__sub_82CBDFE8(ctx, base);
  D3DTraceLeave(kD3DTraceSetPixelShaderIndex);
}

// D3DDevice_SetVertexShader(device, object, ...)
REX_HOOK_RAW(sub_82CBE2E8) {
  D3DTraceEnter(kD3DTraceSetVertexShaderIndex, ctx, base);
  OnSet(ctx.r4.u32, false);
  __imp__sub_82CBE2E8(ctx, base);
  D3DTraceLeave(kD3DTraceSetVertexShaderIndex);
}
