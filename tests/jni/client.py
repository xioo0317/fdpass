#!/usr/bin/env python3
"""模拟魔改 libfastboot.so：connect 抽象 socket → 发 1 字节 → recvmsg(SCM_RIGHTS) 收 fd。"""
import array
import os
import socket
import sys
import time

name = sys.argv[1]
addr = "\0" + name[1:]  # Linux 抽象命名空间

for attempt in range(2):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ok = False
    for _ in range(100):
        try:
            s.connect(addr)
            ok = True
            break
        except OSError:
            time.sleep(0.02)
    if not ok:
        sys.exit("connect failed")

    s.sendall(b"Q")
    fds = array.array("i")
    msg, ancdata, _flags, _addr = s.recvmsg(1, socket.CMSG_SPACE(fds.itemsize))
    if msg != b"\x66":
        sys.exit(f"数据字节错误: {msg!r}，应为 0x66")

    for level, typ, data in ancdata:
        if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            usable = len(data) - (len(data) % fds.itemsize)
            fds.frombytes(data[:usable])
    if len(fds) != 1:
        sys.exit(f"SCM_RIGHTS 未收到 1 个 fd: {len(fds)}")

    fd = fds[0]
    st = os.fstat(fd)  # fd 必须是有效内核对象
    print(f"client#{attempt + 1}: byte=0x{msg[0]:02x} fd={fd} fstat(mode={st.st_mode:o}) OK")
    os.close(fd)
    s.close()

print("PYTHON_CLIENT_OK")
