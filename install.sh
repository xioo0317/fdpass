#!/usr/bin/env bash
# 应用侧唯一允许的动作：只替换 libfdpass.so，其他代码一律不动。
# 用法: ./install.sh <OZJFLASH 仓库根目录> [arm64 产物路径]
set -euo pipefail

APP_ROOT="${1:-}"
SO="${2:-out/arm64-v8a/libfdpass.so}"

[ -n "$APP_ROOT" ] || { echo "用法: ./install.sh <应用仓库根目录> [libfdpass.so]" >&2; exit 1; }
[ -f "$SO" ] || { echo "找不到产物: $SO（先运行 ./build.sh arm64-v8a）" >&2; exit 1; }

# 防止把 host x86_64 验证产物误装进 Android 工程
if ! file "$SO" | grep -q "ARM aarch64"; then
    echo "产物不是 ARM aarch64 ELF，拒绝替换：$SO" >&2
    file "$SO" >&2
    exit 1
fi

TARGET="$APP_ROOT/app/src/main/jniLibs/arm64-v8a/libfdpass.so"
[ -f "$TARGET" ] || { echo "找不到目标文件: $TARGET" >&2; exit 1; }

BACKUP="$TARGET.bak.$(date +%Y%m%d%H%M%S)"
cp -p "$TARGET" "$BACKUP"
cp "$SO" "$TARGET"

echo "已替换: $TARGET"
echo "原文件备份: $BACKUP"
echo "应用其余文件未做任何改动；重新构建 APK 即可生效。"
