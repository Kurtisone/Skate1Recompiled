// skate1 - XDK graphics library call tracing (see tools/gen_d3d_trace.py)

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

extern const uint32_t kD3DTraceAddresses[];
extern const uint32_t kD3DTraceCount;
extern const uint32_t kD3DTraceSwapIndex;

void D3DTraceEnter(uint32_t index, const PPCContext& ctx);
void D3DTraceLeave(uint32_t index);
