#!/usr/bin/env python3
"""テキスト VRAM ダンプ (vtcat/vthost の -v) を PNG にする。
usage: vram2png.py VRAMFILE OUT.png [SCALE]
入力: 4 プレーン x 512 ライン x 128 バイト (プレーン p のライン y = p*65536 + y*128) +
      パレット 16 ワード (ビッグエンディアン、GRB = (g<<11)|(r<<6)|(b<<1)|i)。
出力: 768x512 (SCALE 倍)。各ピクセルの 4 プレーンのビットがパレット番号。
Pillow があれば使い、無ければ zlib で PNG を直接書く。"""
import sys, struct, zlib

W, H, LB = 768, 512, 128


def load(path):
    d = open(path, "rb").read()
    planes = [d[p * H * LB:(p + 1) * H * LB] for p in range(4)]
    pal = []
    for i in range(16):
        w = struct.unpack(">H", d[4 * H * LB + i * 2:4 * H * LB + i * 2 + 2])[0]
        g, r, b = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
        pal.append((r * 255 // 31, g * 255 // 31, b * 255 // 31))
    return planes, pal


def indices(planes):
    """[y][x] -> パレット番号 (0..15)"""
    out = []
    for y in range(H):
        row = bytearray(W)
        for c in range(W // 8):
            o = y * LB + c
            b0, b1, b2, b3 = planes[0][o], planes[1][o], planes[2][o], planes[3][o]
            for k in range(8):
                m = 0x80 >> k
                row[c * 8 + k] = ((b0 & m) and 1) | ((b1 & m) and 2) | ((b2 & m) and 4) | ((b3 & m) and 8)
        out.append(row)
    return out


def write_png(path, rgbrows, w, h):
    raw = b"".join(b"\x00" + bytes(r) for r in rgbrows)
    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    planes, pal = load(src)
    idx = indices(planes)
    rows = []
    for y in range(H):
        line = bytearray()
        for x in range(W):
            line += bytes(pal[idx[y][x]]) * scale
        for _ in range(scale):
            rows.append(line)
    write_png(dst, rows, W * scale, H * scale)


if __name__ == "__main__":
    main()
