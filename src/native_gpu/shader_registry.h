// skate1 - native D3D layer: shader registry (see docs/native_d3d.md)

#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

struct ShaderCacheEntry;

void StartShaderRegistry(rex::memory::Memory* memory);

struct ShaderObject {
  // The recompiled shader, or nullptr if the cache does not have it.
  const ShaderCacheEntry* entry = nullptr;
  // Size of the physical part (literal constants, then the microcode).
  uint32_t physical_size = 0;
};

// A shader object the game passed to SetPixelShader / SetVertexShader.
ShaderObject FindShaderObject(uint32_t object, bool pixel);

// The container of a cached shader (big-endian, virtual then physical part),
// or nullptr if no object with that shader was created.
const uint8_t* FindShaderContainer(uint64_t key);
