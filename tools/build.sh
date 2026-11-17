#!/usr/bin/env bash
# Builds the 64-bit libs3e_android.so into build/arm64.
set -e
cd "$(dirname "$0")/.."
SDK="${ANDROID_HOME:-C:/Users/nebula/AppData/Local/Android/Sdk}"
NDK="$SDK/ndk/27.3.13750724"
CMAKE="$SDK/cmake/3.31.6/bin/cmake.exe"
INC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include"

python -I tools/gen_gl.py "$INC" src/gl_thunks.inc > /dev/null
if [ ! -f build/arm64/build.ninja ]; then
    "$CMAKE" -G Ninja -S . -B build/arm64 \
        -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.31.6/bin/ninja.exe" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release > build-configure.log
fi
"$CMAKE" --build build/arm64 -j 8
ls -la build/arm64/libs3e_android.so
