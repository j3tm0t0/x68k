#!/usr/bin/env python3
"""XEiJ の RS-232C (TCP/IP 接続) を ssh 経由で Raspberry Pi の pppd に橋渡しする (PPP の動作確認用)。
X68000 Z 実機では UART 直結 + templates/raspi/ppp-x68k.sh を使う。これはその代わりに
Pi 側で `pppd notty` (標準入出力を PPP 回線にする) を起動し、XEiJ の AUX とつなぐ。
XEiJ 側: scripts/run.sh -tcpiphost=127.0.0.1 -tcpipport=54321 -tcpiputf8=off -rs232cconnection='TCP%2FIP+%E2%87%94+AUX'
usage: xeijppp.py HOST PORT SSHHOST LOGFILE [LOCALIP REMOTEIP]
  LOGFILE には X> (XEiJ→Pi) / R> (Pi→XEiJ) の生データと pppd の stderr を記録する"""
import os, sys, socket, select, subprocess, time
host, port, sshhost, logfile = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
local = sys.argv[5] if len(sys.argv) > 5 else "192.168.68.1"
remote = sys.argv[6] if len(sys.argv) > 6 else "192.168.68.2"
log = open(logfile, "ab", 0)
for i in range(60):
    try:
        s = socket.create_connection((host, port)); break
    except OSError:
        time.sleep(1)
else:
    sys.exit("connect failed")
log.write(b"== connected\n")
# NAT: X68000 からのパケットを Pi の外側へ出す (templates/raspi/ppp-x68k.sh と同じ)
nat = (f"sudo sysctl -q -w net.ipv4.ip_forward=1; "
       f"sudo iptables -t nat -C POSTROUTING -s {remote} -j MASQUERADE 2>/dev/null || "
       f"sudo iptables -t nat -A POSTROUTING -s {remote} -j MASQUERADE; ")
pppd = (f"exec sudo pppd notty noauth nodetach novj noccp noipv6 nodefaultroute "
        f"lcp-echo-interval 15 lcp-echo-failure 4 {local}:{remote}")
p = subprocess.Popen(["ssh", sshhost, nat + pppd], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(logfile, "ab", 0), bufsize=0)
fds = [s.fileno(), p.stdout.fileno()]
while p.poll() is None:
    r, _, _ = select.select(fds, [], [], 1.0)
    for fd in r:
        if fd == s.fileno():
            d = s.recv(4096)
            if not d: log.write(b"== xeij closed\n"); p.terminate(); sys.exit()
            log.write(b"X>" + d.hex().encode() + b"\n"); p.stdin.write(d)
        else:
            d = os.read(fd, 4096)
            if not d: continue
            log.write(b"R>" + d.hex().encode() + b"\n"); s.sendall(d)
log.write(f"== pppd exited {p.returncode}\n".encode())
