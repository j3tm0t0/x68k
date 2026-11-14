#!/usr/bin/env python3
"""picosim -Z (zl.c、zmux の線を SCSI ID6 に通す BlueSCSI のまね) の試験 (XEiJ なし)。zmux.sys /S と同じ手順で、
ID 6 の LBA 0x230000 を READ、0x220000 に WRITE し、反対側を zmuxd の代わりの TCP で読み書きする。Z キャッシュ (既定 64MB) 越し。
  1. zmuxd がいない間: state は「つながっていない」、WRITE は捨てられる、txfree は 1024
  2. つないだ後: X68000 → TCP、TCP → X68000 (1 セクタずつ、rxmore で 8 セクタ読み足し)、毎回新しい応答が読める
  3. txfree を超えた分は捨てる。F_START で送りかけを捨てる
  4. 新しい接続が古い接続を切る
usage: zltest.py [picosim]"""
import os, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
PORT, ZPORT = 16873, 16874
WLBA, RLBA = 0x220000, 0x230000
fails = 0


def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond:
        fails += 1


p = subprocess.Popen([PICOSIM, "-l", "127.0.0.1:%d" % PORT, "-L", "none", "-Z", str(ZPORT), "-v"],
                     stderr=open(os.path.join(HERE, "zl.log"), "a"))
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", PORT))
        break
    except OSError:
        time.sleep(0.1)
q = 0


def rd(n):
    b = b""
    while len(b) < n:
        b += s.recv(n - len(b))
    return b


def scsi(cdb, data=b""):
    global q
    q += 1
    s.sendall(b"PSQ1" + struct.pack(">IBBBBI", q, 6, 0, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
    h = rd(16)
    return rd(struct.unpack(">I", h[12:16])[0])


def zread(sect=1):
    a = scsi(struct.pack(">BBIBHB", 0x28, 0, RLBA, 0, sect, 0))
    n, txfree, more = struct.unpack(">HHH", a[4:10])
    return a[:4], a[16:16 + n], txfree, more, a[10]


def zwrite(data, flags=0):
    blk = b"X6ZW" + struct.pack(">HH", len(data), flags) + bytes(8) + data
    n = (len(blk) + 511) // 512
    scsi(struct.pack(">BBIBHB", 0x2a, 0, WLBA, 0, n, 0), blk.ljust(n * 512, b"\0"))


def readall(c, n, t=3):
    c.settimeout(t)
    b = b""
    try:
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d:
                break
            b += d
    except socket.timeout:
        pass
    return b


m, d, txfree, more, st = zread()
check("answer before zmuxd: magic, no data, not connected, txfree 1024", m == b"X6ZR" and d == b"" and st == 2 and txfree == 1024, "%r %d %d" % (m, st, txfree))
zwrite(b"dropped while nobody listens")

c = socket.create_connection(("127.0.0.1", ZPORT))
time.sleep(0.2)
m, d, txfree, more, st = zread()
check("connected", st == 3, "state %d" % st)
check("bytes written before the connection are not sent", readall(c, 1, 0.3) == b"")

msg = bytes(range(256)) * 3
zwrite(msg)
check("X68000 -> TCP (768 bytes in one WRITE)", readall(c, len(msg)) == msg)

big = os.urandom(5000)
c.sendall(big)
time.sleep(0.3)
got = b""
sizes = []
m, d, txfree, more, st = zread(1)
got += d
sizes.append(len(d))
while more:
    sect = min(8, (more + 16 + 511) // 512)
    m, d, txfree, more, st = zread(sect)
    got += d
    sizes.append(len(d))
check("TCP -> X68000 (5000 bytes: 1 sector, then up to 8 sectors by rxmore)", got == big and sizes[0] == 496 and max(sizes) == 4080, str(sizes))
m, d, txfree, more, st = zread()
check("nothing left, a fresh answer through the Z cache", d == b"" and more == 0)

c.setblocking(False)                    # TCP が読まないまま送り続けると txfree が減る (picosim は send が詰まるまで送るので、
c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)   # ここでは 1024 を超えて書いた分が捨てられることだけ見る)
zwrite(b"A" * 1500)
m, d, txfree, more, st = zread()
check("txfree after writing more than 1024", 0 <= txfree <= 1024, "txfree %d" % txfree)
c.setblocking(True)
got = readall(c, 1500, 1)
check("bytes over txfree are dropped", len(got) == 1024, "%d bytes arrived" % len(got))

c2 = socket.create_connection(("127.0.0.1", ZPORT))
time.sleep(0.2)
zread()
check("a new connection closes the old one", readall(c, 1, 1) == b"")
zwrite(b"to the new one")
check("bytes go to the new connection", readall(c2, 14) == b"to the new one")

s.close()
p.terminate()
p.wait()
print("zltest: %s (%d failures)" % ("PASS" if not fails else "FAIL", fails))
sys.exit(1 if fails else 0)
