#!/usr/bin/env bash
# Build the native D3D layer's shader cache (out/native/shader_cache.cpp) from
# the game's own files with XenosRecomp (branch skate1, microcode keys).
#   tools/d3d/build_shader_cache.sh [image shader dir]
# The archives are harvested once (~25 min, tools/d3d/harvest_shaders.py).
# The post-processing shaders are in the executable instead: run the game once
# with d3d_trace_shader_args = true and d3d_trace_shader_dir = <dir> and pass
# that dir (its image_*.bin files are used).
# XENOS_RECOMP (default ~/Projects/XenosRecomp) must have been built into
# XENOS_RECOMP_BUILD (default ~/Projects/XenosRecomp-build).
set -euo pipefail
cd "$(dirname "$0")/../.."
XR="${XENOS_RECOMP:-$HOME/Projects/XenosRecomp}"
XRB="${XENOS_RECOMP_BUILD:-$HOME/Projects/XenosRecomp-build}"
OUT=out/native
if [ ! -d "$OUT/allshaders" ]; then
  python3 tools/d3d/harvest_shaders.py game/data "$OUT/allshaders"
fi
rm -rf "$OUT/cacheinput"
mkdir -p "$OUT/cacheinput"
cp "$OUT"/allshaders/*.bin "$OUT/cacheinput/"
if [ -n "${1:-}" ]; then
  cp "$1"/image_*.bin "$OUT/cacheinput/"
fi
XENOS_RECOMP_MICROCODE_KEY=1 XENOS_RECOMP_NO_ZSTD=1 LD_LIBRARY_PATH="$XR/thirdparty/dxc-bin/lib/x64" \
  "$XRB/XenosRecomp/XenosRecomp" "$OUT/cacheinput" "$OUT/shader_cache.cpp" \
  "$XR/XenosRecomp/shader_common.h"
echo "$(grep -c '^	{ 0x' "$OUT/shader_cache.cpp") shaders in $OUT/shader_cache.cpp"
