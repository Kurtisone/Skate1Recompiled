// skate1 - frame rate cap
//
// Skate ran at 30 FPS on the Xbox 360, but its frame pacing lets it present
// every vblank when a frame is quick enough, so with headroom (a single-pass
// scene, scene_msaa_off) it runs well above 30 and the game, the GPU
// emulation thread and the GPU all do up to twice the work per second. On a
// handheld that is heat and battery for nothing visible. frame_rate_cap
// sleeps the render thread in D3D Swap (sub_82CCBE98) until the cap's frame
// time has passed since the previous one.

#include <algorithm>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "d3d_trace.h"
#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_INT32(frame_rate_cap, 0, "Skate1",
                     "Maximum frames per second (0 = uncapped; Skate ran at 30 on the Xbox 360)")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_HOOK_RAW(sub_82CCBE98) {
  const int32_t cap = REXCVAR_GET(frame_rate_cap);
  thread_local std::chrono::steady_clock::time_point next_frame;
  if (cap > 0) {
    const auto frame_time =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / cap));
    auto now = std::chrono::steady_clock::now();
    if (now < next_frame) {
      // Sleep most of the way, then spin the last stretch for an even cadence.
      constexpr auto kSpin = std::chrono::microseconds(500);
      if (next_frame - now > kSpin) {
        std::this_thread::sleep_for(next_frame - now - kSpin);
      }
      while (std::chrono::steady_clock::now() < next_frame) {
      }
      now = next_frame;
    }
    // Late frames do not build up a debt of quick ones.
    next_frame = std::max(next_frame + frame_time, now + frame_time - frame_time / 4);
  }
  D3DTraceEnter(kD3DTraceSwapIndex, ctx, base);
  __imp__sub_82CCBE98(ctx, base);
  D3DTraceLeave(kD3DTraceSwapIndex);
}
