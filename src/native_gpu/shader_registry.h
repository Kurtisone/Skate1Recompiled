// skate1 - native D3D layer: shader registry (see docs/native_d3d.md)

#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

struct ShaderCacheEntry;

void StartShaderRegistry(rex::memory::Memory* memory);

// The recompiled shader for a shader object the game passed to
// SetPixelShader / SetVertexShader, or nullptr if the cache does not have it.
const ShaderCacheEntry* FindShaderForObject(uint32_t object, bool pixel);
