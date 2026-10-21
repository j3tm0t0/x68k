#!/usr/bin/env python3
"""XEiJ の RS-232C を TCP/IP 経由で `ssh -tt SSHHOST` に橋渡しする (シリアルログインの動作確認用)。
XEiJ 側: harness/xeij/run.sh -tcpiphost=127.0.0.1 -tcpipport=54321 -tcpiputf8=off -rs232cconnection='TCP%2FIP+%E2%87%94+AUX'
  (rs232cconnection は「TCP/IP ⇔ AUX」を URL エンコードしたもの。XEiJ が TCP サーバになる)
usage: xeijserial.py HOST PORT SSHHOST LOGFILE   (LOGFILE には X> 送信 / R> 受信 を記録)"""
import os, sys, socket, select, subprocess, time
host, port, sshhost, logfile = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
log = open(logfile, "ab", 0)
for i in range(60):
    try:
        s = socket.create_connection((host, port)); break
    except OSError:
        time.sleep(1)
else:
    sys.exit("connect failed")
log.write(b"== connected\n")
p = subprocess.Popen(["ssh", "-tt", sshhost], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
fds = [s.fileno(), p.stdout.fileno()]
while p.poll() is None:
    r, _, _ = select.select(fds, [], [], 1.0)
    for fd in r:
        if fd == s.fileno():
            d = s.recv(4096)
            if not d: log.write(b"== xeij closed\n"); p.terminate(); sys.exit()
            log.write(b"X>" + d + b"\n"); p.stdin.write(d)
        else:
            d = os.read(fd, 4096)
            if not d: continue
            log.write(b"R>" + d + b"\n"); s.sendall(d)
