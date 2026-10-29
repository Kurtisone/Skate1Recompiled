// skate1 - XDK graphics library call tracing (see tools/gen_d3d_trace.py)

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

extern const uint32_t kD3DTraceAddresses[];
extern const uint32_t kD3DTraceCount;
extern const uint32_t kD3DTraceSwapIndex;
extern const uint32_t kD3DTraceSetPixelShaderIndex;
extern const uint32_t kD3DTraceSetVertexShaderIndex;
extern const uint32_t kD3DTraceCreatePixelShaderIndex;
extern const uint32_t kD3DTraceCreateVertexShaderIndex;

void D3DTraceEnter(uint32_t index, const PPCContext& ctx, uint8_t* base);
void D3DTraceLeave(uint32_t index);
