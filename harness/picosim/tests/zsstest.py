#!/usr/bin/env python3
"""picosim の 0xA7 (端末のセッション、net/npd/zss.h、zss_mock.c) の試験 (XEiJ なし)。
Z の側 (zos.x の sshs.c と同じ手順) をこの台本がやり、ssh のクライアントの代わりに平文の TCP の口 (ZSS_PORT) に入る。
  1. 接続すると OPEN (80x24、user test、TERM xterm、UTF-8) が来る。ACCEPT → 打ったものが INPUT で来る
  2. OUTPUT が接続に届く。CREDIT は 4096 まで
  3. 同じ zseq の要求には同じ応答 (送り直し)
  4. 0xFF の命令: WINCH / SIGNAL / EOF
  5. EXIT で接続に "[exit 5]" が書かれて閉じ、続いて CLOSED が来る
  6. 接続を切ると CLOSED
usage: zsstest.py [picosim のパス]"""
import os, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
PORT, ZPORT = 16872, 23224
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond:
        fails += 1

def rd(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b

def cmd(id, cdb, data=b"", path=1):
    global seq
    seq += 1
    s.sendall(b"PSQ1" + struct.pack(">IBBBBI", seq, id, path, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
    h = rd(16)
    assert h[:4] == b"PSA1" and struct.unpack(">I", h[4:8])[0] == seq
    st, key, asc, ascq, n = struct.unpack(">BBBBI", h[8:16])
    return st, (key, asc, ascq), rd(n)

def read10(id, lba, n): return cmd(id, struct.pack(">BBIBHB", 0x28, 0, lba, 0, n, 0))
def write10(id, lba, data): return cmd(id, struct.pack(">BBIBHB", 0x2a, 0, lba, 0, len(data) // 512, 0), data)


env = dict(os.environ, ZSS_PORT=str(ZPORT))
p = subprocess.Popen([PICOSIM, "-l", "127.0.0.1:%d" % PORT, "-L", "none"], env=env, stderr=open(os.path.join(HERE, "zsstest.log"), "w"))
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", PORT)); break
    except OSError:
        time.sleep(0.1)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
seq = 0

ZIP = socket.inet_aton("192.168.1.68")
GW = socket.inet_aton("192.168.1.1")
zseq = 1000
zsect = 0x1ffff0

def xchg(records=b"", first=8):
    """要求を書き、応答を新しい LBA から読む (npd.x の zxchg)。戻り値: (見出し, データ)"""
    global zseq, zsect
    zseq += 1
    req = struct.pack(">IIHHI", 0x58364e52, zseq, len(records), 4080, struct.unpack(">I", ZIP)[0]) + records
    req = req.ljust((len(req) + 511) // 512 * 512, b"\0")
    st, _, _ = write10(6, 0x100000, req)
    assert st == 0
    for _ in range(64):
        zsect -= 8
        st, _, r = read10(6, zsect, first)
        magic, rseq, n, more, stat = struct.unpack(">IIHHI", r[:16])
        if magic == 0x58364e52 and rseq == zseq:
            break
    else:
        raise RuntimeError("no fresh reply")
    if 16 + n > first * 512:
        _, _, r2 = read10(6, zsect + first, (16 + n - 1) // 512 + 1 - first)
        r += r2
    return (n, more, stat), r[16:16 + n]

def recs(data):
    out = []
    while len(data) >= 16:
        n = struct.unpack(">H", data[:2])[0]
        out.append(data[16:16 + n])
        data = data[16 + ((n + 1) & ~1):]
    return out

def zrec(pkt):
    return struct.pack(">H", len(pkt)) + pkt + (b"\0" if len(pkt) & 1 else b"")


def recs(data):
    out = []
    while len(data) >= 16:
        n = struct.unpack(">H", data[:2])[0]
        out.append(data[16:16 + n])
        data = data[16 + ((n + 1) & ~1):]
    return out

def zrec(pkt):
    return struct.pack(">H", len(pkt)) + pkt + (b"\0" if len(pkt) & 1 else b"")

ZEPOCH, aseq = 0x12345678, 0
def msg(t, sess, gen, body=b""):
    return struct.pack(">BBBBH", t, sess, gen, 0, len(body)) + body + (b"\0" if len(body) & 1 else b"")
def areq(msgs=b"", same=False):
    """0xA7 の要求を 1 個送って応答のメッセージの列を返す。same = 前と同じ zseq で送り直す"""
    global aseq
    if not same: aseq += 1
    r = struct.pack(">BBBBHHI", 0xa7, 1, 0, 0, aseq, 0, ZEPOCH) + msgs
    for rec in recs(xchg(zrec(r))[1]):
        if rec[:1] == b"\xa7":
            assert struct.unpack(">H", rec[4:6])[0] == aseq
            out, m = [], rec[12:]
            while len(m) >= 6:
                t, se, g, _, n = struct.unpack(">BBBBH", m[:6])
                out.append((t, se, g, m[6:6 + n])); m = m[6 + ((n + 1) & ~1):]
            return rec, out
    return None, []

def until(pred, n=50):
    got = []
    for _ in range(n):
        _, ms = areq()
        got += ms
        if pred(got): return got
        time.sleep(0.02)
    return got

_, ms = areq()
check("0xA7 answered with no session", ms == [] or all(t != 0x81 for t, *_ in ms))
c = socket.create_connection(("127.0.0.1", ZPORT)); c.settimeout(3)
got = until(lambda g: any(t == 0x81 for t, *_ in g))
op = [m for m in got if m[0] == 0x81]
check("connect -> OPEN", len(op) == 1)
_, sess, gen, body = op[0] if op else (0, 0, 0, b"\0" * 20)
cols, rows = struct.unpack(">HH", body[:4])
check("OPEN 80x24 user test TERM xterm UTF-8", (cols, rows) == (80, 24) and b"test" in body and b"xterm" in body and body[-1] == 1)
credit = [struct.unpack(">I", b)[0] for t, se, g, b in got if t == 0x87]
check("CREDIT <= 4096", credit and max(credit) <= 4096, str(credit[:3]))
c.sendall(b"hello")
got = until(lambda g: any(t == 0x82 for t, *_ in g), 10)
check("no INPUT before ACCEPT", not any(t == 0x82 for t, *_ in got))
_, ms = areq(msg(0x01, sess, gen, struct.pack(">I", 2048)))
got = ms + until(lambda g: any(t == 0x82 for t, *_ in g))
inp = b"".join(b for t, se, g, b in got if t == 0x82)
check("ACCEPT -> INPUT", inp == b"hello", repr(inp))
areq(msg(0x03, sess, gen, b"out-to-client"))
d = c.recv(100)
check("OUTPUT reaches the client", d == b"out-to-client", repr(d))
c.sendall(b"xyz")
r1, m1 = None, []
for _ in range(30):
    r1, m1 = areq()
    if any(t == 0x82 for t, *_ in m1): break
    time.sleep(0.02)
r2, m2 = areq(same=True)
check("same zseq -> same response (resend)", r1 is not None and r1 == r2)
c.sendall(b"\xffW 100 30\n\xffS INT\n\xffE\n")
got = until(lambda g: any(t == 0x85 for t, *_ in g))
w = [struct.unpack(">HH", b) for t, se, g, b in got if t == 0x83]
check("WINCH 100x30", w == [(100, 30)], str(w))
check("SIGNAL INT", any(t == 0x84 and b == b"INT" for t, se, g, b in got))
check("EOF", any(t == 0x85 for t, *_ in got))
got = [m for m in areq(msg(0x05, sess, gen, struct.pack(">I", 5)))[1]]
d = b""
try:
    while True:
        x = c.recv(100)
        if not x: break
        d += x
except socket.timeout: pass
check("EXIT -> client sees [exit 5] and the connection closes", b"[exit 5]" in d, repr(d))
got += until(lambda g: any(t == 0x86 for t, *_ in g), 10)
check("CLOSED after EXIT", any(t == 0x86 for t, *_ in got))
c2 = socket.create_connection(("127.0.0.1", ZPORT))
got = until(lambda g: any(t == 0x81 for t, *_ in g))
op2 = [m for m in got if m[0] == 0x81]
check("next connection reuses sess with gen+1", op2 and op2[0][1] == sess and op2[0][2] == (gen + 1) & 0xff)
areq(msg(0x01, sess, (gen + 1) & 0xff, struct.pack(">I", 2048)))
c2.close()
got = until(lambda g: any(t == 0x86 for t, *_ in g))
check("disconnect -> CLOSED", any(t == 0x86 for t, *_ in got))
p.terminate()
print("PASS zsstest" if fails == 0 else "FAIL zsstest (%d)" % fails)
sys.exit(1 if fails else 0)
