# Skate 1 D3D entry points (XDK 2.0.5632)

Device: guest `0x40002200` (always r3 for device methods). Calls per frame
measured in free skate at MJC (tools/gen_d3d_trace.py). Confidence:
**sure** = behaviour checked in the disassembly, *likely* = arguments and
written state fit, ? = guess.

| Address | Name | Args (after device) | /frame | Evidence |
|---|---|---|---|---|
| 82CB38E8 | DrawIndexedVertices | prim type (4 = tri list), base vertex, start index, index count | 640 | **sure**: flushes dirty shadow (82CCD390/82CCD730) and emits the draw |
| 82CB3500 | DrawVertices | prim type, start vertex, vertex count | 25 | *likely*: same flush, (6, 0, 4) = a full-screen quad |
| 82CB29D8 / 82CB2E78 | BeginVertices / EndVertices | prim, count, stride / data | 25 | *likely*: inline vertex data in the command buffer, End moves the write pointer (+48) |
| 82CB95A8 | SetIndices | index buffer | 640 | **sure**: stores device+12428, resource fence tracking |
| 82CB9400 | SetStreamSource | stream, buffer, offset, stride | 660 | **sure**: writes the vertex fetch constant, buffer at +12452+4n, stride byte at +12520+n |
| 82CCB4D0 | SetTexture | sampler, texture, (0x80000000 >> sampler) | 1500 | *likely*: per-sampler mask, many call sites |
| 82CBE2E8 | SetVertexShader | shader, ... | 680 | *likely*: device+12688, called with the stream/index setters |
| 82CBDFE8 | SetPixelShader | shader, ... | 82 | *likely*: device+12684 |
| 82CB99C0 | SetSamplerState (group) | sampler, value, ... | 250 | ? |
| 82CB8880..82CB9060 | SetSamplerState_* (14 functions) | sampler, value | 112 | *likely*: write the fetch constant dirty group (+24) |
| 82CB7508..82CB7798 | SetRenderState_* depth/stencil (13) | value | 86 | **sure** register: RB_DEPTHCONTROL fields |
| 82CB87A0..82CB8800 | SetRenderState_* Hi-Z (3) | value | 86 | register RB_HIZCONTROL |
| 82CB6DA0, 82CB72E0, 82CB85E8, 82CB8618 | SetRenderState_* blend/alpha | value | 63 | register RB_COLORCONTROL |
| 82CB7AA0..82CB7B60 | SetRenderState_ColorWriteEnable0-3 | value | 63 | register RB_COLOR_MASK |
| 82CB81F0..82CB8400 | SetRenderState_* (RT0-3 info) | value | 63 | registers RB_COLOR_INFO .. RB_COLOR3_INFO |
| 82CB6D40, 82CB6D70, 82CB78E0, 82CB79A8, 82CB7A50, 82CB8590 | SetRenderState_* raster (cull, fill, ...) | value | 43 | register PA_SU_SC_MODE_CNTL |
| 82CB7A80 | SetRenderState_MultiSampleMask | value | 43 | PA_SC_AA_MASK |
| 82CB80A0 | SetRenderState_* (clip/viewport transform) | value | 43 | PA_CL_CLIP_CNTL, PA_CL_VTE_CNTL |
| 82CB8558 | SetRenderState_* (pixel center) | value | 43 | PA_SU_VTX_CNTL |
| 82CB85C0 | SetRenderState_* (index restart value) | 0xFFFF | 25 | VGT_MULTI_PRIM_IB_RESET_INDX |
| 82CBA6B0 | SetRenderTarget | index, surface | 26 | *likely*: RB_COLOR*_INFO, RB_DEPTH_INFO, color masks |
| 82CBA070 | SetDepthStencilSurface ? | 0, surface | 17 | ? |
| 82CB9FB8 | SetViewport | viewport struct | 33 | *likely*: engine passes x, y, w, h, min/max z |
| 82CB9270 | ? (r3 = 0x4000537C, not the device) | | 159 | writes registers 0x2011/0x2012; called around every pass |
| 82CC7510 | ClearF | flags (0x10), rect, color | 23 | *likely*: called at untiled pass begin |
| 82CC4FF0 | BeginTiling | flags, count, tile rects, ... | 1 | **sure** (count 3, 1280x256 bands) |
| 82CC5578 | EndTiling | ... | 1 | *likely*: reaches the resolve |
| 82CC4E78 | SetPredication ? | tile mask (3, 0xC, 0x30) | 4 | ? 2 bits per tile |
| 82CC7FF0 | Resolve | ... | 29 | **sure** register: RB_COPY_* |
| 82CCBE98 | Swap | front buffer texture, ... | 1 | **sure**: reads the fetch constant at texture+28 |
| 82CBDF20, 82CBFEC0, 82CC4470 | shader container users | (container as r3) | load | **sure**: shader containers (magic 0x102A110x) |
| 82CB60D0, 82CB5F60, 82CB5E60, 82CB6078, 82CB60C0 | resource lock/unlock family | resource first | 9-14 | ? |

Objects: vertex/index buffer +24 address, +28 size; texture fetch constant
(6 dwords) at texture+28.

Shaders (probed 2026-10-29):
- Container magic: `0x102A1100` = pixel shader, `0x102A1101` = vertex shader
  (XenosRecomp: `isPixelShader = (flags & 1) == 0`; the dump file names in
  out/pc-bench/shaders have them the wrong way round).
- `82CBDF20(container, &out)` creates in place and returns the container;
  `82CBFEC0(container, &part, &size)` splits it into parts (XG);
  `82CC4470(container, other, size, &out)` returns 0 (pairs shaders?).
- SetPixelShader `82CBDFE8(device, object)`: object type 7, microcode
  physical address at +24, the shader's container right after the object
  header at +40.
- SetVertexShader `82CBE2E8(device, object, n, ?, microcode physical)`:
  object type 6, microcode at +32, container pointer at +60.
- Correction (registry, 2026-10-29): `82CBDF20(container, &info)` fills
  info = {container, object size (header 40 pixel / 872 vertex + virtual
  size), physical part, physical size}; the game's loader `82B0C700` copies
  the physical part, then `82CC1038` (vertex) / `82CC0EB0` (pixel)
  `(object, size, &info)` zero the header and copy the virtual part after
  it. +24 / +32 is the physical part's address (microcode at physicalOffset
  in it); the vertex copy is patched by D3D (vfetch format/stride/offset).
  +60 of a vertex object is not its container. `Shader.size` is in bytes.
- Cache key: XXH3 of the microcode taken from the container at creation
  (src/native_gpu/shader_registry.cpp).
