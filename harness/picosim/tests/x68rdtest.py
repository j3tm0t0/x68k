#!/usr/bin/env python3
"""picosim -X (x68rd.c、BlueSCSI のリモートドライブのまね) の試験 (XEiJ なし)。bsrmt.sys と同じ手順 (要求を ID 6 の
LBA 0x200000 に 1 回の WRITE(10) で書き、応答を 0x210000 から読む) を、picosim の Z キャッシュ (既定の 64MB) 越しに行う。
  1. INIT が版・件数・大きさ・ユニット数を返す
  2. OPEN / READ (15872 バイトずつ) で中身が合う、Z キャッシュがあっても毎回新しい応答が読める
  3. FILES が 1 回で 10 件 (remoteserv.c の上限) ずつ返し、NFILES で残りが全部そろう
  4. CREATE / WRITE (15872 バイト) / CLOSE で Mac のファイルが合う
  5. 同じ seq の送り直し (flags bit0) は実行せず前の応答を返す、見出しが違えば status 2
usage: x68rdtest.py [picosim]"""
import os, socket, struct, subprocess, sys, time, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
PORT = 16872
WLBA, RLBA, DATA_OFF, DATASIZE = 0x200000, 0x210000, 512, 15872
fails = 0
tmp = tempfile.mkdtemp()
data = os.urandom(100000)
open(os.path.join(tmp, "TEST.BIN"), "wb").write(data)
os.mkdir(os.path.join(tmp, "MANY"))
for i in range(25):
    open(os.path.join(tmp, "MANY", "F%02d.TXT" % i), "w").write("x")

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond: fails += 1

def namebuf(path, name, ext):
    p = path.replace("\\", "\t").encode().ljust(65, b"\0")
    return bytes([0, 0]) + p + name.encode().ljust(8) + ext.encode().ljust(3) + bytes(10)

class Z:
    def __init__(self):
        self.p = subprocess.Popen([PICOSIM, "-l", "127.0.0.1:%d" % PORT, "-L", "none", "-X", tmp, "-v"],
                                  stderr=open(os.path.join(HERE, "x68rd.log"), "a"))
        for _ in range(50):
            try: self.s = socket.create_connection(("127.0.0.1", PORT)); break
            except OSError: time.sleep(0.1)
        self.q = 0; self.seq = 0
    def rd(self, n):
        b = b""
        while len(b) < n: b += self.s.recv(n - len(b))
        return b
    def scsi(self, cdb, data=b""):
        self.q += 1
        self.s.sendall(b"PSQ1" + struct.pack(">IBBBBI", self.q, 6, 0, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
        h = self.rd(16); return self.rd(struct.unpack(">I", h[12:16])[0])
    def xchg(self, cmd, wdata=b"", anbytes=512, retry=False, magic=b"X6RD"):
        if not retry: self.seq += 1
        hdr = magic + struct.pack(">IHHHH", self.seq, len(cmd), 1 if retry else 0, 0, 0)
        blk = (hdr + cmd).ljust(DATA_OFF, b"\0") + wdata
        n = (len(blk) + 511) // 512
        self.scsi(struct.pack(">BBIBHB", 0x2a, 0, WLBA, 0, n, 0), blk.ljust(n * 512, b"\0"))
        r = self.scsi(struct.pack(">BBIBHB", 0x28, 0, RLBA, 0, (anbytes + 511) // 512, 0))
        return r
    def close(self): self.s.close(); self.p.terminate(); self.p.wait()

z = Z()
a = z.xchg(bytes([0x00]))
check("INIT answer", a[:4] == b"X6RA" and struct.unpack(">H", a[10:12])[0] == 0 and a[16] == 0 and a[17] == 1,
      a[:24].hex())
check("INIT sizes: 10 files, 15872 bytes, 1 unit", a[18] == 10 and struct.unpack(">H", a[20:22])[0] == DATASIZE and a[22] == 1)

# OPEN + READ
a = z.xchg(bytes([0x0a, 0]) + struct.pack(">I", 0x1000) + namebuf("\\", "TEST", "BIN"))
size = struct.unpack(">I", a[17:21])[0]
check("OPEN", a[16] == 0 and size == len(data), "res %d size %d" % (a[16], size))
got = b""; pos = 0
while pos < len(data):
    a = z.xchg(bytes([0x0c]) + struct.pack(">IIH", 0x1000, pos, DATASIZE), anbytes=DATA_OFF + DATASIZE)
    n = struct.unpack(">h", a[16:18])[0]
    if n <= 0 or struct.unpack(">I", a[4:8])[0] != z.seq: break
    got += a[DATA_OFF:DATA_OFF + n]; pos += n
check("READ 15872 at a time, through the Z cache", got == data, "%d of %d bytes" % (len(got), len(data)))
a = z.xchg(bytes([0x0b]) + struct.pack(">I", 0x1000))
check("CLOSE", a[16] == 0)

# FILES / NFILES
names = []
a = z.xchg(bytes([0x07, 0x20, 32]) + struct.pack(">I", 0x2000) + namebuf("\\MANY\\", "????????", "???"), anbytes=1536)
check("FILES gives at most 10", a[16] == 0 and a[17] == 10, "res %d num %d" % (a[16], a[17]))
while a[16] == 0 and a[17] > 0:
    for i in range(a[17]):
        e = a[18 + 32 * i:18 + 32 * (i + 1)]
        names.append(e[9:].split(b"\0")[0].decode())
    a = z.xchg(bytes([0x08, 32]) + struct.pack(">I", 0x2000), anbytes=1536)
files = sorted(n for n in names if n.startswith("F"))
check("NFILES brings the rest (25 files)", files == ["F%02d.TXT" % i for i in range(25)], " ".join(files))

# CREATE + WRITE + CLOSE
w = os.urandom(DATASIZE + 1000)
a = z.xchg(bytes([0x09, 0x20, 1]) + struct.pack(">I", 0x3000) + namebuf("\\", "NEW", "BIN"))
check("CREATE", a[16] == 0, "res %d" % a[16])
a = z.xchg(bytes([0x0d]) + struct.pack(">IIH", 0x3000, 0, DATASIZE), w[:DATASIZE])
check("WRITE 15872", struct.unpack(">h", a[16:18])[0] == DATASIZE)
a = z.xchg(bytes([0x0d]) + struct.pack(">IIH", 0x3000, DATASIZE, 1000), w[DATASIZE:])
seq = z.seq
# the X68000 did not get the answer: the same request again is not carried out twice
a = z.xchg(bytes([0x0d]) + struct.pack(">IIH", 0x3000, DATASIZE, 1000), w[DATASIZE:], retry=True)
check("resent WRITE answered from the last answer", struct.unpack(">I", a[4:8])[0] == seq and
      struct.unpack(">h", a[16:18])[0] == 1000)
a = z.xchg(bytes([0x0b]) + struct.pack(">I", 0x3000))
check("CLOSE after WRITE", a[16] == 0)
check("the Mac file matches", open(os.path.join(tmp, "NEW.BIN"), "rb").read() == w)

a = z.xchg(bytes([0x00]), magic=b"XXXX")
check("bad header: status 2", a[:4] == b"X6RA" and struct.unpack(">H", a[10:12])[0] == 2)
z.close()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
