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

if [[ ! -f generated/default/sources.cmake ]]; then
  echo "generated/ is missing: run the desktop build (codegen) first" >&2
  exit 1
fi

# 1. Native libraries.
cmake -S tools/android -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
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
"$LLVM/bin/llvm-strip" --strip-unneeded "$LIBS"/*.so

# 2. Java: SDL's activity and helpers, plus ours.
javac -source 11 -target 11 -nowarn -encoding UTF-8 -cp "$PLATFORM_JAR" -d "$WORK/classes" \
  $(find "$SDL_PROJECT/app/src/main/java" tools/android/java -name '*.java')
"$BUILD_TOOLS/d8" --release --min-api 29 --lib "$PLATFORM_JAR" --output "$WORK" \
  $(find "$WORK/classes" -name '*.class')

# 3. Resources (SDL's, with ours on top) and the manifest.
"$BUILD_TOOLS/aapt2" compile --dir "$SDL_PROJECT/app/src/main/res" -o "$WORK/res/sdl.zip"
"$BUILD_TOOLS/aapt2" compile --dir tools/android/res -o "$WORK/res/skate1.zip"
"$BUILD_TOOLS/aapt2" link -o "$WORK/unsigned.apk" -I "$PLATFORM_JAR" \
  --manifest tools/android/AndroidManifest.xml "$WORK/res/sdl.zip" -R "$WORK/res/skate1.zip"

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

1. Install skate1.apk and launch it once (it creates its folder), then quit.
2. Copy skate1.toml and the extracted game/ folder into
     Android/data/com.kurtisone.skate1/files/
   on the device's internal storage (USB from a PC works best; Android
   restricts file managers in Android/data).
3. Launch it again. Logs go to files/logs/ in the same folder.
TXT
echo "APK ready: $PACKAGE/skate1.apk"
