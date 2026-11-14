#!/usr/bin/env bash
# scripts/mkhds.py の試験 (x68k-zos #175: SCSI のデバイスドライバを途中で切っていた)。
# FORMAT.X は第三者の配布物なので CI には置かない。代わりに目印だけを並べたにせの FORMAT.X で
#   1) ドライバを「0 が 3 バイト + C library for X68000」の手前まで取り出すこと
#   2) その終わりが見つからないときはエラーで止まること
# を確かめる。本物の FORMAT.X (stage/BIN/FORMAT.X か $FORMAT_X) があれば、
#   3) 取り出したドライバが 13141 バイトで、FORMAT.X の中の SCHDISK から 13141 バイトと一致すること
# も確かめる。
set -euo pipefail
cd "$(dirname "$0")/../.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I - "$T" "${FORMAT_X:-stage/BIN/FORMAT.X}" <<'PY'
import os, subprocess, sys
tmp, real = sys.argv[1], sys.argv[2]
DRV_MARK = b"\xff\xff\xff\xff\x40\x00\x00\x00\x00\x6e\x00\x00\x00\x80\x01SCHDISK"
CLIB = b"C library for X68000 XC"
fails = 0
def check(name, ok):
    global fails
    print("%s %s" % ("PASS" if ok else "FAIL", name))
    fails += not ok

def mkhds(fmt, img):
    return subprocess.run([sys.executable, "-I", "scripts/mkhds.py", fmt, img, "4"],
                          capture_output=True, text=True)

def fake(driver_len, terminator=True):
    disk_ipl = b"\x60\x00" + b"\x11" * 0x100 + b"X68000 SCSI DISK IPL MENU" + b"\x22" * 0x2c0
    part_ipl = b"\x60\x24SHARP/KG    1.00" + b"\x33" * 0x300
    body = DRV_MARK + bytes((i * 7 + 1) % 255 + 1 for i in range(driver_len - len(DRV_MARK)))
    tail = (b"\0\0\0" if terminator else b"\x44\x44\x44") + CLIB + b"\0" * 64
    return b"\0" * 0x40 + disk_ipl + part_ipl + body + tail, body

# 1) にせの FORMAT.X: 13141 バイトのドライバ (12771 で切らないこと)
f, body = fake(13141)
p = os.path.join(tmp, "FORMAT.X"); open(p, "wb").write(f)
r = mkhds(p, os.path.join(tmp, "a.hds"))
check("fake FORMAT.X: mkhds.py exits 0", r.returncode == 0)
if r.returncode == 0:
    img = open(os.path.join(tmp, "a.hds"), "rb").read()
    check("fake FORMAT.X: driver copied whole (13141 bytes)", img[0xC00:0xC00 + len(body)] == body)
    check("fake FORMAT.X: zero after driver up to 0x4000", not any(img[0xC00 + len(body):0x4000]))
else:
    print(r.stderr)

# 2) 終わりの目印が無いときは止まる
f, _ = fake(13141, terminator=False)
p2 = os.path.join(tmp, "FORMAT2.X"); open(p2, "wb").write(f)
r = mkhds(p2, os.path.join(tmp, "b.hds"))
check("no driver terminator: mkhds.py fails", r.returncode != 0 and "end of SCSI driver" in r.stderr)

# 3) 本物の FORMAT.X (v2.31)
if os.path.exists(real):
    fmt = open(real, "rb").read()
    r = mkhds(real, os.path.join(tmp, "c.hds"))
    check("FORMAT.X: mkhds.py exits 0", r.returncode == 0)
    img = open(os.path.join(tmp, "c.hds"), "rb").read()
    d = fmt.find(DRV_MARK)
    check("FORMAT.X: driver is 13141 bytes as written by FORMAT.X device init",
          img[0xC00:0xC00 + 13141] == fmt[d:d + 13141] and not any(img[0xC00 + 13141:0x4000]))
else:
    print("SKIP real FORMAT.X (%s not found)" % real)
sys.exit(1 if fails else 0)
PY
