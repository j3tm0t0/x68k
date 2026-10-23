#!/usr/bin/env python3
"""tisnet_peer.py -- tisnet.x (Z) の相手 (Mac の 127.0.0.1:PORT、Z からは picosim のゲートウェイ 192.168.1.1:PORT)

  PUT n\\n + n バイト -> 型を照合して "OK n bad b\\n"
  GET n\\n           -> 型の n バイトを送って閉じる
  SINK n\\n / SRC n\\n -> 中身を見ない (速さだけ: Z の側で型を作る / 照合する手間を除く)
  ECHO\\n            -> 以後のバイトをそのまま返す
"""
import socket
import sys
import threading


def pat(k):
    return (k * 7 + (k >> 8) + (k >> 16)) & 255


def pattern(off, n):
    return bytes(pat(off + i) for i in range(n))


def handle(c):
    f = c.makefile('rb')
    line = f.readline().decode(errors='replace').strip()
    if line.startswith('PUT '):
        n = int(line[4:])
        got = bad = 0
        while got < n:
            b = f.read1(min(65536, n - got))
            if not b:
                break
            exp = pattern(got, len(b))
            bad += sum(1 for x, y in zip(b, exp) if x != y)
            got += len(b)
        c.sendall(b'OK %d bad %d\n' % (got, bad))
    elif line.startswith('GET '):
        n = int(line[4:])
        off = 0
        while off < n:
            m = min(65536, n - off)
            c.sendall(pattern(off, m))
            off += m
    elif line.startswith('SINK '):
        n = int(line[5:])
        got = 0
        while got < n:
            b = f.read1(min(65536, n - got))
            if not b:
                break
            got += len(b)
        c.sendall(b'OK %d\n' % got)
    elif line.startswith('SRC '):
        n = int(line[4:])
        z = bytes(65536)
        while n > 0:
            c.sendall(z[:min(n, 65536)])
            n -= min(n, 65536)
    elif line == 'ECHO':
        while True:
            b = f.read1(4096)
            if not b:
                break
            c.sendall(b)
    print('peer:', line, flush=True)
    c.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port))
    s.listen(8)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == '__main__':
    main()
