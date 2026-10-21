#!/usr/bin/env python3
"""rb-cli が書く Human68k ディレクトリエントリの拡張名フィールド (オフセット 12-21) は
空白 (0x20) で埋められているが、Human68k は 0x00 埋めを期待する (空白だと
"HUMAN   SYS" を見つけられず起動できない)。全ディレクトリを辿って 0x00 に直す。
usage: fixfs.py IMAGE.hds
"""
import struct, sys

img = sys.argv[1]
f = open(img, "r+b")
data = bytearray(f.read())

# X68K パーティションテーブル (SCSI: オフセット 0x800, 1024 バイトセクタ単位)
if data[0x800:0x804] != b"X68K":
    sys.exit("fixfs: no X68K partition table at 0x800")
fixed = 0
for i in range(15):
    e = 0x810 + 16 * i
    name = data[e:e + 8]
    if name.strip(b"\0 ") == b"":
        continue
    start = struct.unpack(">I", data[e + 8:e + 12])[0] & 0x00FFFFFF
    P = start * 1024
    bps = struct.unpack(">H", data[P + 0x12:P + 0x14])[0]
    spc = data[P + 0x14]
    nfats = data[P + 0x15]
    reserved = struct.unpack(">H", data[P + 0x16:P + 0x18])[0]
    rootents = struct.unpack(">H", data[P + 0x18:P + 0x1A])[0]
    total = struct.unpack(">I", data[e + 12:e + 16])[0]   # パーティションのセクタ数 (テーブルから)
    spf = data[P + 0x1D]
    fat_off = P + reserved * bps
    root_off = fat_off + nfats * spf * bps
    data_off = root_off + rootents * 32
    clusters = (total * bps - (data_off - P)) // (spc * bps)
    fat16 = clusters >= 0xFF0

    def fat(n):
        if fat16:
            return struct.unpack("<H", data[fat_off + 2 * n:fat_off + 2 * n + 2])[0]
        o = fat_off + n + n // 2
        v = struct.unpack("<H", data[o:o + 2])[0]
        return (v >> 4) if n & 1 else (v & 0xFFF)

    def chain(c):
        out = []
        while 2 <= c < (0xFFF0 if fat16 else 0xFF0) and c not in out:
            out.append(c)
            c = fat(c)
        return out

    def fix_entries(off, length):
        global fixed
        subdirs = []
        for o in range(off, off + length, 32):
            ent = data[o:o + 32]
            if ent[0] == 0:
                break
            if ent[0] == 0xE5:
                continue
            attr = ent[11]
            if ent[12:22] == b" " * 10:
                data[o + 12:o + 22] = b"\0" * 10
                fixed += 1
            if attr & 0x10 and ent[:8].rstrip() not in (b".", b".."):
                subdirs.append(struct.unpack("<H", ent[26:28])[0])
        return subdirs

    todo = fix_entries(root_off, rootents * 32)
    seen = set()
    while todo:
        c = todo.pop()
        if c in seen:
            continue
        seen.add(c)
        for cl in chain(c):
            todo += fix_entries(data_off + (cl - 2) * spc * bps, spc * bps)
    print("fixfs: partition %d (%s) FAT%d, %d clusters" % (i + 1, name.decode("ascii", "replace").strip(), 16 if fat16 else 12, clusters))

f.seek(0); f.write(data); f.close()
print("fixfs: fixed %d entries" % fixed)
