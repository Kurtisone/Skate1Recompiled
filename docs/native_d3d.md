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
