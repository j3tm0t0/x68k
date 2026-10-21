#!/usr/bin/env python3
"""XEiJ の RS-232C (TCP/IP 接続) を ssh 越しに Raspberry Pi の SLIP (slattach) につなぐ試験用ブリッジ。
Pi 側で疑似端末 (pty) を作って slattach をかけ、その master と XEiJ の AUX を中継する。
Pi 側は sl1 = 192.168.69.1 <-> X68000 (XEiJ) 192.168.69.2。X68000 側の netup は 192.168.68.x 固定なので、
XEiJ では SLIPMY=192.168.69.2 SLIPPEER=192.168.69.1 を設定してから netup する。
usage: xeijslip.py HOST PORT SSHHOST LOGFILE"""
import os, sys, socket, select, subprocess, time
host, port, sshhost, logfile = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
log = open(logfile, "ab", 0)
for _ in range(60):
    try: s = socket.create_connection((host, port)); break
    except OSError: time.sleep(1)
else: sys.exit("connect failed")
log.write(b"== connected\n")
# Pi 側で動かす中継スクリプト (ssh の引数で渡すと崩れるので、先にファイルとして置く)
remote = r'''
import os, pty, sys, select, subprocess, time
m, sl = pty.openpty()
name = os.ttyname(sl)
import tty; tty.setraw(sl)
p = subprocess.Popen(["sudo", "/usr/sbin/slattach", "-p", "slip", "-L", name], stdout=sys.stderr, stderr=sys.stderr)
time.sleep(2)
out = subprocess.run("ip -o link | grep -o 'sl[0-9]*' | sort | tail -1", shell=True, capture_output=True, text=True).stdout.strip()
for c in ["sysctl -q -w net.ipv6.conf.%s.disable_ipv6=1" % out, "ip addr flush dev %s" % out, "ip addr add 192.168.69.1 peer 192.168.69.2 dev %s" % out, "ip link set %s up" % out]:
    subprocess.run("sudo " + c, shell=True)
sys.stderr.write("slip on %s via %s\n" % (out, name)); sys.stderr.flush()
while True:
    r, _, _ = select.select([0, m], [], [], 1)
    if 0 in r:
        d = os.read(0, 4096)
        if not d: break
        os.write(m, d)
    if m in r:
        os.write(1, os.read(m, 4096))
p.terminate()
'''
subprocess.run(["ssh", sshhost, "cat > /tmp/xeijslip_remote.py"], input=remote.encode(), check=True)
p = subprocess.Popen(["ssh", sshhost, "python3", "-u", "/tmp/xeijslip_remote.py"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=open(logfile, "ab", 0), bufsize=0)
fds = [s.fileno(), p.stdout.fileno()]
while p.poll() is None:
    r, _, _ = select.select(fds, [], [], 1.0)
    for fd in r:
        if fd == s.fileno():
            d = s.recv(4096)
            if not d: p.terminate(); sys.exit()
            log.write(b"X>" + d.hex().encode() + b"\n"); p.stdin.write(d)
        else:
            d = os.read(fd, 4096)
            if d: log.write(b"R>" + d.hex().encode() + b"\n"); s.sendall(d)
