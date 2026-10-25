#!/usr/bin/env bash
# Android (arm64) APK of skate1, for handhelds such as the AYN Odin 2.
#
# Needs: Android NDK r29 ($ANDROID_NDK), the Android SDK with platform 35 and
# build-tools 35.0.0 ($ANDROID_SDK), a JDK, and generated/ from a desktop
# build (this never runs codegen). The game files are not in the APK: copy
# game/ and skate1.toml to the device as out/android/package/README.txt says.
#
#   tools/android/build.sh   ->  out/android/package/skate1.apk
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/android-ndk-r29}"
ANDROID_SDK="${ANDROID_SDK:-$HOME/Android/sdk}"
BUILD_TOOLS="$ANDROID_SDK/build-tools/35.0.0"
PLATFORM_JAR="$ANDROID_SDK/platforms/android-35/android.jar"
LLVM="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
SDL_PROJECT=third_party/rexglue-sdk/thirdparty/sdl3/android-project
BUILD=out/android/game
WORK=out/android/apk
PACKAGE=out/android/package
KEYSTORE="${KEYSTORE:-$HOME/Android/skate1-debug.keystore}"
# ARMv8.2 (Snapdragon 845 and later): LSE atomics instead of LL/SC loops, for
# the runtime's locks and the recompiled code's atomics.
ARCH_FLAGS="-march=armv8.2-a"

if [[ ! -f generated/default/sources.cmake ]]; then
  echo "generated/ is missing: run the desktop build (codegen) first" >&2
  exit 1
fi

# 1. Native libraries.
cmake -S tools/android -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
  "-DCMAKE_C_FLAGS=$ARCH_FLAGS" "-DCMAKE_CXX_FLAGS=$ARCH_FLAGS" \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
cmake --build "$BUILD" --target main

rm -rf "$WORK" && mkdir -p "$WORK/lib/arm64-v8a" "$WORK/classes" "$WORK/res"
LIBS="$WORK/lib/arm64-v8a"
cp "$BUILD/libmain.so" "$LIBS/"
# The SDK writes the runtime into its own out/<platform> folder; the build
# stages the GPU plugin next to libmain.so.
cp third_party/rexglue-sdk/out/linux-arm64/librexruntime.so "$LIBS/"
cp "$BUILD/librexgpu-xenos.so" "$LIBS/"
cp "$LLVM/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$LIBS/"
# adrenotools' hooks, loaded from the native library folder.
cp "$BUILD"/adrenotools/src/hook/*.so "$LIBS/"
"$LLVM/bin/llvm-strip" --strip-unneeded "$LIBS"/*.so

# 2. Java: SDL's activity and helpers, plus ours.
javac -source 11 -target 11 -nowarn -encoding UTF-8 -cp "$PLATFORM_JAR" -d "$WORK/classes" \
  $(find "$SDL_PROJECT/app/src/main/java" tools/android/java -name '*.java')
"$BUILD_TOOLS/d8" --release --min-api 29 --lib "$PLATFORM_JAR" --output "$WORK" \
  $(find "$WORK/classes" -name '*.class')

# 3. Resources (SDL's, with ours on top) and the manifest.
"$BUILD_TOOLS/aapt2" compile --dir "$SDL_PROJECT/app/src/main/res" -o "$WORK/res/sdl.zip"
"$BUILD_TOOLS/aapt2" compile --dir tools/android/res -o "$WORK/res/skate1.zip"
# Launcher icon: the game's own title icon, read from the user's XEX by the
# desktop rexglue tool (it is the game's art, so it is never committed).
# Without the tool the APK keeps SDL's icon.
ICON_OVERLAY=()
REXGLUE_TOOL=third_party/rexglue-sdk/out/linux-amd64/rexglue
if [[ -x "$REXGLUE_TOOL" ]]; then
  ICON_TMP="$(mktemp -d)"
  (cd "$ICON_TMP" && LD_LIBRARY_PATH="$ROOT/$(dirname "$REXGLUE_TOOL")" "$ROOT/$REXGLUE_TOOL" \
     init --project-name icon --xex-path "$ROOT/game/default.xex" \
     achievements "$ROOT/game/default.xex" "$ICON_TMP/out" >/dev/null 2>&1) || true
  if [[ -f "$ICON_TMP/out/icons/title.png" ]]; then
    for density in mdpi hdpi xhdpi xxhdpi xxxhdpi; do
      mkdir -p "$WORK/res-icon/mipmap-$density"
      cp "$ICON_TMP/out/icons/title.png" "$WORK/res-icon/mipmap-$density/ic_launcher.png"
    done
    "$BUILD_TOOLS/aapt2" compile --dir "$WORK/res-icon" -o "$WORK/res/icon.zip"
    ICON_OVERLAY=(-R "$WORK/res/icon.zip")
  fi
  rm -rf "$ICON_TMP"
fi
"$BUILD_TOOLS/aapt2" link -o "$WORK/unsigned.apk" -I "$PLATFORM_JAR" \
  --manifest tools/android/AndroidManifest.xml "$WORK/res/sdl.zip" -R "$WORK/res/skate1.zip" \
  "${ICON_OVERLAY[@]}"

# 4. Code and native libraries into the APK, align, sign.
python3 - "$WORK" <<'PY'
import os, sys, zipfile
work = sys.argv[1]
with zipfile.ZipFile(os.path.join(work, 'unsigned.apk'), 'a', zipfile.ZIP_DEFLATED) as apk:
    apk.write(os.path.join(work, 'classes.dex'), 'classes.dex')
    for name in sorted(os.listdir(os.path.join(work, 'lib/arm64-v8a'))):
        apk.write(os.path.join(work, 'lib/arm64-v8a', name), 'lib/arm64-v8a/' + name)
PY
"$BUILD_TOOLS/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
if [[ ! -f "$KEYSTORE" ]]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias skate1 -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=skate1 debug" >/dev/null
fi
rm -rf "$PACKAGE" && mkdir -p "$PACKAGE"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$PACKAGE/skate1.apk" "$WORK/aligned.apk"

# 5. Config for the device: desktop settings, then the Android overrides.
python3 - config/skate1.runtime.toml tools/android/android-overrides.toml "$PACKAGE/skate1.toml" <<'PY'
import re, sys
base, over, out = sys.argv[1:4]
key = lambda l: re.match(r'\s*([A-Za-z0-9_]+)\s*=', l)
over_lines = open(over).read().splitlines()
over_keys = {key(l).group(1) for l in over_lines if key(l)}
kept = [l for l in open(base).read().splitlines() if not (key(l) and key(l).group(1) in over_keys)]
open(out, 'w').write('\n'.join(kept + [''] + over_lines) + '\n')
PY
cat > "$PACKAGE/README.txt" <<'TXT'
skate1 for Android (arm64)

1. Install skate1.apk and launch it once: it asks for "All files access";
   allow it, then quit.
2. Make a folder named skate1 at the root of the internal storage (or of a
   microSD card) and put skate1.toml and the extracted game/ folder in it:
     skate1/skate1.toml
     skate1/game/default.xex ...
   Any way works: microSD card, file manager, LocalSend, adb push.
   (Without the permission, Android/data/com.kurtisone.skate1/files/ is used.)
3. Launch it again. Logs go to skate1/logs/, a crash report to skate1/crash.txt.

Custom GPU driver (optional): unzip a driver package for Adreno (Turnip, or a
newer Qualcomm driver, as made for AdrenoTools: meta.json + the driver .so)
into skate1/driver/. The game loads it at launch and writes the outcome to
skate1/driver_status.txt. Remove the folder to go back to the system driver.

GPU turbo (optional): gpu_turbo = true in skate1.toml keeps the Adreno at its
top clock (more heat and battery use).
TXT
echo "APK ready: $PACKAGE/skate1.apk"
