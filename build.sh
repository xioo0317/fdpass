#!/usr/bin/env bash
# 用 Android NDK 编译可替换的 libfdpass.so。
# 用法: ./build.sh [arm64-v8a|armeabi-v7a|x86_64|x86]   (默认 arm64-v8a)
set -euo pipefail

ABI="${1:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-21}"
case "$ABI" in
    arm64-v8a|armeabi-v7a|x86_64|x86) ;;
    *) echo "不支持的 ABI: $ABI" >&2; exit 1 ;;
esac

find_ndk() {
    local cand
    for cand in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_LATEST_HOME:-}" \
                "${ANDROID_HOME:-}/ndk-bundle" "${ANDROID_SDK_ROOT:-}/ndk-bundle"; do
        if [ -n "$cand" ] && [ -f "$cand/build/cmake/android.toolchain.cmake" ]; then
            echo "$cand"; return 0
        fi
    done
    for base in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
        [ -n "$base" ] || continue
        local latest
        latest=$(ls -d "$base"/ndk/* 2>/dev/null | sort -V | tail -1 || true)
        if [ -n "$latest" ] && [ -f "$latest/build/cmake/android.toolchain.cmake" ]; then
            echo "$latest"; return 0
        fi
    done
    return 1
}

NDK="$(find_ndk)" || {
    echo "未找到 Android NDK。请设置 ANDROID_NDK_HOME 或 ANDROID_HOME（内含 ndk/<version>）。" >&2
    exit 1
}
echo "NDK: $NDK"
echo "ABI: $ABI  platform: $ANDROID_PLATFORM"

cmake -S "$(dirname "$0")" -B "build/$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "build/$ABI" --target fdpass -j

mkdir -p "out/$ABI"
cp "build/$ABI/libfdpass.so" "out/$ABI/libfdpass.so"
echo "产物: out/$ABI/libfdpass.so"
file "out/$ABI/libfdpass.so"

# 符号契约自检（nm 可读取交叉编译产物）
bash "$(dirname "$0")/tests/check_symbols.sh" "" "out/$ABI/libfdpass.so"
