#!/usr/bin/env bash
# 校验新产物导出的 JNI 符号与原 libfdpass.so 完全一致（只允许这 3 个 T 符号）。
# 用法: ./tests/check_symbols.sh [原so路径] [新so路径]
#   - 传入原 so 时，直接从它提取期望集合；否则用反汇编取证的固定清单。
set -euo pipefail

ORIGINAL="${1:-}"
NEW_SO="${2:-./libfdpass.so}"

[ -f "$NEW_SO" ] || { echo "找不到待检查产物: $NEW_SO" >&2; exit 1; }

if [ -n "$ORIGINAL" ] && [ -f "$ORIGINAL" ]; then
    EXPECTED=$(nm -D --defined-only "$ORIGINAL" | awk '$2=="T"{print $3}' | sort)
    echo "期望符号来自: $ORIGINAL"
else
    EXPECTED=$(printf '%s\n' \
        Java_com_Android_MoFlash_data_FdPass_sendFd \
        Java_com_Android_MoFlash_data_FdPass_serveFdLoop \
        Java_com_Android_MoFlash_data_FdPass_stopServeLoop | sort)
    echo "期望符号来自: 反汇编取证固定清单"
fi

ACTUAL=$(nm -D --defined-only "$NEW_SO" | awk '$2=="T"{print $3}' | sort)

echo "--- 原库 ---"; echo "$EXPECTED"
echo "--- 新库 ---"; echo "$ACTUAL"

if diff -u <(echo "$EXPECTED") <(echo "$ACTUAL") >/dev/null; then
    echo "[OK] 导出符号完全一致（3 个，无多余符号）"
else
    echo "[FAIL] 导出符号不一致" >&2
    diff -u <(echo "$EXPECTED") <(echo "$ACTUAL") >&2 || true
    exit 1
fi
