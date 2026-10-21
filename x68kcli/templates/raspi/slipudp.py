#!/usr/bin/env python3
# slipudp.py -- x68kzremotedrv (Pico W) の netpipe が UDP で送ってくる SLIP のバイト列を pty につなぐ。
# pty 側に slattach をかければ、シリアルのときと同じ sl0 として扱える (ppp-x68k.sh MODE=pico が使う)。
#   slipudp.py [リンクのパス (既定 /run/x68k-slip)] [UDP ポート (既定 6868)]
# 送り先 (Pico) は最後に UDP が届いたアドレス。Pico は Z が問い合わせるたび、2 秒に 1 回は 1 バイト送ってくる。
import os, select, socket, sys, termios, tty

link = sys.argv[1] if len(sys.argv) > 1 else "/run/x68k-slip"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 6868
CHUNK = 1400                            # Pico の UDP_CHUNK と同じ (IP の断片化を避ける)

master, slave = os.openpty()
tty.setraw(slave)
attr = termios.tcgetattr(slave)
attr[3] &= ~termios.ECHO
termios.tcsetattr(slave, termios.TCSANOW, attr)
try:
    os.unlink(link)
except FileNotFoundError:
    pass
os.symlink(os.ttyname(slave), link)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", port))
peer = None
print(f"slipudp: {link} -> {os.ttyname(slave)}, udp {port}", flush=True)
while True:
    r, _, _ = select.select([master, sock], [], [])
    if sock in r:
        data, addr = sock.recvfrom(4096)
        if addr != peer:
            print(f"slipudp: peer {addr[0]}:{addr[1]}", flush=True)
            peer = addr
        os.write(master, data)
    if master in r:
        data = os.read(master, 4096)
        if peer:
            for i in range(0, len(data), CHUNK):
                sock.sendto(data[i:i + CHUNK], peer)
