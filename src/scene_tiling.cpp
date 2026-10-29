// skate1 - single-tile scene rendering
//
// Skate renders its main scene with 4x MSAA. At 720p that does not fit the
// Xbox 360's 10 MB of EDRAM, so the scene uses predicated tiling: D3D replays
// the whole scene's command buffers once per 1280x256 band, three times per
// frame, and the GPU emulation thread translates every scene draw three times
// - most of its work in free skate.
//
// The scene view (+120 of the renderer) keeps two surface sets: +472
// single-sampled (one tile, table at +36) and +480 4x MSAA (three tiles, table
// at +324). Before a scene pass (sub_82736238) the view is switched to the
// MSAA set, sample count (+28) 4, and back to the single-sampled one after
// (sub_8273D3F8). scene_msaa_off keeps the scene on the single-sampled set,
// so the engine renders it in one pass through its own untiled path. Edges are then
// aliased, which a host-side anti-aliasing pass can make up for.

#include <rex/cvar.h>
#include <rex/hook.h>

#include "generated/default/skate1_init.h"

REXCVAR_DEFINE_BOOL(scene_msaa_off, false, "Skate1",
                    "Render the 3D scene single-sampled in one pass instead of with 4x MSAA in "
                    "three predicated tiles (about a third of the GPU emulation work)");

// Every scene pass begins in sub_82736238(view); switch a view set up for 4x
// MSAA back to its single-sampled surfaces there, whichever path set it up.
REX_HOOK_RAW(sub_82736238) {
  const uint32_t view = ctx.r3.u32;
  if (REXCVAR_GET(scene_msaa_off) && view && REX_LOAD_U32(view + 28) == 4) {
    REX_STORE_U32(view + 28, 1);
    REX_STORE_U32(view + 32, view + 36);
    REX_STORE_U32(view + 468, REX_LOAD_U32(view + 472));
  }
  __imp__sub_82736238(ctx, base);
}
