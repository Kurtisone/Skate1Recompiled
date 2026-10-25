// skate1 - GPU wait without burning a host core
//
// The game's D3D waits for the GPU (ring buffer space, fences) in
// sub_82CB11C0, which polls sub_82CB4AA0 until the command processor has read
// far enough. On the console that poll is a short spin; recompiled, it keeps
// the render thread at 100% of a host core for as long as the GPU is behind.
// On a handheld the CPU and GPU share one power budget, so that core is taken
// from the GPU and the battery. Sleep briefly between the polls instead.

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_INT32(gpu_wait_sleep_us, 100, "Skate1",
                     "Host sleep in microseconds between the game's polls while it waits for "
                     "the GPU (0 = spin like the console)")
    .range(0, 2000);

REX_HOOK_RAW(sub_82CB4AA0) {
  __imp__sub_82CB4AA0(ctx, base);
  // r3 = 1: the GPU has not caught up yet and the caller polls again.
  if (ctx.r3.u32 == 0) {
    return;
  }
  int32_t sleep_us = REXCVAR_GET(gpu_wait_sleep_us);
  if (sleep_us > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
}
