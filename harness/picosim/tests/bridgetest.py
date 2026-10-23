#!/usr/bin/env python3
"""picosim の試験 (XEiJ なし): PicoBridge.java と同じ形式で SCSI の命令を送り、netpipe の交換を npd.x と同じ手順で行う。
  1. ID 6 の INQUIRY / READ CAPACITY / REQUEST SENSE、LBA 0 が 0
  2. Z の Linux のキャッシュ: 同じ LBA を 2 回読むと前の中身 (交換を挟んでも)
  3. X6NR で Z (192.168.1.68) からゲートウェイ 192.168.1.1 に ping → echo reply がレコードで返る
  4. 暗号アクセラレータ 0xA5 の PING → caps
  5. Mac の 127.0.0.1:2323 に接続 → Z 宛ての SYN がレコードで届く (slirp の転送)
  6. LAN から Z 宛ての ping は Pico が代わりに返す (Z には渡らない) ... は外から ping できないので省く
usage: bridgetest.py [picosim のパス]   (picosim を -L 23230:2323 で起動して終わらせる)"""
import os, socket, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
PORT, FWD = 16868, 23230
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond:
        fails += 1

p = subprocess.Popen([PICOSIM] + (["-vvv", "-p", "/tmp/bt.pcap"] if os.environ.get("DBG") else []) + ["-l", "127.0.0.1:%d" % PORT, "-L", "%d:2323" % FWD], stderr=open(os.path.join(HERE, "picosim.log"), "w"))
for _ in range(50):
    try:
        s = socket.create_connection(("127.0.0.1", PORT)); break
    except OSError:
        time.sleep(0.1)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
seq = 0

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

st, _, d = cmd(6, bytes([0x12, 0, 0, 0, 36, 0]), path=0)
check("inquiry id6", st == 0 and d[8:16] == b"PICOSIM " and len(d) == 36)
st, _, d = cmd(6, bytes([0x25] + [0] * 9), path=0)
check("read capacity id6", st == 0 and struct.unpack(">II", d) == (0x3fffff, 512))
st, _, d = read10(6, 0, 1)
check("lba 0 is zero (not a Human68k disk)", st == 0 and d == bytes(512))
st, sense, _ = cmd(2, bytes([0x00] * 6), path=0)
check("absent id -> check condition", st == 2 and sense[0] == 5)
st, _, d = cmd(6, bytes([0x03, 0, 0, 0, 18, 0]), path=0)
check("request sense", st == 0 and len(d) == 18 and d[0] == 0x70)

# ---- npd.x と同じ交換 ----
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

def csum(b):
    if len(b) & 1: b += b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff

def ipv4(proto, src, dst, payload, ident=1):
    h = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, 0, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack(">H", csum(h)) + h[12:]
    return h + payload

# キャッシュ: 同じ LBA を読み直すと前の応答 (seq が古い)
h, _ = xchg()
lba_prev = zsect
_, _, r = read10(6, lba_prev, 1)
h2, _ = xchg()
_, _, r2 = read10(6, lba_prev, 1)
check("Z read cache returns the old reply for a re-read LBA", r2[:16] == r[:16] and struct.unpack(">I", r2[4:8])[0] == zseq - 1)

icmp = struct.pack(">BBHHH", 8, 0, 0, 0x1234, 1) + b"picosim-ping" * 4
icmp = icmp[:2] + struct.pack(">H", csum(icmp)) + icmp[4:]
t0 = time.time()
got = None
recs_in = [xchg(zrec(ipv4(1, ZIP, GW, icmp)))[1]]     # slirp はその場で答えるので、送った交換の応答に載ることもある
for _ in range(50):
    data = recs_in.pop() if recs_in else xchg()[1]
    for pkt in recs(data):
        if os.environ.get("DBG"): print("rec", pkt[:32].hex())
        if len(pkt) >= 28 and pkt[9] == 1 and pkt[20] == 0 and pkt[24:26] == b"\x12\x34":
            got = pkt
    if got: break
    time.sleep(0.01)
check("ping 192.168.1.1 via slirp", got is not None and got[12:16] == GW and got[28:] == icmp[8:],
      "%.1f ms" % ((time.time() - t0) * 1000))

# DNS: ゲートウェイの 53 番に聞く (zshrc の NETDNS の既定)。slirp の DNS の口に付け替えられ、Mac の resolver が答える。
# 返事の送り元はゲートウェイに戻っていること
q = struct.pack(">HHHHHH", 0x4242, 0x0100, 1, 0, 0, 0) + b"\x09localhost\x00" + struct.pack(">HH", 1, 1)
udp = struct.pack(">HHHH", 2050, 53, 8 + len(q), 0) + q
dns = None
t0 = time.time()
recs_in = [xchg(zrec(ipv4(17, ZIP, GW, udp)))[1]]
for _ in range(150):
    data = recs_in.pop() if recs_in else xchg()[1]
    for pkt in recs(data):
        if len(pkt) > 40 and pkt[9] == 17 and pkt[20:22] == b"\x00\x35" and pkt[28:30] == b"\x42\x42":
            dns = pkt
    if dns: break
    time.sleep(0.02)
check("DNS via gateway:53", dns is not None and dns[12:16] == GW and (dns[31] & 15) == 0,
      "%.0f ms, rcode=%d, answers=%d" % ((time.time() - t0) * 1000, dns[31] & 15, struct.unpack(">H", dns[34:36])[0]) if dns else "no reply")

# 仮想の LAN の逆引きは Mac に聞かずに picosim が名付ける
q = struct.pack(">HHHHHH", 0x4343, 0x0100, 1, 0, 0, 0) + b"\x011\x011\x03168\x03192\x07in-addr\x04arpa\x00" + struct.pack(">HH", 12, 1)
udp = struct.pack(">HHHH", 2051, 53, 8 + len(q), 0) + q
ptr = None
recs_in = [xchg(zrec(ipv4(17, ZIP, GW, udp)))[1]]
for _ in range(50):
    data = recs_in.pop() if recs_in else xchg()[1]
    for pkt in recs(data):
        if len(pkt) > 40 and pkt[9] == 17 and pkt[28:30] == b"\x43\x43":
            ptr = pkt
    if ptr: break
check("PTR 192.168.1.1 -> gateway.picosim", ptr is not None and b"\x07gateway\x07picosim" in ptr)

# 暗号アクセラレータ PING
xchg(zrec(bytes([0xa5, 0, 0x00, 0x07])))
res = None
for _ in range(20):
    _, data = xchg()
    for pkt in recs(data):
        if pkt and pkt[0] == 0xa5:
            res = pkt
    if res: break
check("accel ping", res is not None and res[1] == 0 and res[2:4] == b"\x00\x07" and res[4] == 0,
      "caps=0x%02x" % res[5] if res else "")

# サービス (picosim-echo の 0xA6): 要求のレコードがそのまま、その交換の応答に載る。'M' なら more が立つ
if "echo" in os.path.basename(PICOSIM):
    body = bytes([0xa6, ord("M")]) + b"tistack-seam"
    (n, more, _), data = xchg(zrec(body))
    r = [x for x in recs(data) if x and x[0] == 0xa6]
    check("service 0xA6 echoed in the same exchange", r == [body], "more=%d" % more)
    check("service more -> header more", more >= 1)
    (n, more, _), data = xchg(zrec(bytes([0xa6, 0]) + b"x" * 3001))
    r = [x for x in recs(data) if x and x[0] == 0xa6]
    check("service 3003-byte reply", len(r) == 1 and len(r[0]) == 3003 and more == 0)

# Mac -> Z の転送 (rcmd.x の 2323): SYN が Z に届く
c = socket.socket()
c.setblocking(False)
try: c.connect(("127.0.0.1", FWD))
except BlockingIOError: pass
syn = None
for _ in range(100):
    _, data = xchg()
    for pkt in recs(data):
        if len(pkt) >= 40 and pkt[9] == 6 and pkt[16:20] == ZIP and struct.unpack(">H", pkt[22:24])[0] == 2323 and pkt[33] & 2:
            syn = pkt
    if syn: break
    time.sleep(0.01)
check("Mac 127.0.0.1:%d -> Z:2323 SYN" % FWD, syn is not None)
c.close()

s.close()
p.terminate()
p.wait()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
