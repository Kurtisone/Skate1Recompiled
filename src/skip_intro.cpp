// skate1 - skip the "EA HD" bumper at startup
//
// At boot the game plays the "EA HD" bumper (movie id 0) and then the skate/EA
// intro (id 1) through sub_82468430(player, id). Each frame sub_82468330 ends
// the playing movie once the movie system's decoder busy flag (byte 504) is
// clear, and the movie screen's update (sub_82362A10) then moves on. Clear
// that flag while the bumper plays, so it ends at once the natural way.
//
// The intro is left alone: skipping it, by this or by posting the screen's
// skip event (what the button handler does when data\AllowMovieSkip.txt
// exists), leaves the title screen never taking Start.

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_BOOL(skip_intro, true, "Skate1", "Skip the EA HD bumper movie at startup");

namespace {
constexpr uint32_t kMovieEaHdBumper = 0;
// Pointer to the movie system; its decoder status object is at +84.
constexpr uint32_t kMovieSystemPointerAddress = 0x831AB1C0;
// Set while the bumper plays, until the movie screen sees the movie end.
bool skip_pending = false;
}  // namespace

// Movie player: start the movie with the given id.
REX_HOOK_RAW(sub_82468430) {
  const uint32_t movie_id = ctx.r4.u32;
  __imp__sub_82468430(ctx, base);
  skip_pending = REXCVAR_GET(skip_intro) && movie_id == kMovieEaHdBumper;
}

// Movie screen: per-frame update.
REX_HOOK_RAW(sub_82362A10) {
  if (skip_pending) {
    const uint32_t movie_system = REX_LOAD_U32(kMovieSystemPointerAddress);
    const uint32_t decoder_status = movie_system ? REX_LOAD_U32(movie_system + 84) : 0;
    if (decoder_status) {
      REX_STORE_U8(decoder_status + 504, 0);
    } else {
      skip_pending = false;
    }
  }
  __imp__sub_82362A10(ctx, base);
}
