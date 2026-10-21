#!/usr/bin/env python3
"""UTF-8(Unicode) -> JIS X0208 変換表を C ヘッダ (kanjitab.h) に生成する。
X68000 CGROM の 16x16 漢字フォントは JIS 区点で引く:
  addr = 0xF00000 + ((ku-1)*94 + (ten-1)) * 32   (ku=JIS上位-0x20, ten=JIS下位-0x20)
表は Unicode コードポイント昇順の {u16 ucs; u16 jis} 配列。vt100.c が二分探索で引く。
EUC-JP で全区点をデコードして Unicode を得る (機種依存文字を避けるため JIS X0208 のみ)。
usage: genkanji.py > kanjitab.h"""
import sys

pairs = []
for ku in range(1, 95):
    for ten in range(1, 95):
        euc = bytes([0x80 + 0x20 + ku, 0x80 + 0x20 + ten])
        try:
            s = euc.decode("euc_jp")
        except UnicodeDecodeError:
            continue
        if len(s) != 1:
            continue
        u = ord(s)
        if u > 0xFFFF:
            continue
        jis = ((ku + 0x20) << 8) | (ten + 0x20)
        pairs.append((u, jis))

pairs.sort()
# Unicode の重複を除く (最初の JIS を採用)
seen = set(); uniq = []
for u, j in pairs:
    if u in seen:
        continue
    seen.add(u); uniq.append((u, j))

out = sys.stdout
out.write("/* 自動生成 (genkanji.py)。UTF-8->JIS X0208。Unicode 昇順。 */\n")
out.write("#ifndef KANJITAB_H\n#define KANJITAB_H\n")
out.write("typedef struct { unsigned short ucs, jis; } kanji_ent;\n")
out.write("static const kanji_ent kanjitab[] = {\n")
for i, (u, j) in enumerate(uniq):
    out.write("{0x%04x,0x%04x}," % (u, j))
    if i % 6 == 5:
        out.write("\n")
out.write("\n};\n")
out.write("#define KANJITAB_N %d\n" % len(uniq))
out.write("""
/* Unicode -> JIS (見つからなければ 0) */
static unsigned short ucs_to_jis(unsigned long u)
{
  int lo = 0, hi = KANJITAB_N - 1;
  if (u > 0xffff) return 0;
  while (lo <= hi) {
    int m = (lo + hi) >> 1;
    unsigned short k = kanjitab[m].ucs;
    if (k == u) return kanjitab[m].jis;
    if (k < u) lo = m + 1; else hi = m - 1;
  }
  return 0;
}
#endif
""")
sys.stderr.write("kanjitab: %d entries\n" % len(uniq))
