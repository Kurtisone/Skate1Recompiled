#!/usr/bin/env bash
# Native Linux build of skate1 for the Steam Deck (SteamOS).
#
# Runs the build inside the Ubuntu 22.04 container from ./Containerfile so the
# binaries need nothing newer than glibc 2.35, then assembles a folder to copy
# to the Deck. Needs podman (or docker via CONTAINER=docker) and the extracted
# game in game/ (tools/extract_iso.py).
#
#   tools/steamdeck/build.sh            # build and package into out/steamdeck/skate1
#   tools/steamdeck/build.sh --sdk-only # build the SDK and tool only (no game files needed)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTAINER="${CONTAINER:-podman}"
IMAGE="${IMAGE:-skate1-deck-build}"

if [[ "${1:-}" != "--in-container" ]]; then
  if ! "$CONTAINER" image exists "$IMAGE" 2>/dev/null; then
    "$CONTAINER" build -t "$IMAGE" -f "$ROOT/tools/steamdeck/Containerfile" "$ROOT/tools/steamdeck"
  fi
  exec "$CONTAINER" run --rm --userns=keep-id -v "$ROOT:/src:Z" -w /src "$IMAGE" \
    bash tools/steamdeck/build.sh --in-container "$@"
fi
shift
SDK_ONLY=0
[[ "${1:-}" == "--sdk-only" ]] && SDK_ONLY=1

# The Deck's Zen 2 CPU has AVX2, so x86-64-v3 is safe there.
ARCH_FLAGS="-march=x86-64-v3"
SDK_BUILD=out/steamdeck/sdk
GAME_BUILD=out/steamdeck/build
PACKAGE=out/steamdeck/skate1

cmake -S third_party/rexglue-sdk -B "$SDK_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  "-DCMAKE_C_FLAGS=$ARCH_FLAGS" "-DCMAKE_CXX_FLAGS=$ARCH_FLAGS" \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build "$SDK_BUILD" --target rexglue rexgpu-xenos
[[ $SDK_ONLY == 1 ]] && { echo "SDK built in $SDK_BUILD"; exit 0; }

if [[ ! -f game/default.xex ]]; then
  echo "game/default.xex not found: extract your ISO first (tools/extract_iso.py)" >&2
  exit 1
fi

# The project cannot be configured before codegen has written
# generated/rexglue.cmake, so the first build runs it by hand. After that the
# build reruns codegen itself whenever its inputs (including the tool) change,
# and running it here too would do the 30+ minute analysis twice.
if [[ ! -f generated/rexglue.cmake ]]; then
  REXGLUE="$(ls third_party/rexglue-sdk/out/linux-amd64/rexglue* | grep -v '\.so' | head -1)"
  LD_LIBRARY_PATH="$(dirname "$REXGLUE")" "$REXGLUE" --force codegen skate1_manifest.toml
fi

cmake -S . -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  "-DCMAKE_C_FLAGS=$ARCH_FLAGS" "-DCMAKE_CXX_FLAGS=$ARCH_FLAGS" \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build "$GAME_BUILD" --target skate1

rm -rf "$PACKAGE"
mkdir -p "$PACKAGE"
cp "$GAME_BUILD/skate1" "$PACKAGE/"
cp "$GAME_BUILD"/librexruntime*.so "$GAME_BUILD"/librexgpu-xenos*.so "$PACKAGE/"
# Desktop settings, then the Deck overrides (later keys win in a TOML table,
# so drop the overridden ones first).
python3 - config/skate1.runtime.toml tools/steamdeck/deck-overrides.toml "$PACKAGE/skate1.toml" <<'PY'
import re, sys
base, over, out = sys.argv[1:4]
key = lambda l: re.match(r'\s*([A-Za-z0-9_]+)\s*=', l)
over_lines = open(over).read().splitlines()
over_keys = {key(l).group(1) for l in over_lines if key(l)}
kept = [l for l in open(base).read().splitlines() if not (key(l) and key(l).group(1) in over_keys)]
open(out, 'w').write('\n'.join(kept + [''] + over_lines) + '\n')
PY
cat > "$PACKAGE/README.txt" <<'TXT'
skate1, native Linux build for the Steam Deck

1. Copy this folder to the Deck, and put the extracted game/ folder inside it
   (next to the skate1 binary).
2. In Desktop Mode, Steam > Add a Non-Steam Game > browse to skate1.
   Do not enable Proton for it: this is a native Linux build.
3. Settings live in skate1.toml next to the binary.
TXT
echo "Package ready: $PACKAGE"
