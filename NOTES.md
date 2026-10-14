# Skate 1 PC port (ReXGlue recompilation)

Same approach as `Desktop\Skate2Port\skate2`: statically recompile the Xbox 360
`default.xex` to C++ with ReXGlue, using the SDK fork from the Skate 3 recomp
(`Skate2Port\skate2recomp\third_party\rexglue-sdk`, already built there). The
SDK is shared, not copied: `CMakePresets.json` points `REXSDK_DIR` at it and the
tools call its `rexglue.exe`.

Game files are never shipped or downloaded. `game/` is extracted from the
user's own `Skate. (USA).iso` with `tools/extract_iso.py`.

## Layout

| Path | What |
|---|---|
| `game/` | extracted disc (default.xex, data/, $SystemUpdate) |
| `skate1_manifest.toml` | rexglue project manifest |
| `config/skate1_functions.toml` | setjmp/longjmp + function boundary overrides |
| `generated/` | codegen output (rexglue codegen skate1_manifest.toml) |
| `src/` | app shell (`skate1_app.h`, `main.cpp`) |
| `tools/` | Skate 2 helper scripts retargeted to Skate 1 |
| `work/xexdump/` | decrypted image from `rexglue dump-xex` |

## Workflow (same as Skate 2)

```
tools\extract_iso.py "..\Skate. (USA).iso" game        # once
rexglue codegen skate1_manifest.toml                    # ~9 min
..\..\Skate2Port\devenv.bat cmake --preset win-amd64-relwithdebinfo
..\..\Skate2Port\devenv.bat cmake --build out/build/win-amd64-relwithdebinfo
tools\iterate.ps1                                       # regen+build+run loop
```

Missed functions: after a build, run `tools/scan_data_ptrs.py` and
`tools/scan_code_ptrs.py` against `work/xexdump/*.bin` and
`generated/default/skate1_init.cpp`, then add `START` hits to the functions toml.

## Skate 1 vs Skate 2

| | Skate 1 | Skate 2 |
|---|---|---|
| XEX image | 0x01250000 bytes | 0x00F10000 bytes |
| .text | 821F0000-8305994C (14.9 MB) | 82310000-82D09958 (10.0 MB) |
| Entry | 0x82DF9450 | 0x82C70C00 |
| setjmp / longjmp | 0x82E50F20 / 0x82E48B60 | 0x82CDE7C0 / 0x82C85590 |
| Movies | 31 VP6 + 21 jpg thumbs | 40 VP6 |
| Audio | .big, .mpf/.mus, .hla, .ems | .big/.bh, .mpf/.mus, .snr/.sek, .ems |
| Extra dirs | data/jobs (empty on disc) | data/upload |

- Same EA engine lineage: Lua scripting, VP6 movies (`.XBMOVIE` section), MPF
  music, BIG archives. Video and audio playback should go through the same
  SDK paths that already work for Skate 2.
- setjmp/longjmp in Skate 1 are byte-identical to Skate 2's CRT routines.
- First codegen pass hit 11 unresolved calls (Skate 2 hit similar ones):
  3 missed tail-call targets and 4 shared tail blocks placed just before
  the function that branches back into them. Fixed in the functions toml.
- Skate 1's `.text` is ~50% larger, so codegen takes ~9 min (Skate 2: ~1 min).
- Codegen has no fall-through between functions, so the 4 shared tail blocks
  are declared as a parent chunk (+0) plus a standalone copy (+4).
- `src/skate1_app.h` must use `skate1_PPCImageConfig` (the init template's
  `PPCImageConfig` does not compile).
- Runtime config `skate1.toml` sits next to the exe and sets `game_data_root`;
  a copy is kept in `config/skate1.runtime.toml`.
- Building skate1 relinks `rexglue.exe` / `rexruntimerd.dll` into the shared
  SDK output under Skate2Port (same sources, so harmless for Skate 2).
- Missed functions: Skate 2's scans were curated by hand. For Skate 1 the rule
  is: data-pointer `START` hits (opcode != 0), plus code-pointer hits whose
  first instruction touches no r14-r31 register.

## Boot log

1. XEX loads, crashes at once on missed callback 0x83024D40 (data pointer).
2. After adding scan results: opens audio (.csi/.evt/.big) and data probes,
   then crashes on 0x829725A0 (lis/addi callback, filtered out by the first rule).
3. Wider code-pointer rule: reaches fonts / input.cfg, but 23 false positives
   split real functions (codegen REX_FATAL stubs). Removed them.
4. Zero stubs. Reaches particle loading, crashes on 0x8242B3E0 (a lone blr).
5. Reaches the title screen ("Press START to continue"), no crash.
   `work/snaps/run5_20s.png`.

Next: press Start (tools/press_start.ps1, mnk_mode is on) and follow the
crashes into the front end, then free skate, as with Skate 2.

## Goal checklist (videos, campaign missions, audio)

- [ ] codegen clean
- [ ] builds and boots to EA intro movie (VP6 video)
- [ ] front end / menus
- [ ] in-game free skate
- [ ] audio (music MPF, SFX, speech)
- [ ] career / missions (challenge flow, save data)

## Fixes after first play test

- Green block corruption in the intro videos: a ReXGlue codegen bug, not a Skate 1
  issue. `vpkuwus` / `vpkuhus` wrote vD element by element while still reading
  vA/vB, so `vpkuwus v0,v12,v0` (every use in Skate 1, in the VP6 decoder) read
  its own half-written output. Fixed in
  `Skate2Port/skate2recomp/third_party/rexglue-sdk/src/codegen/builders/vector.cpp`
  by copying the sources first. Skate 2/3 only use the `128` forms with distinct
  registers, which is why their videos were fine.
- Input delay: trying `d3d12_frame_latency_waitable_object = true` with
  `d3d12_max_frame_latency = 1` in skate1.toml (caps queued swapchain frames).
  The original game ran at 30 FPS on 360, so some delay is native.
- Menu input delay (Skate 1 only): with `vsync = false` the runtime fires guest
  vblank interrupts at 1000 Hz and skips ring-buffer vblank waits, so Skate 1 ran
  its loop uncapped (~170 guest FPS on the overlay). Skate 2 paces itself (~53).
  `vsync = true` gives real 60 Hz vblanks; Skate 1 then runs at 60.
  Ruled out: XamInputGetKeystrokeEx is only polled with the keyboard/chatpad flag.
- Clothing flicker in direct sun while moving (shadow acne, Skate 1 only):
  fixed by `depth_float24_convert_in_pixel_shader = true` in skate1.toml.

## Career / audio check (2026-10-09)

- New Career boots; the career intro FMV (ambulance scene) plays clean.
- Menu music plays (track overlay shows "Booker T & The MG's - Green Onions").
- No audio/XMA errors in the log. All ~690 failed file opens are loose-file
  probes (d:\*.abk, *.csi, ...) for data that exists inside the .big archives
  (tools/missing_check.py). Only two truly absent: data\AllowMovieSkip.txt and
  data\NoAds.txt, both optional dev flag files.
- Created game/data/AllowMovieSkip.txt so movies can be skipped.
- Alternating-frame flicker (scene brightness and edges swap every other frame):
  fixed by `clear_memory_page_state = true` (refresh GPU-written page state at
  frame end; Skate 1 reuses previous-frame render results).
- Dark strip under the board: matches original Xbox 360 footage (Simon checked),
  so it is the game's own contact shadow, not a port bug.

## Performance pass (2026-10-11)

- `REXGLUE_ENABLE_PERF_COUNTERS` is now OFF in the presets. A profile showed
  `rex::perf::IncrementCounter` (called from `DeferredCommandList::Execute`)
  taking ~6% CPU; with counters off the slow spots run a steady 60 FPS.
- That exposed a race behind the alternating-frame flicker:
  `SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten` double-buffers the
  page-valid flags, but when 1-16 blocks were dirty it copied only this frame's
  dirty blocks into staging. Staging is two swaps stale, so it missed the
  previous frame's blocks and the two buffers diverged every other frame. Slower
  frames dirtied more than 16 blocks and took the full-copy path, hiding it. The
  SDK patch now always copies the whole table (~16 KB per frame).
- Tested at draw_resolution_scale 1x and 2x: no flicker, no stutter.
- Remaining flicker on the shirt, board and feet (shadow acne, 2026-10-14): at 2x
  scale it came back on the body even with `depth_float24_convert_in_pixel_shader`;
  at 1x the body was clean but the board and feet still flickered sometimes.
  Adding `depth_float24_round = true` cleared it at 1x (2x not retested yet).

## Portable test build

`dist/Skate1-PC-Test.zip` (no game files): skate1.exe, rexruntimerd.dll, app-local
VC++ runtime DLLs, skate1.toml (relative `game_data_root = 'game'`), portable.txt,
Setup.bat + tools/extract_iso.ps1 (PowerShell XDVDFS extractor, no Python), README.
`src/skate1_app.h` OnConfigurePaths resolves a relative/missing game_data_root
next to the exe and, with portable.txt, keeps saves in `userdata\`.
Tested from a clean folder launched with a different working directory.
