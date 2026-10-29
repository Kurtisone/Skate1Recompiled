// skate1 - native D3D layer: one-frame capture (see frame_capture.cpp)

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace rex::memory {
class Memory;
}

void StartFrameCapture(rex::memory::Memory* memory);

// Called from the Swap hook (src/frame_cap.cpp) before D3D's Swap runs.
void FrameCaptureOnSwap(const PPCContext& ctx, uint8_t* base);
