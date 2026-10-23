#!/usr/bin/env python3
"""B: (リモートドライブ) の試験 (XEiJ なし)。Z の scsiremote.sys の com_cmdres と同じ手順 (要求を ID 6 の LBA 0x20 に書き、
応答を 0x200000〜 の帯から 8 ページずつ下へ読む) を、picosim の Z キャッシュ (先読み 16KB) 越しに行う。
  1. OPEN / READ が正しい中身を返す
  2. _EXEC と同じ並び: 24 ページ (8 の倍数) の応答の次に 31 ページの応答 -> 古い (ファームと同じ) 数え方では
     ページ 8 以降が 8 ずれる (2026-10-23 に Z で ssh.x の一部の版が B: から起動すると落ちた原因)。-P では正しい
  3. rpage パッチの新しいドライバ (見出しの reserved に読み始めの sect、bit 7。ページ番号も照合) は正しい。
     pwauth の rdchk と同じ大きさ (DOS _READ 26972 / 23294 バイト = 15360 + 11612 / 7934 の後に 15360) も
  4. 古いドライバ (bit 7 無し) は今までどおり数える (同じく壊れる = 互換)
usage: remotetest.py [picosim] [driver/]"""
import os, socket, struct, subprocess, sys, time, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PICOSIM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "picosim")
FW = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/src/github.com/yunkya2/x68kzremotedrv-hid/driver")
PORT = 16870
fails = 0
tmp = tempfile.mkdtemp()
data = os.urandom(200000)
open(os.path.join(tmp, "TEST.BIN"), "wb").write(data)

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (("  " + extra) if extra else ""))
    if not cond: fails += 1

class Z:
    def __init__(self, extra):
        self.p = subprocess.Popen([PICOSIM, "-l", "127.0.0.1:%d" % PORT, "-L", "none", "-F", FW, "-R", tmp, "-vv"] + extra,
                                  stderr=open(os.path.join(HERE, "remote.log"), "a"))
        for _ in range(50):
            try: self.s = socket.create_connection(("127.0.0.1", PORT)); break
            except OSError: time.sleep(0.1)
        self.q = 0; self.seqno = 0; self.sect = 0x400000; self.session = 0x1234abcd
        self.new = False; self.retries = 0
    def rd(self, n):
        b = b""
        while len(b) < n: b += self.s.recv(n - len(b))
        return b
    def cmd(self, cdb, data=b""):
        self.q += 1
        self.s.sendall(b"PSQ1" + struct.pack(">IBBBBI", self.q, 6, 0, len(cdb), 0, len(data)) + cdb.ljust(16, b"\0") + data)
        h = self.rd(16); return self.rd(struct.unpack(">I", h[12:16])[0])
    def read(self, lba): return self.cmd(struct.pack(">BBIBHB", 0x28, 0, lba, 0, 1, 0))
    def write(self, lba, d): self.cmd(struct.pack(">BBIBHB", 0x2a, 0, lba, 0, 1, 0), d)
    def cmdres(self, w, rsize):
        """scsiremote.c の com_cmdres をそのまま (self.new: rpage パッチの版)"""
        wcnt = (len(w) - 1) // 496; rcnt = (rsize - 1) // 496
        rsect = ((self.sect - 8) % 0x200000) + 0x200000
        res = bytes([0x80 | ((rsect >> 11) & 0x1f), (rsect >> 3) & 0xff]) if self.new else bytes(2)
        for i in range(wcnt + 1):
            hdr = b"X68Z" + struct.pack(">II", self.session, self.seqno) + bytes([i, wcnt]) + res
            self.write(0x20, (hdr + w[i * 496:(i + 1) * 496]).ljust(512, b"\0"))
        self.sect = rsect
        out = b""; i = 0
        while i <= rcnt:
            while True:
                b = self.read(self.sect + (i & 7))
                if b[:12] == hdr[:12] and (not self.new or b[12] == i): break
                self.retries += 1
                self.sect = ((self.sect - 0x10000) % 0x200000) + 0x200000
            out += b[16:]
            rcnt = b[13]
            if (i & 7) == 7: self.sect = ((self.sect - 8) % 0x200000) + 0x200000
            i += 1
        self.seqno += 1
        return out[:rsize]
    def close(self): self.s.close(); self.p.terminate(); self.p.wait()

def run(extra, new=False, sizes=(15360, 11612, 15360), reboot=False):
    """READ を続けて行い、それぞれが正しいかを返す。reboot: 途中で Z をソフトリセットした形 (ドライバは sect と session を
    最初から、Z のキャッシュは残る -> 古い応答に当たって 0x10000 下で読み直す道を通る)"""
    z = Z(extra); z.new = new
    z.cmdres(bytes([0x00]), 1)                                        # INIT
    nb = bytes([0, 0]) + b"\t".ljust(65, b"\0") + b"TEST    " + b"BIN" + bytes(10)   # dos_namebuf: flag drive path name1 ext name2
    z.cmdres(bytes([0x0a, 0, 0, 0]) + struct.pack(">I", 0x1000) + nb, 8)
    res = []; pos = 0
    for k, n in enumerate(sizes):
        if reboot and k == len(sizes) // 2:
            z.sect = 0x400000; z.session ^= 0x5a5a; z.seqno = 0
            z.cmdres(bytes([0x00]), 1)
            z.cmdres(bytes([0x0a, 0, 0, 0]) + struct.pack(">I", 0x1000) + nb, 8)
        r = z.cmdres(bytes([0x4c, 0, 0, 0]) + struct.pack(">IIH", 0x1000, pos, n), 2 + n)
        got = r[2:2 + struct.unpack(">h", r[:2])[0]]
        res.append(got == data[pos:pos + n]); pos += n
    z.close()
    return res, z.retries

ok, _ = run([])
check("old driver: plain 15KB READ", ok[0])
check("old driver: 24-page reply", ok[1])
check("old driver: READ after a 24-page reply is corrupted (the B: _EXEC bug, legacy counting kept)", not ok[2])
ok, _ = run([], sizes=(15360, 7934, 15360))
check("old driver: READ after a 16-page reply is corrupted too", not ok[2])
ok, _ = run(["-P"])
check("old driver + -P: all three READs correct", all(ok))
for a in (26972, 23294, 26000, 30000):
    sizes = (15360, a - 15360, 15360)            # DOS _READ a バイト (15360 ずつ直に読む) の後に 15360
    ok, retries = run([], new=True, sizes=sizes)
    check("rpage driver: _READ %d then 15360" % a, all(ok), "retries=%d" % retries)
ok, retries = run([], new=True, sizes=(15360,) * 3 + (11612, 15360, 3500, 15360, 7934, 15360, 100, 15360))
check("rpage driver: mixed sizes", all(ok), "retries=%d" % retries)
ok, retries = run([], new=True, sizes=(15360, 11612, 15360, 15360, 11612, 15360), reboot=True)
check("rpage driver: soft reset with the Z cache kept (stale hits, 0x10000 jumps)", all(ok) and retries > 0, "retries=%d" % retries)
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
