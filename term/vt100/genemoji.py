#!/usr/bin/env python3
"""絵文字 PNG を 16x16・16色 (ANSI テキストパレット) に変換して C ヘッダ emojitab.h を作る。
X68000 のテキスト画面は 4 プレーン = 1 ピクセル 4bit で、テキストパレット (16 色) を引く。
その 16 色は vt100.c の ANSI 8+8 色なので、絵文字もその 16 色に量子化する (色は粗くなる割り切り)。
透明ピクセルはパレット 0 (黒) にする (黒背景の端末を前提)。

ソースは Twemoji 15.1.0 (CC-BY 4.0, (c) 2023 Twitter/X, jdecked/twemoji)。変換後の表だけを収録する。
usage: genemoji.py SRCDIR [LIST] > emojitab.h   (SRCDIR に <hexコードポイント>.png)
LIST (emojilist.txt) を渡すとその行頭の hex コードポイントだけを収録する (無い PNG はエラー)。
"""
import sys, os, glob
from PIL import Image

# vt100.c と同じ ANSI 16 色 (X68000 値 g,r,b: 0..31) -> RGB 0..255
PAL31 = [
    (0, 0, 0), (0, 21, 0), (21, 0, 0), (21, 21, 0), (0, 0, 21), (0, 21, 21), (21, 0, 21), (21, 21, 21),
    (10, 10, 10), (0, 31, 0), (31, 0, 0), (31, 31, 0), (0, 0, 31), (0, 31, 31), (31, 0, 31), (31, 31, 31),
]
# 注: vt100.c の rgb() は (g<<11)|(r<<6)|(b<<1)、tbl は [r,g,b]。ここでは index の色を RGB で持つ。
# vt100.c の tbl (r,g,b 0..31):
TBL = [
    (0, 0, 0), (21, 0, 0), (0, 21, 0), (21, 21, 0), (0, 0, 21), (21, 0, 21), (0, 21, 21), (21, 21, 21),
    (10, 10, 10), (31, 0, 0), (0, 31, 0), (31, 31, 0), (0, 0, 31), (31, 0, 31), (0, 31, 31), (31, 31, 31),
]
PAL = [(r * 255 // 31, g * 255 // 31, b * 255 // 31) for (r, g, b) in TBL]


def nearest(r, g, b):
    best, bi = 1 << 30, 0
    for i, (pr, pg, pb) in enumerate(PAL):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if d < best:
            best, bi = d, i
    return bi


def convert(path):
    im = Image.open(path).convert("RGBA").resize((16, 16), Image.LANCZOS)
    px = im.load()
    bm = bytearray(128)   # 16 行 x 8 バイト (2 px/byte, 上位ニブル = 左)
    for y in range(16):
        for x in range(16):
            r, g, b, a = px[x, y]
            idx = 0 if a < 128 else nearest(r, g, b)   # 透明 -> 0 (黒)
            byi = y * 8 + x // 2
            if x & 1:
                bm[byi] |= idx
            else:
                bm[byi] |= idx << 4
    return bm


srcdir = sys.argv[1]
ents = []
if len(sys.argv) > 2:
    want = [l.split()[0] for l in open(sys.argv[2], encoding="utf-8") if l.strip() and not l.startswith("#")]
    paths = [os.path.join(srcdir, h + ".png") for h in want]
else:
    paths = sorted(glob.glob(os.path.join(srcdir, "*.png")))
for p in paths:
    name = os.path.splitext(os.path.basename(p))[0]
    try:
        cp = int(name, 16)
    except ValueError:
        continue
    ents.append((cp, convert(p)))
ents.sort()

out = sys.stdout
out.write("/* 自動生成 (genemoji.py)。絵文字 16x16・16色 (ANSI テキストパレットに量子化)。\n")
out.write(" * ソース: Twemoji 15.1.0 (CC-BY 4.0, (c) 2023 Twitter/X). 変換後の表のみ収録。 */\n")
out.write("#ifndef EMOJITAB_H\n#define EMOJITAB_H\n")
out.write("typedef struct { unsigned long cp; unsigned char bm[128]; } emoji_ent;\n")
out.write("static const emoji_ent emojitab[] = {\n")
for cp, bm in ents:
    out.write("{0x%x,{%s}},\n" % (cp, ",".join(str(b) for b in bm)))
out.write("};\n")
out.write("#define EMOJITAB_N %d\n" % len(ents))
out.write("#define EMOJITAB_MIN 0x%x   /* 表の最小コードポイント (これ未満は引かない) */\n" % ents[0][0])
out.write("""
/* コードポイント -> 絵文字インデックス (無ければ -1) */
static int emoji_index(unsigned long cp)
{
  int lo = 0, hi = EMOJITAB_N - 1;
  while (lo <= hi) {
    int m = (lo + hi) >> 1;
    unsigned long k = emojitab[m].cp;
    if (k == cp) return m;
    if (k < cp) lo = m + 1; else hi = m - 1;
  }
  return -1;
}
#endif
""")
sys.stderr.write("emojitab: %d entries\n" % len(ents))
