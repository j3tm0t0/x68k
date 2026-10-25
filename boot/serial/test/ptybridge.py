#!/usr/bin/env python3
"""疑似端末 (pty) と XEiJ の RS-232C (TCP) をつなぐ。sx.py の --port (termios) の経路を XEiJ で試すため。
usage: ptybridge.py HOST:PORT LINKFILE
  開いた pty の名前 (/dev/ttysNNN) を LINKFILE に書く。sx.py --port $(cat LINKFILE) で使う。"""
import os, select, socket, sys, time, tty
host, port = sys.argv[1].rsplit(":", 1)
m, s = os.openpty()
tty.setraw(s)
name = os.ttyname(s)
for _ in range(60):
    try:
        so = socket.create_connection((host, int(port))); break
    except OSError:
        time.sleep(1)
else:
    sys.exit("ptybridge: cannot connect")
open(sys.argv[2], "w").write(name)
while True:
    r, _, _ = select.select([m, so], [], [], 1.0)
    if m in r:
        try:
            d = os.read(m, 4096)
        except OSError:      # 相手 (sx.py) が閉じた直後は EIO になることがある
            time.sleep(0.05); continue
        if d: so.sendall(d)
    if so in r:
        d = so.recv(4096)
        if not d: sys.exit("ptybridge: XEiJ closed")
        os.write(m, d)
