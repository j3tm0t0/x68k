#!/usr/bin/env python3
"""文字幅の表 (widthtab.h) を生成する。全角 (2 桁) と幅 0 (結合文字・異体字セレクタなど) のコードポイント範囲。
ホスト側のアプリ (Claude Code の string-width、tmux、vim、pyte が使う wcwidth) は East Asian Width と
絵文字の表示幅で桁を数えるので、端末側も同じ表で数えないと行の後ろがずれる (⏰ ✅ ⚡ など BMP の絵文字は W)。
wcwidth (pyte と同じもの) の表から作る。tests/venv の python で:
usage: tests/venv/bin/python genwidth.py > widthtab.h"""
import sys
from wcwidth import wcwidth


def ranges(pred):
    out, start = [], None
    for cp in range(0x110000):
        if 0xd800 <= cp <= 0xdfff:
            ok = False
        else:
            ok = pred(cp)
        if ok and start is None:
            start = cp
        elif not ok and start is not None:
            out.append((start, cp - 1))
            start = None
    if start is not None:
        out.append((start, 0x10ffff))
    return out


import wcwidth as _w
wide = ranges(lambda cp: wcwidth(chr(cp)) == 2)
zero = ranges(lambda cp: cp >= 0xa0 and wcwidth(chr(cp)) == 0)


def emit(name, rs):
    print("static const unsigned long %s[][2] = {" % name)
    line = ""
    for a, b in rs:
        item = "{0x%x,0x%x}," % (a, b)
        if len(line) + len(item) > 110:
            print(line)
            line = ""
        line += item
    print(line)
    print("};")
    print("#define %s_N (sizeof %s / sizeof %s[0])" % (name.upper(), name, name))


print("/* 自動生成 (genwidth.py、wcwidth %s)。全角 2 桁と幅 0 のコードポイント範囲 (昇順・両端含む)。 */" % _w.__version__)
print("#ifndef WIDTHTAB_H")
print("#define WIDTHTAB_H")
emit("widetab", wide)
emit("zerotab", zero)
print("""
/* 範囲表の二分探索。含まれれば 1 */
static int in_ranges(const unsigned long (*t)[2], int n, unsigned long cp)
{
  int lo = 0, hi = n - 1;
  if (n == 0 || cp < t[0][0] || cp > t[n - 1][1]) return 0;
  while (lo <= hi) {
    int m = (lo + hi) >> 1;
    if (cp < t[m][0]) hi = m - 1;
    else if (cp > t[m][1]) lo = m + 1;
    else return 1;
  }
  return 0;
}
#endif""")
sys.stderr.write("wide %d ranges, zero %d ranges\n" % (len(wide), len(zero)))
