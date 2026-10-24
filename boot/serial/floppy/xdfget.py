#!/usr/bin/env python3
"""xdfget.py XDF PATH OUT -- Human68k の 2HD フロッピーイメージからファイルを 1 つ取り出す (例: /SYS/RSDRV.SYS)"""
import struct, sys
xdf, path, out = sys.argv[1:4]
d = open(xdf, "rb").read()
sec, spc, res, nfat, nroot, total, media, fatsec = 1024, 1, 1, 2, 192, 1232, 0xFE, 2   # X68000 の 2HD
fat = d[res * sec:(res + fatsec) * sec]
root = (res + nfat * fatsec) * sec
data0 = root + nroot * 32
def nxt(c):
    o = c * 3 // 2
    v = fat[o] | fat[o + 1] << 8
    return (v >> 4) if c & 1 else (v & 0xFFF)
def chain(c):
    while 2 <= c < 0xFF8:
        yield c; c = nxt(c)
def read(c, size=None):
    b = b"".join(d[data0 + (x - 2) * sec:data0 + (x - 1) * sec] for x in chain(c))
    return b if size is None else b[:size]
ents = d[root:data0]
for part in [p for p in path.upper().split("/") if p]:
    base, _, ext = part.partition(".")
    for i in range(0, len(ents), 32):
        e = ents[i:i + 32]
        if e[0] == 0: break
        nm = (e[0:8].rstrip(b" ") + e[12:22].rstrip(b"\0")).decode("cp932").upper()
        ex = e[8:11].rstrip(b" ").decode("cp932").upper()
        if nm == base and ex == ext:
            c, size = struct.unpack("<H", e[26:28])[0], struct.unpack("<I", e[28:32])[0]
            if e[11] & 0x10: ents = read(c)
            else: open(out, "wb").write(read(c, size)); sys.exit(0)
            break
    else:
        sys.exit("xdfget: %s not found" % path)
sys.exit("xdfget: %s is a directory" % path)
