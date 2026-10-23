#!/usr/bin/env python3
"""Pico Z Loader (x68k-zos の branch pzl) の SCSI の層をホストで (build/picosim-pzl --pzl=config.txt)。
PZL の virtual_disk.c / storage.c / config_file.c はそのまま、Wi-Fi・SMB は picosim の代わり (SMB の共有 = 手元のディレクトリ)。
  1. 接続が済むまで ID 0 は NOT READY、INQUIRY は答える
  2. 開いたら: LBA 0 はファイルのまま、LBA 2 は起動 IPL (hdsboot)、LBA 0x20/0x21 はファイルの 2/3、READ CAPACITY はファイルの大きさ
  3. 書き込みがファイルに届く (WRITE(10) 2 セクタ)
  4. ID 6 はリモートの顔 (X68SCSI1 / X68000ZRemoteDrv)
  5. HDS1 に無いファイルを書くと、開けずに MEDIUM ERROR (03/11)
  6. 設定画面のコマンド GETINFO (unit = REMOTE_UNIT) と、B: (remoteserv): rpage のドライバの手順で OPEN / READ
  7. netpipe (X6NR): Z (192.168.1.68) からゲートウェイへ ping (PZL の netpipe.c が lwIP の Wi-Fi の代わりの netif で
     代理 ARP とブリッジ)、暗号アクセラレータ 0xA5 の PING
usage: pzltest.py [build/picosim-pzl]"""
import os, socket, struct, subprocess, sys, time, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim-pzl")
if not os.path.exists(PICOSIM):
    print("SKIP pzltest: no %s (make build/picosim-pzl)" % PICOSIM); sys.exit(0)
PORT = 16871
fails = 0
def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond: fails += 1
root = tempfile.mkdtemp()
os.makedirs(os.path.join(root, "share", "x68000z"))
os.makedirs(os.path.join(root, "share", "bdrive"))
bdata = os.urandom(40000)
open(os.path.join(root, "share", "bdrive", "TEST.BIN"), "wb").write(bdata)
img = bytearray(os.urandom(512 * 128)); img[0:8] = b"X68SCSI1"
hds = os.path.join(root, "share", "x68000z", "test.hds")
open(hds, "wb").write(img)
cfg = os.path.join(root, "config.txt")
open(cfg, "w").write("WIFI_SSID: picosim\nSMB2_SERVER: 192.168.1.1\nHDS0: share/x68000z/test.hds\nHDS1: share/x68000z/none.hds\n"
                     "REMOTE_UNIT: 1\nREMOTE0: share/bdrive\nTZ: JST-9\n")
p = subprocess.Popen([PICOSIM, "--pzl=" + cfg, "--pzl-root=" + root, "-l", "127.0.0.1:%d" % PORT, "-L", "none", "-T", "0.2,0.4,1", "-v"],
                     stderr=open(os.path.join(HERE, "pzltest.log"), "w"))
for _ in range(50):
    try: s = socket.create_connection(("127.0.0.1", PORT)); break
    except OSError: time.sleep(0.1)
q = [0]
def rd(n):
    b = b""
    while len(b) < n: b += s.recv(n - len(b))
    return b
def cmd(id, cdb, data=b""):
    q[0] += 1
    s.sendall(b"PSQ1" + struct.pack(">IBBBBI", q[0], id, 0, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
    h = rd(16); st, k, a, aq, n = struct.unpack(">BBBBI", h[8:16]); return st, (k, a, aq), rd(n)
def read(id, lba, n=1): return cmd(id, struct.pack(">BBIBHB", 0x28, 0, lba, 0, n, 0))

st, sense, _ = read(0, 0)
check("before the HDS opens: ID0 NOT READY", st == 2 and sense == (2, 4, 1))
st, _, d = cmd(0, bytes([0x12, 0, 0, 0, 36, 0]))
check("INQUIRY answered while not ready", st == 0 and len(d) == 36)
time.sleep(1.3)
st, _, d = read(0, 0, 4)
check("ID0 LBA 0 = the file", st == 0 and d[:512] == img[:512])
check("ID0 LBA 2 = boot IPL (hdsboot), not the file", d[1024:1536] != img[1024:1536] and any(d[1024:1536]))
st, _, d = read(0, 0x20, 2)
check("ID0 LBA 0x20/0x21 = file LBA 2/3", st == 0 and d == img[1024:2048])
st, _, d = cmd(0, bytes([0x25] + [0] * 9))
check("READ CAPACITY = file size", st == 0 and struct.unpack(">I", d[:4])[0] == 127, "last LBA %d" % struct.unpack(">I", d[:4])[0])
data = os.urandom(1024)
st, _, _ = cmd(0, struct.pack(">BBIBHB", 0x2a, 0, 40, 0, 2, 0), data)
check("WRITE(10) 2 sectors reach the file", st == 0 and open(hds, "rb").read()[40 * 512:42 * 512] == data)
st, _, d = read(6, 0)
check("ID6 LBA 0 = remote drive face", d[:8] == b"X68SCSI1" and d[16:32] == b"X68000ZRemoteDrv")
st, sense, _ = read(1, 0)
check("HDS1 missing file: MEDIUM ERROR", st == 2 and sense[0] == 3, "sense %s" % (sense,))

# ---- 6. vdbuf (scsiremote.c の com_cmdres、rpage) ----
st6 = {"sect": 0x400000, "seq": 0}
def cmdres(w, rsize, session=0x1234abcd):
    wcnt = (len(w) - 1) // 496
    rsect = ((st6["sect"] - 8) % 0x200000) + 0x200000
    res = bytes([0x80 | ((rsect >> 11) & 0x1f), (rsect >> 3) & 0xff])
    for i in range(wcnt + 1):
        hdr = b"X68Z" + struct.pack(">II", session, st6["seq"]) + bytes([i, wcnt]) + res
        cmd(6, struct.pack(">BBIBHB", 0x2a, 0, 0x20, 0, 1, 0), (hdr + w[i * 496:(i + 1) * 496]).ljust(512, b"\0"))
    st6["sect"] = rsect
    out = b""; i = 0; rcnt = (rsize - 1) // 496
    while i <= rcnt:
        for _ in range(64):
            _, _, b = read(6, st6["sect"] + (i & 7))
            if b[:12] == hdr[:12] and b[12] == i: break
            st6["sect"] = ((st6["sect"] - 0x10000) % 0x200000) + 0x200000
        out += b[16:]; rcnt = b[13]
        if (i & 7) == 7: st6["sect"] = ((st6["sect"] - 8) % 0x200000) + 0x200000
        i += 1
    st6["seq"] += 1
    return out[:rsize]
r = cmdres(struct.pack(">H", 0xff00), 26)
check("GETINFO: unit = REMOTE_UNIT (1), version", r[7] == 1 and r[8] == 1, "unit=%d ver=%d" % (r[7], r[8]))
r = cmdres(bytes([0x00]), 1)
check("B: INIT", r[0] == 0)
nb = bytes([0, 0]) + b"\t".ljust(65, b"\0") + b"TEST    " + b"BIN" + bytes(10)
r = cmdres(bytes([0x0a, 0, 0, 0]) + struct.pack(">I", 0x1000) + nb, 8)
check("B: OPEN TEST.BIN", r[0] == 0 and struct.unpack(">I", r[4:8])[0] == len(bdata), "res=%d" % (r[0] - 256 if r[0] > 127 else r[0]))
got = b""
for pos, n in ((0, 15360), (15360, 11612), (26972, 13028)):
    r = cmdres(bytes([0x4c, 0, 0, 0]) + struct.pack(">IIH", 0x1000, pos, n), 2 + n)
    got += r[2:2 + struct.unpack(">h", r[:2])[0]]
check("B: READ 15360 + 11612 (24-page reply) + 13028 = the file", got == bdata)

# ---- 7. netpipe ----
ZIP = socket.inet_aton("192.168.1.68"); GW = socket.inet_aton("192.168.1.1")
zst = {"seq": 5000, "sect": 0x1ffff0}
def xchg(records=b""):
    zst["seq"] += 1
    req = struct.pack(">IIHHI", 0x58364e52, zst["seq"], len(records), 4080, struct.unpack(">I", ZIP)[0]) + records
    req = req.ljust((len(req) + 511) // 512 * 512, b"\0")
    cmd(6, struct.pack(">BBIBHB", 0x2a, 0, 0x100000, 0, len(req) // 512, 0), req)
    for _ in range(64):
        zst["sect"] -= 8
        _, _, r = read(6, zst["sect"], 8)
        magic, rseq, n, more, stat = struct.unpack(">IIHHI", r[:16])
        if magic == 0x58364e52 and rseq == zst["seq"]:
            return r[16:16 + n]
    return b""
def recs(data):
    out = []
    while len(data) >= 16:
        n = struct.unpack(">H", data[:2])[0]; out.append(data[16:16 + n]); data = data[16 + ((n + 1) & ~1):]
    return out
def zrec(pkt): return struct.pack(">H", len(pkt)) + pkt + (b"\0" if len(pkt) & 1 else b"")
def csum(b):
    if len(b) & 1: b += b"\0"
    t = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while t >> 16: t = (t & 0xffff) + (t >> 16)
    return ~t & 0xffff
def ipv4(proto, src, dst, payload):
    h = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 1, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack(">H", csum(h)) + h[12:] + payload
icmp = struct.pack(">BBHHH", 8, 0, 0, 0x4242, 1) + b"pzl-netpipe" * 4
icmp = icmp[:2] + struct.pack(">H", csum(icmp)) + icmp[4:]
xchg()                                   # 最初の交換で netpipe が開き、Z の IP を名乗る
got = None
pending = [xchg(zrec(ipv4(1, ZIP, GW, icmp)))]
for _ in range(100):
    data = pending.pop() if pending else xchg()
    for pkt in recs(data):
        if len(pkt) >= 28 and pkt[9] == 1 and pkt[20] == 0 and pkt[24:26] == b"\x42\x42": got = pkt
    if got: break
    time.sleep(0.02)
check("netpipe: ping 192.168.1.1 through PZL's bridge", got is not None and got[28:] == icmp[8:])
res = None
pending = [xchg(zrec(bytes([0xa5, 0, 0x00, 0x09])))]
for _ in range(50):
    data = pending.pop() if pending else xchg()
    for pkt in recs(data):
        if pkt and pkt[0] == 0xa5: res = pkt
    if res: break
    time.sleep(0.02)
check("accel 0xA5 PING (PZL accel.c, core 1 = a thread)", res is not None and res[2:4] == b"\x00\x09" and res[4] == 0,
      "caps=0x%02x" % res[5] if res else "")
s.close(); p.terminate(); p.wait()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
