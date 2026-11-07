#!/usr/bin/env bash
# 真实 JVM 验证：用与 Kotlin FdPass 同签名的 Java 类加载新 .so，调用 3 个 native 方法。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

command -v javac >/dev/null  || { echo "需要 javac" >&2; exit 1; }
command -v python3 >/dev/null || { echo "需要 python3" >&2; exit 1; }
[ -f libfdpass.so ] || make libfdpass.so

OUT="tests/jni/out"
LOG="tests/jni/smoke.log"
mkdir -p "$OUT"
rm -f "$LOG"
javac -d "$OUT" \
    tests/jni/com/Android/MoFlash/data/FdPass.java \
    tests/jni/JniSmoke.java

NAME="@ozj_fdpass_jni_$$_$(date +%s)"
java -Djava.library.path="$ROOT" -cp "$OUT" JniSmoke "$NAME" >"$LOG" 2>&1 &
JPID=$!

# 等 JVM 进入 listen
sleep 0.4
python3 tests/jni/client.py "$NAME"

if ! wait "$JPID"; then
    echo "JVM 进程异常退出：" >&2; cat "$LOG" >&2; exit 1
fi
cat "$LOG"
grep -q "JNI_SERVED=2" "$LOG"       || { echo "serveFdLoop 计数不符" >&2; exit 1; }
grep -q "JNI_SEND_ERR_OK" "$LOG"    || { echo "sendFd 错误码不符" >&2; exit 1; }

echo "[OK] JVM 按 FdPass 同签名成功解析并调用 3 个 native 方法（应用无需改代码）"
