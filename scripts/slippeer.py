#!/usr/bin/env python3
"""XEiJ の RS-232C (TCP/IP 接続) の向こうで SLIP の相手役をする試験用スクリプト。
X68000 (slip.sys) から来た IPv4 パケットを表示し、ICMP echo request には echo reply を返す。
XEiJ 側: scripts/run.sh -model=SUPER -tcpiphost=127.0.0.1 -tcpipport=54322 -tcpiputf8=off -rs232cconnection='TCP%2FIP+%E2%87%94+AUX'
usage: slippeer.py HOST PORT"""
import socket, struct, sys, time
END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD

def csum(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('!%dH' % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff

def slip_encode(p):
    out = bytearray([END])
    for c in p:
        out += bytes([ESC, ESC_END]) if c == END else bytes([ESC, ESC_ESC]) if c == ESC else bytes([c])
    return bytes(out + bytes([END]))

host, port = sys.argv[1], int(sys.argv[2])
for _ in range(60):
    try: s = socket.create_connection((host, port)); break
    except OSError: time.sleep(1)
else: sys.exit("connect failed")
print("connected", flush=True)
buf, esc = bytearray(), False
while True:
    d = s.recv(4096)
    if not d: break
    for c in d:
        if c == END:
            if len(buf) >= 20:
                p = bytes(buf)
                ver, proto = p[0] >> 4, p[9]
                src, dst = socket.inet_ntoa(p[12:16]), socket.inet_ntoa(p[16:20])
                ihl = (p[0] & 15) * 4
                print(f"rx {len(p)} bytes v{ver} proto {proto} {src} -> {dst}", flush=True)
                if proto == 1 and p[ihl] == 8:          # ICMP echo request -> reply
                    icmp = bytearray(p[ihl:]); icmp[0] = 0; icmp[2:4] = b'\0\0'
                    icmp[2:4] = struct.pack('!H', csum(bytes(icmp)))
                    ip = bytearray(p[:ihl]); ip[12:16], ip[16:20] = p[16:20], p[12:16]
                    ip[8] = 64; ip[10:12] = b'\0\0'; ip[10:12] = struct.pack('!H', csum(bytes(ip)))
                    s.sendall(slip_encode(bytes(ip) + bytes(icmp)))
                    print("  -> echo reply", flush=True)
            buf, esc = bytearray(), False
        elif c == ESC: esc = True
        else:
            if esc: c = END if c == ESC_END else ESC if c == ESC_ESC else c; esc = False
            buf.append(c)
