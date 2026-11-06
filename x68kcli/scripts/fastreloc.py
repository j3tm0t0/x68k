#!/usr/bin/env python3
"""Human68k の .x の再配置の表を、プログラム自身が起動時に当てる形に直す (src/xshim/x68reloc.S と組で使う)。
usage: fastreloc.py FILE.x ...

Human68k の _EXEC は再配置の表を 1 項目に約 33 マイクロ秒かけて当てる (XEiJ で測った。vim 9 の 5 万項目で 1.7 秒)。
入口が x68reloc.S の __x68_fastreloc_start (目印 "X68RELO1") の .x について:
  - 再配置の表を x68reloc.S の形にしてデータの後ろに足し、その分 bss を減らす (表は bss の頭と重なり、crt0 が後で 0 にする)
  - x68reloc.S の __x68_reloc_table に表の位置 (テキストの頭から) を書く
  - ヘッダの再配置の大きさを 0 にする (Human68k は何もしない)
目印が無いもの、再配置の無いもの、もう直したものはそのまま。
"""
import struct
import sys

MARK = b"X68RELO1"


def fix(path):
    d = bytearray(open(path, "rb").read())
    if d[:2] != b"HU":
        return "not .x"
    entry, text, data, bss, rel, sym = struct.unpack(">IIIIII", d[8:32])
    if entry + 14 > text:
        return "entry is not in text"
    if (text + data) & 1:
        return "text+data is odd"
    e = 64 + entry
    if d[e:e + 2] != b"\x60\x10" or d[e + 2:e + 10] != MARK:
        return "no x68reloc entry"
    if struct.unpack(">I", d[e + 10:e + 14])[0] != 0:
        return "already"
    if rel == 0:
        return "no relocation"
    body_end = 64 + text + data
    # Human68k の表 (16 ビットの差、1 なら続く 32 ビット) を読み、x68reloc.S の形 (符号付き 16 ビット、0 の後に 32 ビット) にする
    src = bytes(d[body_end:body_end + rel])
    table = bytearray()
    i = 0
    while i < len(src):
        delta = struct.unpack(">H", src[i:i + 2])[0]; i += 2
        if delta == 1:
            delta = struct.unpack(">I", src[i:i + 4])[0]; i += 4
        if delta & 1 or delta == 0:   # 奇数の番地と、0 (位置 0 の項目。終わりと区別できない) は扱わない
            return "odd or zero delta (not supported)"
        if 0 < delta < 0x8000:
            table += struct.pack(">H", delta)
        else:
            table += struct.pack(">HI", 0, delta)
    table += struct.pack(">HI", 0, 0)
    table = bytes(table)
    rest = bytes(d[body_end + rel:])   # 記号の表と、その後ろに何かあればそれも残す
    struct.pack_into(">I", d, e + 10, text + data)       # 表の位置 (テキストの頭から)
    newdata = data + len(table)
    newbss = max(0, bss - len(table))
    struct.pack_into(">IIII", d, 16, newdata, newbss, 0, sym)
    out = bytes(d[:body_end]) + table + rest
    open(path, "wb").write(out)
    return "ok (%d bytes of relocation)" % rel


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print("fastreloc: %s: %s" % (p, fix(p)))
