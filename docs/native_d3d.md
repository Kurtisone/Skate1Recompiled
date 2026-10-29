# Native D3D layer (plan)

Goal: replace the game's statically linked Xbox 360 D3D (XDK 2.0.5632) with a
native Vulkan implementation, as Unleashed Recompiled does, instead of
emulating the Xenos GPU (PM4 command processor). Per-draw cost then becomes a
few state lookups by object identity instead of decoding registers, fetch
constants and EDRAM state, which also means less CPU and GPU load (heat and
battery on the Steam Deck). The emulated path stays as the fallback until the
native one covers everything.

## What the game uses (tools/gen_d3d_trace.py, free skate, per frame)

- ~95 D3D entry points on the render thread.
- SetTexture `82CCB4D0` (~1500), draw `82CB38E8` (~640, flushes dirty state
  from the device shadow and emits DRAW), its setters `82CB95A8`,
  `82CB9400`, `82CBE2E8` (~650 each), ~60 per-state render state setters
  (42-111 each, called from `82B12CB0`..`82B131A4`), Resolve `82CC7FF0` (29),
  BeginTiling `82CC4FF0` (1), Swap `82CCBE98` (1).
- Device at guest `0x40002200`: 64-bit dirty masks at +0..+40, vertex
  constants at +1920, pixel constants at +6016, render state registers
  around +10528..+10560, D3D rings set up by `82CB1FD8`.
- No draw packets are written outside D3D, so the layer can intercept at
  function level.

## Progress

- Step 1 done (2026-10-29): the game hands D3D its shader containers
  (magic `0x102A1100` VS / `0x102A1101` PS) through `82CBDF20`, `82CBFEC0`,
  `82CC4470`; `d3d_trace_shader_args` + `d3d_trace_shader_dir` dump them.
  247 shaders (boot to free skate at MJC) all convert with XenosRecomp and
  compile to SPIR-V/DXIL once raw constant registers are handled (branch
  `skate1` of a local XenosRecomp clone next to this repo, one commit).

## Decision (2026-10-29): port Unleashed Recompiled's renderer

The user chose to reuse Unleashed Recompiled's D3D replacement
(hedge-dev/UnleashedRecomp, `UnleashedRecomp/gpu/video.cpp`, GPL-3.0) on
plume (renderbag/plume, MIT) with XenosRecomp shaders. Consequence:
Skate1Recompiled is distributed under GPL-3.0 once that code is included.

Why it fits: Skate and Sonic Unleashed link the same XDK D3D, and the device
layout Unleashed declares (`GuestDevice`: dirty flags at +0, render state
function table at +0x40 (0x65 entries), sampler state functions, sampler
fetch constants at +0x480 (32 x 24 bytes), float constants at +0x780 /
+0x1780, bool constants at +0x2780) matches what Skate's draw flush reads.
Unleashed replaces the device's render state function table instead of
hooking each setter, so Skate's ~60 per-state setters come with it.

Porting plan:
1. Vendor plume and the D3D core of video.cpp (resources, states, draws,
   pipeline/vertex declaration caches, resolves, present) into
   `src/native_gpu/`, without Unleashed's UI, config, installer and
   Sonic-specific patches.
2. Compat layer for rexglue: guest memory (`g_memory` -> `base`), hooks
   (`GUEST_FUNCTION_HOOK` -> `REX_HOOK`), physical heap allocations, guest
   threads, vblank/swap interrupts the game waits on.
3. Map the ~42 hooked functions to Skate's addresses (docs/d3d_functions.md)
   and add Skate's own needs: BeginTiling/EndTiling and predication
   (render once), Resolve, BeginVertices/EndVertices, ClearF.
4. Presentation: plume swapchain on the rexglue SDL window while the native
   path runs (rexglue's GPU plugin not loaded then); keep rexglue's GPU
   plugin as the fallback path with a cvar.
5. Shader cache: XenosRecomp (branch `skate1`) over the dumped containers,
   then over all shaders in the game's archives.

## Finding (2026-10-29): Skate creates no resources through D3D

A trace from boot through loading shows no CreateTexture,
CreateVertexBuffer, Lock or Unlock: like other EA/RenderWare games, Skate
fills its texture and buffer headers itself and passes them straight to
SetTexture / SetStreamSource. Unleashed's resource half (GuestTexture etc.
created by hooked CreateTexture, uploads at Unlock) does not apply; textures
and buffers would have to be tracked by guest address with write detection,
as the emulated path does. The native layer's gain is then mostly the
per-draw translation cost, which scene_msaa_off already cut by ~3x.

## Design (2026-10-29)

D3D keeps the whole draw state in its device shadow (registers in Xenos
layout, float constants, fetch constants) and only turns dirty parts into
PM4 in the draw (`82CB38E8`, flush helper `82CCD390`: dirty bit k of a group
is register base+k at device offset base+4k, MSB first; command buffer write
pointer at device+48). So the game's own setters stay as they are (cheap
shadow writes) and the native layer:

- hooks the draw and reads the shadow directly to build the host draw
  (pipeline key from the render state registers, XenosRecomp constants from
  device+1920 / +6016 byte-swapped, bools, fetch constants);
- hooks the few calls that give object identity: SetTexture, stream source,
  indices, shaders, buffer/texture Lock/Unlock (explicit uploads, no page
  watching), render targets, Clear, Resolve, tiling, Swap;
- does not emit PM4 for draws it handles, so the GPU emulation thread only
  sees what is left.

Shadow offset -> register groups (from the draw flush): +1920 c0 VS, +6016
c256 PS, +10112 0x4900 bools, +10368 0x2000, +10444 0x2100, +10528 0x2180,
+10548 0x2200, +10596 0x2280, +10680 0x2300, +10832 0x2380.
`tools/d3d/name_setters.py <d3d_trace.txt>` lists the registers each traced
entry point writes.

## Steps

1. Shaders: capture the shader containers the game creates and check
   XenosRecomp (hedge-dev, MIT) can convert them; build an offline cache.
2. Name every D3D entry point the game calls (trace + disassembly), with
   argument layouts.
3. Native device: state tracking for render states, samplers, textures,
   streams, indices, constants, shaders; draws on a native Vulkan renderer.
4. Render targets, clears, resolves (the EDRAM model: what the game expects
   resolve to produce), predicated tiling reduced to one pass.
5. Textures and buffers: creation, lock/unlock (explicit, no page watching),
   untiling at upload.
6. Swap into the presenter; switch between native and emulated paths with a
   cvar; compare screenshots on the headless benchmark (out/pc-bench).
