/*
 * test_core.c — host 侧真实 SCM_RIGHTS 回环测试（不依赖 JNI / Android）。
 *
 * 模拟魔改 libfastboot.so / libadb.so 的接收端：
 *   connect 抽象 socket → write 1 字节请求 → recvmsg(SCM_RIGHTS) 拿 fd
 * 再用 pipe 的 fstat inode + 真实 write/read 证明收到的 fd 指向同一内核对象。
 */
#define _GNU_SOURCE
#include "fdpass_core.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

/* ---------- 模拟魔改二进制的接收端 ---------- */

static int client_recv(const char *name, int *out_fd, char *out_byte) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const char *tail = name + 1;
    size_t n = strlen(tail);
    CHECK(n <= 106, "test socket name too long");
    memcpy(addr.sun_path + 1, tail, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket: %s", strerror(errno));

    /* 与原二进制一致：connect 失败重试 101×20ms */
    int connected = 0;
    for (int i = 0; i < 101; i++) {
        if (connect(s, (struct sockaddr *)&addr, len) == 0) { connected = 1; break; }
        usleep(20000);
    }
    if (!connected) { close(s); return -2; }

    CHECK(write(s, "Q", 1) == 1, "write request byte");

    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    ssize_t got_bytes = recvmsg(s, &msg, 0);
    if (got_bytes < 0) { close(s); return -3; }

    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c == NULL || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len < CMSG_LEN(sizeof(int))) {
        close(s);
        return -4;
    }
    int received_fd = -1;
    memcpy(&received_fd, CMSG_DATA(c), sizeof(received_fd));
    *out_fd = received_fd;
    *out_byte = byte;
    close(s);
    return (int)got_bytes;
}

static void assert_same_pipe(int expected, int received) {
    struct stat a, b;
    CHECK(fstat(expected, &a) == 0, "fstat(expected)");
    CHECK(fstat(received, &b) == 0, "fstat(received)");
    CHECK(a.st_dev == b.st_dev && a.st_ino == b.st_ino,
          "fd 未指向同一内核对象 (dev/ino 不一致)");
    CHECK(write(received, "P", 1) == 1, "write through received fd");
}

/* ---------- 线程载体 ---------- */

struct serve_ctx { int fd; const char *name; int ret; };

static void *serve_thread(void *arg) {
    struct serve_ctx *c = (struct serve_ctx *)arg;
    c->ret = fdpass_serve_loop(c->fd, c->name);
    return NULL;
}

static void *once_thread(void *arg) {
    struct serve_ctx *c = (struct serve_ctx *)arg;
    c->ret = fdpass_send_once(c->fd, c->name);
    return NULL;
}

static void wait_listener(void) {
    for (int i = 0; i < 400 && fdpass_current_listen_fd() < 0; i++) usleep(5000);
}

/* ---------- 用例 ---------- */

static void test_serve_loop(void) {
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    char name[80];
    snprintf(name, sizeof(name), "@ozj_fdpass_serve_%d", (int)getpid());

    struct serve_ctx ctx = { .fd = p[1], .name = name, .ret = -999 };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, serve_thread, &ctx) == 0, "pthread_create");
    wait_listener();
    CHECK(fdpass_current_listen_fd() >= 0, "serve_loop 未进入 listen");

    for (int i = 0; i < 3; i++) {
        int got = -1;
        char byte = 0;
        int n = client_recv(name, &got, &byte);
        CHECK(n == 1, "第 %d 次 recvmsg 字节数=%d", i + 1, n);
        CHECK(byte == 0x66, "第 %d 次数据字节=0x%02x（应为 0x66）", i + 1,
              (unsigned char)byte);
        CHECK(got >= 0, "第 %d 次未收到有效 fd", i + 1);
        assert_same_pipe(p[1], got);
        char ch = 0;
        CHECK(read(p[0], &ch, 1) == 1 && ch == 'P', "第 %d 次 pipe 读回失败", i + 1);
        close(got);
    }

    /* 给 serve 线程留出完成第 3 次计数的时间，规避测试竞态（非库行为） */
    usleep(100000);
    fdpass_stop_serve_loop();
    pthread_join(t, NULL);
    CHECK(ctx.ret == 3, "serve_loop 返回值=%d（应为成功次数 3）", ctx.ret);
    CHECK(fdpass_current_listen_fd() == -1, "stop 后全局 listen fd 未复位");
    close(p[0]);
    close(p[1]);
    printf("  [OK] serve_loop: 3 次传递 / 0x66 / fd 同一 pipe / stop 返回 3 / 全局复位\n");
}

static void test_send_once(void) {
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    char name[80];
    snprintf(name, sizeof(name), "@ozj_fdpass_once_%d", (int)getpid());

    struct serve_ctx ctx = { .fd = p[1], .name = name, .ret = -999 };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, once_thread, &ctx) == 0, "pthread_create");

    int got = -1;
    char byte = 0;
    int n = client_recv(name, &got, &byte);
    CHECK(n == 1, "send_once recvmsg 字节数=%d", n);
    CHECK(byte == 0x66, "数据字节=0x%02x（应为 0x66）", (unsigned char)byte);
    CHECK(got >= 0, "未收到有效 fd");
    assert_same_pipe(p[1], got);
    char ch = 0;
    CHECK(read(p[0], &ch, 1) == 1 && ch == 'P', "pipe 读回失败");
    close(got);

    pthread_join(t, NULL);
    CHECK(ctx.ret == 1, "send_once 返回值=%d（应为 sendmsg 字节数 1）", ctx.ret);
    close(p[0]);
    close(p[1]);
    printf("  [OK] send_once: 原始 fd 一次性传递 / 0x66 / 返回 sendmsg 字节数 1\n");
}

static void test_error_codes(void) {
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    CHECK(fdpass_serve_loop(p[1], NULL) == FDPASS_ERR_NAME, "serve(NULL) != -2");
    CHECK(fdpass_serve_loop(p[1], "no_at_prefix") == FDPASS_ERR_NAME, "serve(no@) != -2");
    CHECK(fdpass_send_once(p[1], NULL) == FDPASS_ERR_NAME, "once(NULL) != -2");
    CHECK(fdpass_send_once(p[1], "no_at_prefix") == FDPASS_ERR_NAME, "once(no@) != -2");
    fdpass_stop_serve_loop(); /* 无 listen fd 时必须安全返回 */
    printf("  [OK] 错误码: name 校验 -2 / stop 空调用安全\n");

    /* 抽象 socket 名字冲突 → bind 失败 -3 */
    char name[80];
    snprintf(name, sizeof(name), "@ozj_fdpass_busy_%d", (int)getpid());
    struct serve_ctx ctx = { .fd = p[1], .name = name, .ret = -999 };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, serve_thread, &ctx) == 0, "pthread_create");
    wait_listener();
    CHECK(fdpass_current_listen_fd() >= 0, "busy serve 未 listen");
    CHECK(fdpass_send_once(p[1], name) == FDPASS_ERR_BIND, "重复 bind 应返回 -3");
    fdpass_stop_serve_loop();
    pthread_join(t, NULL);
    close(p[0]);
    close(p[1]);
    printf("  [OK] 错误码: 重复 bind 返回 -3，且不影响常驻服务\n");
}

int main(void) {
    printf("fdpass core 回环测试 (pid=%d)\n", (int)getpid());
    printf("  CMSG_LEN=%zu  CMSG_SPACE=%zu  (arm64 原库: 20 / 24)\n",
           (size_t)CMSG_LEN(sizeof(int)), (size_t)CMSG_SPACE(sizeof(int)));
    CHECK(CMSG_LEN(sizeof(int)) == 20 && CMSG_SPACE(sizeof(int)) == 24,
          "host 的 CMSG 布局与 arm64 不一致，测试结论不可外推");

    test_serve_loop();
    test_send_once();
    test_error_codes();
    printf("ALL PASS — core 行为与原 libfdpass.so 契约一致\n");
    return 0;
}
