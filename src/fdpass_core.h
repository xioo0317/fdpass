/*
 * fdpass_core.h — 纯 C 核心，不依赖 JNI，可在 host 侧直接编译与测试。
 *
 * 行为契约来自对 MOFLASH.apk 内 libfdpass.so (arm64, stripped, 6640B) 的反汇编取证，
 * 详见 README.md「契约取证」一节。本文件只声明能力，不定义任何 JNI 符号。
 */
#ifndef FDPASS_CORE_H
#define FDPASS_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码：与原 libfdpass.so 的返回值逐字对齐 */
#define FDPASS_ERR_SOCKET  (-1)  /* socket() 失败            */
#define FDPASS_ERR_NAME    (-2)  /* name 为空 / 不以 '@' 开头 */
#define FDPASS_ERR_BIND    (-3)  /* bind() 失败              */
#define FDPASS_ERR_LISTEN  (-4)  /* listen() 失败            */
#define FDPASS_ERR_ACCEPT  (-5)  /* accept() 失败（sendFd）  */
#define FDPASS_ERR_TIMEOUT (-6)  /* poll() 超时/失败（sendFd）*/

/**
 * 常驻服务：绑定抽象 socket，循环接受连接并传递 fd，直到 fdpass_stop_serve_loop()。
 *
 * 每次连接：poll(2s) → 可选 read(1B) → dup(fd) → sendmsg(SCM_RIGHTS) → 关闭副本。
 * 返回成功执行传递的次数；建链失败时返回上述负错误码。
 * 该调用阻塞，应由调用方放在独立线程（应用层即 "fdpass-serve" daemon 线程）。
 */
int fdpass_serve_loop(int fd, const char *name);

/** 中断 fdpass_serve_loop()：shutdown(listen_fd, SHUT_RDWR)，不 close。 */
void fdpass_stop_serve_loop(void);

/**
 * 一次性服务：listen(1) → poll(10s) → accept → read(1B) → sendmsg 传原始 fd（不 dup）。
 * 返回 sendmsg() 的字节数（成功为 1）；失败返回负错误码。
 */
int fdpass_send_once(int fd, const char *name);

/**
 * 当前常驻 listen fd（-1 表示未建立）。
 * 仅供 host 测试规避轮询竞态使用；visibility=hidden，不会出现在 .so 导出符号表。
 */
int fdpass_current_listen_fd(void);

#ifdef __cplusplus
}
#endif
#endif /* FDPASS_CORE_H */
