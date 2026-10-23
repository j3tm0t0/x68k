#!/usr/bin/env python3
"""mptest の VRAM を確かめる。usage: mpcmp.py MP.vram FULL0.vram X,Y,W,H [FULL1.vram X,Y,W,H ...]
各枠の中 = そのフィクスチャを全面 WxH で流した VRAM の (0,0) から、枠の外 (表示の 512 ライン) = 0。"""
import sys
mp = open(sys.argv[1], "rb").read()
rects = []
for i in range(2, len(sys.argv), 2):
    x, y, w, h = map(int, sys.argv[i + 1].split(","))
    rects.append((open(sys.argv[i], "rb").read(), x, y, w, h))
LB, NL = 128, 512
bad = 0
for p in range(4):
    for yl in range(NL):
        row = yl // 16
        for c in range(LB):
            v = mp[(p * NL + yl) * LB + c]
            e = 0
            for full, x, y, w, h in rects:
                if y <= row < y + h and x <= c < x + w:
                    e = full[(p * NL + (yl - y * 16)) * LB + (c - x)]
                    break
            if v != e:
                bad += 1
                if bad <= 5: print("違う: プレーン %d ライン %d 桁 %d: %02x (期待 %02x)" % (p, yl, c, v, e))
print("違い %d バイト" % bad)
sys.exit(1 if bad else 0)
