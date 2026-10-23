#!/usr/bin/env python3
"""Pico Z Loader (x68k-zos の branch pzl) の SCSI の層をホストで (build/picosim-pzl --pzl=config.txt)。
PZL の virtual_disk.c / storage.c / config_file.c はそのまま、Wi-Fi・SMB は picosim の代わり (SMB の共有 = 手元のディレクトリ)。
  1. 接続が済むまで ID 0 は NOT READY、INQUIRY は答える
  2. 開いたら: LBA 0 はファイルのまま、LBA 2 は起動 IPL (hdsboot)、LBA 0x20/0x21 はファイルの 2/3、READ CAPACITY はファイルの大きさ
  3. 書き込みがファイルに届く (WRITE(10) 2 セクタ)
  4. ID 6 はリモートの顔 (X68SCSI1 / X68000ZRemoteDrv)
  5. HDS1 に無いファイルを書くと、開けずに MEDIUM ERROR (03/11)
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
img = bytearray(os.urandom(512 * 128)); img[0:8] = b"X68SCSI1"
hds = os.path.join(root, "share", "x68000z", "test.hds")
open(hds, "wb").write(img)
cfg = os.path.join(root, "config.txt")
open(cfg, "w").write("WIFI_SSID: picosim\nSMB2_SERVER: 192.168.1.1\nHDS0: share/x68000z/test.hds\nHDS1: share/x68000z/none.hds\n"
                     "REMOTE_UNIT: 0\nTZ: JST-9\n")
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
s.close(); p.terminate(); p.wait()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
