/*
 * fdpass_core.c — MOFLASH libfdpass.so 的等价 C 实现（兼容尝试）。
 *
 * 逐条对齐反汇编结论（arm64 / llvm-objdump）：
 *   socket(AF_UNIX=1, SOCK_STREAM=1, 0)
 *   sockaddr_un: memset 0 → sun_family=1 → sun_path[0]=0 → strncpy(sun_path+1, name+1, 106)
 *   addrlen = strlen(name + 1) + 3
 *   listen(128) [serveFdLoop] / listen(1) [sendFd]
 *   cmsg: level=SOL_SOCKET, type=SCM_RIGHTS, cmsg_len=20, msg_controllen=24
 *   iov: 1 字节，内容 0x66 ('f')
 *
 * 仅两处防御性增强，正常输入下行为与原库完全一致：
 *   1) 超长 name 时把 addrlen 收敛到 sizeof(sockaddr_un)，避免原实现潜在的越界读；
 *   2) 全局 listen fd 用 atomic_int，保证跨线程 shutdown 的可见性。
 */
#include "fdpass_core.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ABSTRACT_PREFIX   '@'
#define SUN_PATH_COPY_MAX 106  /* sizeof(sockaddr_un.sun_path) - 2 */
#define DATA_BYTE         0x66 /* 原库恒发一个 'f' */
#define SERVE_BACKLOG     128
#define SERVE_POLL_MS     2000
#define ONCE_BACKLOG      1
#define ONCE_POLL_MS      10000

/* 全局唯一 listen fd：serveFdLoop 保存，stopServeLoop 用 shutdown 唤醒 */
static atomic_int g_listen_fd = -1;

/** 构造 Linux 抽象 socket 地址；name 必须以 '@' 开头（'@' 本身不进 sun_path） */
static int make_addr(const char *name, struct sockaddr_un *out, socklen_t *out_len) {
    if (name == NULL || name[0] != ABSTRACT_PREFIX) return FDPASS_ERR_NAME;

    const char *tail = name + 1;
    size_t n = strlen(tail);
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;

    /* 原实现 strncpy(sun_path+1, tail, 106)；memset 已保证末尾有 NUL */
    size_t copy = n < SUN_PATH_COPY_MAX ? n : SUN_PATH_COPY_MAX;
    memcpy(out->sun_path + 1, tail, copy);

    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    if (len > (socklen_t)sizeof(*out)) len = (socklen_t)sizeof(*out);
    *out_len = len;
    return 0;
}

static int open_listener(const char *name, int backlog, int *out_fd) {
    struct sockaddr_un addr;
    socklen_t addrlen;
    int rc = make_addr(name, &addr, &addrlen);
    if (rc != 0) return rc;

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return FDPASS_ERR_SOCKET;
    if (bind(s, (const struct sockaddr *)&addr, addrlen) < 0) {
        close(s);
        return FDPASS_ERR_BIND;
    }
    if (listen(s, backlog) < 0) {
        close(s);
        return FDPASS_ERR_LISTEN;
    }
    *out_fd = s;
    return 0;
}

/** sendmsg：1 字节数据 + SCM_RIGHTS 携带一个 fd。返回 sendmsg 的结果。 */
static ssize_t send_one_fd(int sock, int fd) {
    char byte = (char)DATA_BYTE;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };

    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0);
}

int fdpass_serve_loop(int fd, const char *name) {
    int lfd = -1;
    int rc = open_listener(name, SERVE_BACKLOG, &lfd);
    if (rc != 0) return rc;

    atomic_store(&g_listen_fd, lfd);
    int served = 0;

    for (;;) {
        int client = accept(lfd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break; /* stopServeLoop 的 shutdown 会让 accept 失败，从而退出 */
        }

        /* 原实现：poll 2s，有数据才 read 1 字节；超时也照样发送 */
        struct pollfd pfd = { .fd = client, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, SERVE_POLL_MS) >= 1) {
            char req;
            ssize_t ignored = read(client, &req, 1);
            (void)ignored;
        }

        int copy = dup(fd);
        if (copy >= 0) {
            ssize_t ignored = send_one_fd(client, copy);
            (void)ignored;
            close(copy);
            served++; /* 与原库一致：dup 成功即计数，sendmsg 成败不影响 */
        }
        close(client);
    }

    int expected = lfd;
    atomic_compare_exchange_strong(&g_listen_fd, &expected, -1);
    close(lfd);
    return served;
}

void fdpass_stop_serve_loop(void) {
    int fd = atomic_load(&g_listen_fd);
    if (fd >= 0) {
        (void)shutdown(fd, SHUT_RDWR);
        atomic_store(&g_listen_fd, -1);
    }
}

int fdpass_send_once(int fd, const char *name) {
    int lfd = -1;
    int rc = open_listener(name, ONCE_BACKLOG, &lfd);
    if (rc != 0) return rc;

    struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, ONCE_POLL_MS) <= 0) {
        close(lfd);
        return FDPASS_ERR_TIMEOUT;
    }

    int client = accept(lfd, NULL, NULL);
    close(lfd);
    if (client < 0) return FDPASS_ERR_ACCEPT;

    char req;
    ssize_t ignored = read(client, &req, 1); /* 等客户端发来 1 字节请求，不校验内容 */
    (void)ignored;
    ssize_t sent = send_one_fd(client, fd); /* 原库这里传原始 fd，不 dup */
    close(client);
    return (int)sent;
}

int fdpass_current_listen_fd(void) {
    return atomic_load(&g_listen_fd);
}
