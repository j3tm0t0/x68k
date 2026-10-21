"""xip.x (X 形式) から、番地 0 に置く再配置前のテキスト + データ + BSS (0) を作る"""
import struct

HDR = 0x40


def load(path):
    b = open(path, 'rb').read()
    text, data, bss, rel = struct.unpack('>IIII', b[12:28])
    img = b[HDR:HDR + text + data] + bytes(bss)
    return img, b, (text, data, bss, rel)
