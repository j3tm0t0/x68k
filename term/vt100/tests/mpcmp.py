#!/usr/bin/env python3
"""mptest の VRAM を確かめる。usage: mpcmp.py [-F] MP.vram FULL0.vram X,Y,W,H [FULL1.vram X,Y,W,H ...]
-F は小さい字 (セル 6x12。枠の左端と幅は 4 桁の倍数で、バイトの境目にそろう)。
各枠の中 = そのフィクスチャを全面 WxH で流した VRAM の (0,0) から、枠の外 (表示の 512 ライン) = 0。"""
import sys
args = sys.argv[1:]
small = args[:1] == ["-F"]
if small: args = args[1:]
CW, CHL = (6, 12) if small else (8, 16)
sys.argv = [sys.argv[0]] + args
mp = open(sys.argv[1], "rb").read()
rects = []
for i in range(2, len(sys.argv), 2):
    x, y, w, h = map(int, sys.argv[i + 1].split(","))
    rects.append((open(sys.argv[i], "rb").read(), x, y, w, h))
LB, NL = 128, 512
bad = 0
for p in range(4):
    for yl in range(NL):
        row = yl // CHL
        for c in range(LB):
            v = mp[(p * NL + yl) * LB + c]
            e = 0
            for full, x, y, w, h in rects:
                bx, bw = x * CW // 8, (w * CW + 7) // 8
                if y <= row < y + h and bx <= c < bx + bw:
                    e = full[(p * NL + (yl - y * CHL)) * LB + (c - bx)]
                    break
            if v != e:
                bad += 1
                if bad <= 5: print("違う: プレーン %d ライン %d 桁 %d: %02x (期待 %02x)" % (p, yl, c, v, e))
print("違い %d バイト" % bad)
sys.exit(1 if bad else 0)
