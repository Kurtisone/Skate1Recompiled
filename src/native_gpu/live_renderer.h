// skate1 - native D3D layer: live rendering (see live_renderer.cpp)

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace plume {
struct RenderDevice;
}
namespace rex::memory {
class Memory;
}

void StartLiveRenderer(plume::RenderDevice* device, rex::memory::Memory* memory);

// A hooked D3D call has run: `before` holds its arguments, `result` its r3,
// `packets_start` the command buffer write pointer before the call.
void LiveRendererOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base,
                        uint32_t packets_start);

// Called from the Swap hook before D3D's Swap runs.
void LiveRendererOnSwap(uint8_t* base);
