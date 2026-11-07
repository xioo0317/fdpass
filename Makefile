# host 侧验证：编译 core + JNI 薄层，跑真实 SCM_RIGHTS 回环测试与符号契约检查。
# Android 产物请用 ./build.sh（需要 NDK）。
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=c11 -pthread
JAVA_HOME ?= $(shell dirname $(shell dirname $(shell readlink -f $(shell command -v java))))
JNI_INC ?= $(JAVA_HOME)/include
JNI_MD_INC ?= $(JNI_INC)/linux
ORIGINAL_SO ?=

.PHONY: all check check-jni clean

all: libfdpass.so test_fdpass

# -fvisibility=hidden 保证只导出 3 个 JNI 符号，与原 stripped so 一致
libfdpass.so: src/fdpass_core.c src/fdpass_jni.c src/fdpass_core.h
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared \
		-Isrc -I$(JNI_INC) -I$(JNI_MD_INC) \
		-o $@ src/fdpass_core.c src/fdpass_jni.c

test_fdpass: tests/test_core.c src/fdpass_core.c src/fdpass_core.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_core.c src/fdpass_core.c

check: all
	./tests/check_symbols.sh "$(ORIGINAL_SO)" ./libfdpass.so
	./test_fdpass

# 真实 JVM 验证（需要 javac + python3）：证明 Kotlin FdPass 无需改代码即可调用
check-jni: libfdpass.so
	./tests/run_jni_smoke.sh

clean:
	rm -f libfdpass.so test_fdpass
	rm -rf build out
