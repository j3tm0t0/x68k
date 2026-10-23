#!/usr/bin/env python3
"""ペイン (Z-OS) の試験: 全面で描いた VRAM と、枠 (x 桁, y 行, w, h) の中に描いた VRAM を比べる。
usage: panecmp.py FULL.vram PANE.vram X Y W H
FULL は -s WxH で原点 (0,0) に描いたもの、PANE は同じ入力を -P X,Y,W,H で描いたもの。
合格: PANE の枠の中 = FULL の (0,0) からの WxH、枠の外 (4 プレーン、表示の 512 ライン) は全部 0 (何も描いていない)。
vthost の VRAM ダンプは 4 プレーン x 512 ライン x 128 バイト + パレット 32 バイト。"""
import sys
full, pane = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
X, Y, W, H = map(int, sys.argv[3:7])
LB, NL = 128, 512
bad = 0
for p in range(4):
    for y in range(NL):
        row = y // 16
        for c in range(LB):
            v = pane[(p * NL + y) * LB + c]
            if Y <= row < Y + H and X <= c < X + W:
                e = full[(p * NL + (y - Y * 16)) * LB + (c - X)]
                if v != e:
                    bad += 1
                    if bad <= 5: print("枠の中が違う: プレーン %d ライン %d 桁 %d: %02x (全面では %02x)" % (p, y, c, v, e))
            elif v:
                bad += 1
                if bad <= 5: print("枠の外に描いた: プレーン %d ライン %d 桁 %d: %02x" % (p, y, c, v))
print("違い %d バイト" % bad)
sys.exit(1 if bad else 0)
