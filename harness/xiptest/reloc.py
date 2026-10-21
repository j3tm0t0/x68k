"""X 形式の再配置表: 語の並び。各語は前の位置からの差 (偶数)。1 なら続く長語が差。その位置の長語に先頭番地を足す"""
import struct


def parse(tab):
    offs, o, i = [], 0, 0
    while i < len(tab):
        w = struct.unpack('>H', tab[i:i + 2])[0]
        i += 2
        if w == 1:
            w = struct.unpack('>I', tab[i:i + 4])[0]
            i += 4
        o += w
        offs.append(o)
    return offs


def build(offs):
    out, prev = b'', 0
    for o in offs:
        d = o - prev
        assert d > 0 and d % 2 == 0, (hex(o), d)
        out += struct.pack('>H', d) if d < 0x10000 else struct.pack('>HI', 1, d)
        prev = o
    return out
