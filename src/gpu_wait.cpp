// skate1 - GPU wait without burning a host core
//
// The game's D3D waits for the GPU (ring buffer space, fences) in
// sub_82CB11C0, which polls sub_82CB4AA0 until the command processor has read
// far enough. On the console that poll is a short spin; recompiled, it keeps
// the render thread at 100% of a host core for as long as the GPU is behind.
// On a handheld the CPU and GPU share one power budget, so that core is taken
// from the GPU and the battery. Keep spinning through short waits, where a
// late wakeup would cost frame time, and sleep briefly between the polls once
// a wait gets long.
//
// Off by default: on the Steam Deck any sleep here dropped the game to 20-24 FPS
// with choppy audio (likely the CPU governor clocking down once the render
// thread idles), although it ran cooler. Set gpu_wait_sleep_us to try it.

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_INT32(gpu_wait_sleep_us, 0, "Skate1",
                     "Host sleep in microseconds between the game's polls once a wait for the "
                     "GPU gets long (0 = spin like the console)")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(gpu_wait_spin_us, 2000, "Skate1",
                     "Microseconds a wait for the GPU spins before it starts sleeping")
    .range(0, 100000);

namespace {
// Polls further apart than this belong to separate waits (a sleeping wait
// polls every gpu_wait_sleep_us plus the wakeup latency).
constexpr auto kNewWaitGap = std::chrono::milliseconds(1);
}  // namespace

REX_HOOK_RAW(sub_82CB4AA0) {
  __imp__sub_82CB4AA0(ctx, base);
  // r3 = 1: the GPU has not caught up yet and the caller polls again.
  if (ctx.r3.u32 == 0) {
    return;
  }
  int32_t sleep_us = REXCVAR_GET(gpu_wait_sleep_us);
  if (sleep_us <= 0) {
    return;
  }
  thread_local auto wait_start = std::chrono::steady_clock::time_point();
  thread_local auto last_poll = std::chrono::steady_clock::time_point();
  auto now = std::chrono::steady_clock::now();
  if (now - last_poll > kNewWaitGap) {
    wait_start = now;
  }
  last_poll = now;
  if (now - wait_start >= std::chrono::microseconds(REXCVAR_GET(gpu_wait_spin_us))) {
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    last_poll = std::chrono::steady_clock::now();
  }
}
