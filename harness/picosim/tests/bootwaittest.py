#!/usr/bin/env python3
"""picosim の Pico Z Loader の部分の試験 (XEiJ なし)。PicoBridge.java と同じ形式で、Z の待ち IPL・設定画面と同じ読み書きをする。
  1. 控え無し: ID 0 は NOT READY → HDS が開くと読める、控えのファイルができる
  2. 控えあり (電源の入れ直し): ID 0 の LBA 2 が待ち IPL の前半、ID 6 の 0x80 が後半、BW_BAND の答えが WAIT → READY、
     READY ならスロットの 1-2 セクタが本物の IPL で和が合う
  3. HDS の先頭 16KB を変えて電源を入れ直す: 控えを返したあと開くと CHANGED、控えは書き直される
  4. 設定画面の vdbuf (GETSTATUS / GETCONFIG)、ID 6 の LBA 0 の署名、settingui
usage: bootwaittest.py [picosim] [driver/ のディレクトリ]"""
import os, socket, struct, subprocess, sys, time, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
FW = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/src/github.com/yunkya2/x68kzremotedrv-hid/driver")
if not os.path.exists(os.path.join(FW, "bootwait1.bin")):
    print("SKIP: no firmware pieces in %s" % FW); sys.exit(0)
PORT = 16869
fails = 0
tmp = tempfile.mkdtemp()
HDS = os.path.join(tmp, "test.hds")
SNAP = HDS + ".bootsnap"
img = bytearray(os.urandom(512 * 64))
img[0:8] = b"X68SCSI1"
open(HDS, "wb").write(img)

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond: fails += 1

class Sim:
    def __init__(self, T):
        self.p = subprocess.Popen([PICOSIM, "-l", "127.0.0.1:%d" % PORT, "-L", "none", "-0", HDS, "-T", T, "-F", FW, "-v"],
                                  stderr=open(os.path.join(HERE, "bootwait.log"), "a"))
        for _ in range(50):
            try: self.s = socket.create_connection(("127.0.0.1", PORT)); break
            except OSError: time.sleep(0.1)
        self.seq = 0
        self.t0 = time.time()
    def rd(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c: raise EOFError
            b += c
        return b
    def cmd(self, id, cdb, data=b""):
        self.seq += 1
        self.s.sendall(b"PSQ1" + struct.pack(">IBBBBI", self.seq, id, 0, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
        h = self.rd(16)
        st, k, a, q, n = struct.unpack(">BBBBI", h[8:16])
        return st, (k, a, q), self.rd(n)
    def read(self, id, lba, n=1): return self.cmd(id, struct.pack(">BBIBHB", 0x28, 0, lba, 0, n, 0))
    def write(self, id, lba, data): return self.cmd(id, struct.pack(">BBIBHB", 0x2a, 0, lba, 0, len(data) // 512, 0), data)
    def close(self):
        self.s.close(); self.p.terminate(); self.p.wait()

slot = [0]
def ask(sim):
    slot[0] += 1
    lba = 0x40000 + (slot[0] % 12288) * 64
    st, _, d = sim.read(6, lba, 3)
    return d

bw1 = open(os.path.join(FW, "bootwait1.bin"), "rb").read()
bw2 = open(os.path.join(FW, "bootwait.bin"), "rb").read()

# 1. 控え無し
sim = Sim("0.2,0.4,1")
st, sense, _ = sim.read(0, 0)
check("no snapshot: ID0 NOT READY", st == 2 and sense == (2, 4, 1))
time.sleep(1.2)
st, _, d = sim.read(0, 0, 4)
check("HDS open: ID0 readable", st == 0 and d[:512] == img[:512])
sim.close()
check("snapshot file written", os.path.exists(SNAP) and os.path.getsize(SNAP) == 4096 + 16384)

# 2. 控えあり
sim = Sim("0.3,0.6,1.5")
st, _, d = sim.read(0, 0, 4)
check("snapshot served before HDS open", st == 0 and d[:512] == img[:512] and d[1024:1024 + len(bw1)] == bw1 and d[1536:2048] == bytes(512))
st, _, d = sim.read(6, 0x80, 32)
check("stage 2 at ID6 0x80", st == 0 and d[:len(bw2)] == bw2 and d[:4] == b"BW2!")
r = ask(sim)
check("waiter: WAIT while Wi-Fi", r[:4] == b"X6BW" and r[4] == 0 and r[5] == 1, "sysstatus=%d" % r[5])
time.sleep(1.7)
r = ask(sim)
sum_ = sum(struct.unpack(">256I", r[512:1536])) & 0xffffffff
check("waiter: READY with the real IPL", r[4] == 1 and r[5] == 5 and r[512:1536] == img[1024:2048] and struct.unpack(">I", r[20:24])[0] == sum_)
# 4. 設定画面
st, _, d = sim.read(6, 0)
check("ID6 LBA0 signature", d[:8] == b"X68SCSI1" and d[16:32] == b"X68000ZRemoteDrv")
st, _, d = sim.read(6, 0x100)
check("settingui at ID6 0x100", d[:2] == b"HU")
hdr = b"X68Z" + struct.pack(">II", 0x1234, 7) + bytes([0, 0, 0, 0])
sim.write(6, 0x20, (hdr + struct.pack(">H", 0xff03)).ljust(512, b"\0"))
st, _, d = sim.read(6, 0x3ffff8)
check("vdbuf GETSTATUS", d[:12] == hdr[:12] and d[16] == 5)
sim.write(6, 0x20, (hdr[:8] + struct.pack(">I", 8) + bytes(4) + struct.pack(">H", 0xff01)).ljust(512, b"\0"))
st, _, d = sim.read(6, 0x3fffe8, 4)      # 1700 バイト = 4 ページ (lba % 8 がページ)
cfg = b"".join(d[i * 512 + 16:(i + 1) * 512] for i in range(4))
check("vdbuf GETCONFIG (4 pages, ssid and hds0)", d[13] == 3 and cfg[:7] == b"picosim" and cfg[1160:1160 + len(HDS)] == HDS.encode())
sim.close()

# 3. HDS が変わった
img[20 * 512 + 100] ^= 0xff
open(HDS, "wb").write(img)
sim = Sim("0.1,0.2,0.8")
st, _, d = sim.read(0, 0, 32)
check("old snapshot served", st == 0)
time.sleep(1.0)
r = ask(sim)
check("waiter: CHANGED", r[4] == 2, "state=%d" % r[4])
sim.close()
snap = open(SNAP, "rb").read()
check("snapshot rewritten with the new HDS", snap[4096:4096 + 16384] == bytes(img[:16384]))
sim = Sim("0,0,0.3")
sim.read(0, 0, 32)
time.sleep(0.5)
r = ask(sim)
check("after power cycle: READY again", r[4] == 1)
sim.close()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
