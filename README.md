# fdpass — MOFLASH `libfdpass.so` 的等价源码实现

把 OZJFLASH 里那颗 **stripped 的二进制 `libfdpass.so`** 用可读 C 源码重写，行为与原库逐条对齐。

> **边界（本次只做兼容尝试）**
> - 独立的源码仓库，**不修改 OZJFLASH 应用仓库里的任何代码**。
> - 不改 Kotlin 调用方式：`FdPass.sendFd / serveFdLoop / stopServeLoop` 三个 `external fun` 一个字不动。
> - 不改 `libfastboot.so` / `libadb.so` 的接收逻辑，也不改 `ToolConsole` / `UsbFdSession`。
> - 应用侧唯一动作：**把编译出的 `libfdpass.so` 覆盖进 `jniLibs/arm64-v8a/`**（`install.sh` 只做这一件事）。

---

## 1. 契约取证

对 `app/src/main/jniLibs/arm64-v8a/libfdpass.so`（ELF64 aarch64, stripped, 6640 B）：

```bash
nm -D --defined-only libfdpass.so       # 只有 3 个 T 符号
readelf -d libfdpass.so | grep NEEDED   # liblog/libm/libdl/libc
llvm-objdump -d --start-address=0x990 --stop-address=0xf58 libfdpass.so
```

| JNI 符号 | 反汇编结论 |
|---|---|
| `Java_com_Android_MoFlash_data_FdPass_sendFd` | 一次性服务：`socket→bind→listen(1)→poll(10s)→accept→read(1B)→sendmsg`，传**原始 fd**（不 dup），返回 `sendmsg` 字节数 |
| `Java_com_Android_MoFlash_data_FdPass_serveFdLoop` | 常驻服务：`listen(128)`，循环 `accept→poll(2s)→可选 read(1B)→dup(fd)→sendmsg(SCM_RIGHTS)→关副本`；返回成功次数，被 `stopServeLoop` 中断 |
| `Java_com_Android_MoFlash_data_FdPass_stopServeLoop` | `shutdown(g_listen_fd, SHUT_RDWR)` 并置 `-1`，**不 close**；由 serve 循环自行关 listen fd |

导入函数仅：`socket bind listen poll accept read sendmsg close dup shutdown strlen strncpy __errno __stack_chk_fail`。**无 `JNI_OnLoad`、无 `RegisterNatives`、无 `connect`** —— 两个 JNI 方法都是「服务端」，客户端（魔改二进制）自己 connect。

逐字节细节：

| 项 | 值 |
|---|---|
| socket | `AF_UNIX(1), SOCK_STREAM(1), 0` |
| 地址 | `sockaddr_un` 清零 → `sun_family=1` → `sun_path[0]=0`（抽象命名空间）→ `strncpy(sun_path+1, name+1, 106)` |
| `addrlen` | `strlen(name+1) + 3` |
| `name` 要求 | 首字符必须是 `@`；否则返回 `-2` |
| backlog | `serveFdLoop=128`，`sendFd=1` |
| 数据 | `iov_len=1`，恒为 `0x66`（`'f'`） |
| CMSG | `SOL_SOCKET/SCM_RIGHTS`，`cmsg_len=20`，`msg_controllen=24`（64 位） |
| 错误码 | `-1 socket` / `-2 name` / `-3 bind` / `-4 listen` / `-5 accept(sendFd)` / `-6 poll 超时(sendFd)` |

`serveFdLoop` 计数规则：`dup()` 成功即 `+1`，`sendmsg` 成败不影响；`accept` 遇 `EINTR` 重试，其它错误退出循环。

---

## 2. 目录

```
fdpass/
├── src/
│   ├── fdpass_core.h      # 纯 C API + 错误码（不依赖 JNI）
│   ├── fdpass_core.c      # 核心实现：地址构造 / SCM_RIGHTS / 循环与中断
│   └── fdpass_jni.c       # 3 个 JNIEXPORT 薄层（唯一依赖 jni.h）
├── tests/
│   ├── test_core.c        # host 真实回环：模拟魔改二进制 recvmsg 收 fd
│   ├── check_symbols.sh   # 导出符号契约比对（对齐原 so）
│   ├── run_jni_smoke.sh   # 真实 JVM 冒烟：按 FdPass 同签名调用 3 个 native 方法
│   ├── jni/
│   │   ├── com/Android/MoFlash/data/FdPass.java  # 与 Kotlin 侧同签名的 Java 替身
│   │   ├── JniSmoke.java
│   │   └── client.py      # python 版魔改二进制接收端（recvmsg SCM_RIGHTS）
│   └── jni/smoke.log      # 运行产物（已忽略）
├── CMakeLists.txt         # NDK / host 通用构建
├── build.sh               # NDK 编译可替换的 arm64 .so
├── install.sh             # 只覆盖应用 jniLibs 里的 libfdpass.so
├── Makefile               # host 侧 make check / make check-jni
└── .github/workflows/build.yml  # CI：host 测试 + 多 ABI 产物
```

**core / JNI 分离**的目的是：核心可以在 x86_64 host 上直接编译并做真实 `SCM_RIGHTS` 回环测试，不需要 Android 设备。

---

## 3. 验证（无需 Android SDK）

```bash
cd fdpass
make check
```

`make check` 做两件事：

1. **符号契约**：编译 host `.so`，`nm -D` 断言导出的 T 符号恰好是那 3 个（`-fvisibility=hidden` 保证 core 辅助不外泄）。
   若手头有原库，可传入做逐字比对：
   ```bash
   make check ORIGINAL_SO=/workspace/OZJFLASH/OZJFLASH-main/app/src/main/jniLibs/arm64-v8a/libfdpass.so
   ```
2. **行为回环** `test_core.c`：起线程跑 `fdpass_serve_loop`，主线程模拟 `libfastboot.so`：
   `connect` 抽象 socket → `write 1B` → `recvmsg(SCM_RIGHTS)`，然后
   - 校验数据字节为 `0x66`；
   - `fstat` 比对 dev/ino，确认收到的 fd 与原始 `pipe` 是同一内核对象；
   - 通过收到的 fd 写 `'P'`，从 pipe 另一端读回，证明 fd 真的可用；
   - 连续 3 次连接后 `stopServeLoop()`，断言返回成功次数 `3`、全局 listen fd 复位为 `-1`；
   - `send_once` 一次性语义、错误码 `-2/-3`、空 `stop` 安全性。
   同时断言 `CMSG_LEN=20 / CMSG_SPACE=24`，确保 host 结论可外推到 arm64。

### 3.1 真实 JVM 冒烟（关键：证明应用不用改代码）

```bash
make check-jni
```

用 `com.Android.MoFlash.data.FdPass`（包名/类名/方法签名与 OZJFLASH 的 Kotlin 类完全一致）的 Java 替身 `System.loadLibrary("fdpass")`，JVM 按**短名**解析到这 3 个符号；python 客户端连两次收 fd，验证 `serveFdLoop` 返回 2、`sendFd` 非法 name 返回 -2。这直接证明替换 `.so` 后 Kotlin 侧零改动即可工作。

---

## 4. 产出 Android 产物

```bash
./build.sh arm64-v8a        # 需要 ANDROID_NDK_HOME / ANDROID_HOME/ndk/<ver>
```

产物在 `out/arm64-v8a/libfdpass.so`，脚本会自动跑一遍符号契约检查。

CI（`.github/workflows/build.yml`）在 push 时：
- `host-check` job 跑 `make check` + `make check-jni`；
- `android` job 用 runner 预装 NDK 编译 `arm64-v8a / armeabi-v7a / x86_64`，每个产物先跑一次符号契约复核，再上传 artifact。

> 本沙盒无 Android NDK，`make check` 产出的 `libfdpass.so` 是 **x86_64 host 验证产物**，
> 不能直接装进 Android 工程；`install.sh` 会校验 ELF 架构并拒绝误装。
> 当前阶段**未改动应用仓库任何文件**，真机替换请用 `./build.sh arm64-v8a` 或 CI artifact。

---

## 5. 应用侧替换（唯一允许的改动）

```bash
# 在 fdpass 仓库根目录，产物来自 build.sh 或 CI artifact
./install.sh /workspace/OZJFLASH/OZJFLASH-main out/arm64-v8a/libfdpass.so
```

`install.sh` 会：
- 校验产物是 **ARM aarch64 ELF**（防止误装 host x86_64 验证产物）；
- 备份原文件为 `libfdpass.so.bak.<时间戳>`；
- 只覆盖 `app/src/main/jniLibs/arm64-v8a/libfdpass.so`，**不触碰其他任何文件**。

之后重新构建 APK 即可。Kotlin 的 `FdPass` 类、`libfastboot.so`、`libadb.so`、`ToolConsole`、`UsbFdSession` 全部零改动。

---

## 6. 与原库的两处防御性差异

正常输入（`@` 前缀 + 长度 < 100 的 socket 名）下行为完全一致，仅极端情况不同：

1. **超长 name**：原实现 `addrlen = strlen(name+1)+3` 可能超过 `sizeof(sockaddr_un)`；本实现把它收敛到 `sizeof`，并把拷贝量限制在 106 字节。
2. **全局 listen fd**：原实现是普通 `int`；本实现用 `atomic_int`，保证 `stopServeLoop()` 跨线程调用的可见性（shutdown 语义不变）。

其余（错误码、计数规则、poll 超时、CMSG 布局、数据字节、`dup` 与否、`shutdown` 而非 `close`）均逐条对齐。

---

## 7. 接入全链路的位置（只读参考）

```
UsbFdSession.ensure()           openDevice → getFileDescriptor()   ← 与 fdpass 无关
ToolConsole.execWithFdPass()    Thread{ FdPass().serveFdLoop(fd, "@momiui_fb_...") }
                                ↓ waitSocketReady()
ProcessBuilder(fastboot/adb)    env: MOMIUI_USB_SOCK / MOMIUI_USB_FD
                                ↓
libfastboot.so / libadb.so      connect → write(1B) → recvmsg(SCM_RIGHTS) → claim interface
                                ↓ 进程退出
ToolConsole.finally             FdPass().stopServeLoop() → join
```

fdpass 只负责「把已授权的 usbfs fd 递给子进程」这一段；它既不做 USB 枚举授权，也不参与协议通信。
