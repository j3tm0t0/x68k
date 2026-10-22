#!/usr/bin/env python3
"""pyte (参照レンダラ) とエミュレータの画面を、文字と属性 (前景・背景・下線) で比べる。
usage: compare.py REC COLS ROWS MINE.txt MINE.attr
  MINE.txt / MINE.attr は vthost (vtcat) の出力 (vt_dump_text / vt_dump_attr)。
属性の期待値はエミュレータの規則で pyte の属性から作る:
  ・16 色 (30-37/90-97/40-47/100-107) はそのまま、bold は前景 0-7 を 8-15 に、reverse は前景と背景を入れ替える
  ・256 色・24bit 色 (pyte は 16 進 RGB で持つ) はパレット 16 色への「妥当な」縮退か調べる:
    エミュレータの色と元の色の距離が、最も近いパレット色との距離 + SLACK 以内なら可
差分があれば最初の数件を出して終了コード 1。"""
import sys
import pyte

# vt100.c palette_setup の表 (0..31) を 8bit にしたもの
PAL5 = [(0, 0, 0), (21, 0, 0), (0, 21, 0), (21, 21, 0), (0, 0, 21), (21, 0, 21), (0, 21, 21), (21, 21, 21),
        (10, 10, 10), (31, 0, 0), (0, 31, 0), (31, 31, 0), (0, 0, 31), (31, 0, 31), (0, 31, 31), (31, 31, 31)]
PAL = [tuple(v * 255 // 31 for v in c) for c in PAL5]
NAMES = ["black", "red", "green", "brown", "blue", "magenta", "cyan", "white"]
SLACK = 60


def name_idx(name, default):
    """pyte の色 -> (パレット番号 or None, RGB or None)"""
    if name == "default":
        return default, None
    if name in NAMES:
        return NAMES.index(name), None
    if name.startswith("bright") or name.startswith("bfight"):   # pyte の綴り間違い (bfightmagenta)
        return NAMES.index(name[6:]) + 8, None
    return None, tuple(int(name[i:i + 2], 16) for i in (0, 2, 4))


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def ok_colour(got, exp_idx, exp_rgb):
    if exp_idx is not None:
        return got == exp_idx
    best = min(dist(p, exp_rgb) for p in PAL)
    return dist(PAL[got], exp_rgb) <= best + SLACK


def main():
    rec, cols, rows, mine_txt, mine_attr = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    screen = pyte.Screen(cols, rows)
    pyte.ByteStream(screen).feed(open(rec, "rb").read())
    mine = open(mine_txt, encoding="utf-8").read().split("\n")
    attrs = [ln.split() for ln in open(mine_attr).read().split("\n")]
    errs = []
    for r, line in enumerate(screen.display):
        if r >= len(mine) or mine[r] != line:
            errs.append("text row %2d\n   mine: |%s|\n   pyte: |%s|" % (r, mine[r] if r < len(mine) else "", line))
    for r in range(rows):
        for c in range(cols):
            ch = screen.buffer[r][c]
            fg, bg, fl = (int(x, 16) for x in attrs[r][c].split(","))
            if fl & 8:   # 全角の右半分は左に従う
                continue
            fi, frgb = name_idx(ch.fg, 7)
            bi, brgb = name_idx(ch.bg, 0)
            if ch.bold and fi is not None and fi < 8:
                fi += 8
            if ch.bold and fi is None:   # 256 色の前景に bold: エミュレータは縮退後 0-7 なら 8 を足す
                frgb = None; fi = fg if fg >= 8 else -1
            if ch.reverse:
                fi, frgb, bi, brgb = bi, brgb, fi, frgb
            good = ok_colour(fg, fi, frgb) and ok_colour(bg, bi, brgb) and bool(fl & 1) == bool(ch.underscore)
            if not good:
                errs.append("attr r%d c%d %r: mine fg=%d bg=%d ul=%d / pyte fg=%s bg=%s bold=%d rev=%d ul=%d" % (
                    r, c, ch.data, fg, bg, fl & 1, ch.fg, ch.bg, ch.bold, ch.reverse, ch.underscore))
    if errs:
        for e in errs[:8]:
            print("   " + e)
        if len(errs) > 8:
            print("   ... %d more" % (len(errs) - 8))
        sys.exit(1)


if __name__ == "__main__":
    main()
