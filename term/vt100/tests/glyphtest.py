#!/usr/bin/env python3
"""グリフ試験: エミュレータが VRAM に描いた全角/半角のグリフが正しい字か確かめる。
usage: glyphtest.py VTHOST CGROM [OUTDIR]

見本の文字を 1 画面に並べて vthost に描かせ、VRAM ダンプ (-v) から各セルの 16x16 (半角は 8x16) を切り出し、
  (A) CGROM の「正しい」アドレスのグリフと一致するか。アドレスはここで独立に計算する:
      Unicode -> JIS は Python の euc_jp コーデック、addr = ((ku'-1)*94 + (ten-1))*32、
      ku' = ku (1〜8 区) / ku-7 (16 区以降。CGROM には 9〜15 区が無い)。ANK は 0x3A800 + code*16。
  (B) システムの日本語フォント (ヒラギノ) で描いた見本との類似度で、最も似ている見本が自分自身か
      (アドレス規則そのものに依らない検査。7 区ずれると別の字に最も似る)。
を調べる。dd8e297 より前の計算 (ku-7 なし) では (A)(B) とも落ちる (run.sh が変異版で確かめる)。"""
import os, subprocess, sys, tempfile

# 見本: 漢字 (第 1/第 2 水準、16 区〜84 区)、かな、1〜8 区の記号・英数・ギリシャ・キリル
KANJI = "通無危漢字日本語亜腕弌熙鰯凜堯槇遙瑤"
KANA = "あいうをんアイウヲンヴヶ"
# 1〜8 区の記号は East Asian Width = W/F のものだけ (→ ○ ※ α 等の A (曖昧幅) は半角扱いで ASCII に寄せる)
SYMS = "、。・ー「」【】〜〒￥＄％＃＆＊＠ＡＺａｚ０９"
ASCII = "Ag~|{}"   # 半角: CGROM 8x16 (~ は自前グリフなので (A) では別扱い)

VBYTES, PLANE = 128, 512 * 128


def jis_of(ch):
    e = ch.encode("euc_jp")
    assert len(e) == 2, ch
    return e[0] - 0x80, e[1] - 0x80


def cgrom_kanji(cg, ch):
    hi, lo = jis_of(ch)
    ku, ten = hi - 0x20, lo - 0x20
    assert not (9 <= ku <= 15), ch
    kk = ku - 7 if ku >= 16 else ku
    off = ((kk - 1) * 94 + (ten - 1)) * 32
    return [((cg[off + y * 2] << 8) | cg[off + y * 2 + 1]) for y in range(16)]


def cgrom_ank(cg, ch):
    off = 0x3A800 + ord(ch) * 16
    return [cg[off + y] << 8 for y in range(16)]


def cell_bits(vram, col, row, wide):
    """プレーン 0..2 の OR (前景 7 = 白) を 16 行の 16bit で返す (半角は上位 8bit)"""
    out = []
    for s in range(16):
        o = (row * 16 + s) * VBYTES + col
        b0 = vram[o] | vram[PLANE + o] | vram[2 * PLANE + o]
        b1 = (vram[o + 1] | vram[PLANE + o + 1] | vram[2 * PLANE + o + 1]) if wide else 0
        out.append((b0 << 8) | b1)
    return out


def _img(bits):
    from PIL import Image
    im = Image.new("L", (16, 16), 0)
    px = im.load()
    for y in range(16):
        for x in range(16):
            if (bits[y] >> (15 - x)) & 1:
                px[x, y] = 255
    return im


def _prep(im):
    """1 ドット程度のずれ・線の太さの差に鈍くするため、ガウスぼかしした 16x16 の濃度列にする"""
    from PIL import ImageFilter
    return list(im.filter(ImageFilter.GaussianBlur(1.2)).getdata())


def render_font(chars):
    """ヒラギノで 64x64 に描いて 16x16 に縮めた見本 (無ければ None)"""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None
    cands = ["/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"]
    path = next((p for p in cands if os.path.exists(p)), None)
    if not path:
        return None
    font = ImageFont.truetype(path, 60)
    res = {}
    for ch in chars:
        im = Image.new("L", (64, 64), 0)
        ImageDraw.Draw(im).text((32, 32), ch, fill=255, font=font, anchor="mm")
        res[ch] = _prep(im.resize((16, 16), Image.BOX))
    return res


def corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    sa = sum((x - ma) ** 2 for x in a) ** 0.5
    sb = sum((x - mb) ** 2 for x in b) ** 0.5
    if sa == 0 or sb == 0:
        return 0.0
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb)


def main():
    vthost, cgpath = sys.argv[1], sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp()
    os.makedirs(outdir, exist_ok=True)
    cg = open(cgpath, "rb").read()
    wide = KANJI + KANA + SYMS
    # 1 行に全角 40 字 (80 桁) ずつ置く。カーソルは最後に (0,31) へ逃がす
    layout = []   # (ch, col, row, is_wide)
    data = bytearray(b"\x1b[H\x1b[2J")
    col = row = 0
    for ch in wide:
        if col + 2 > 80:
            col, row = 0, row + 1
        data += b"\x1b[%d;%dH" % (row + 1, col + 1) + ch.encode()
        layout.append((ch, col, row, True)); col += 2
    row += 1; col = 0
    for ch in ASCII:
        data += b"\x1b[%d;%dH" % (row + 1, col + 1) + ch.encode()
        layout.append((ch, col, row, False)); col += 2
    data += b"\x1b[32;1H"
    rec = os.path.join(outdir, "glyphs.rec")
    open(rec, "wb").write(data)
    vram_path = os.path.join(outdir, "glyphs.vram")
    subprocess.run([vthost, "-g", cgpath, "-s", "96x32", "-v", vram_path, rec, os.path.join(outdir, "glyphs.txt")], check=True)
    vram = open(vram_path, "rb").read()
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run([sys.executable, "-I", os.path.join(here, "vram2png.py"), vram_path,
                        os.path.join(outdir, "glyphs.png"), "2"], check=True)
    except Exception:
        pass

    fails = 0
    # (A) CGROM の正しいアドレスとの一致
    for ch, c, r, w in layout:
        got = cell_bits(vram, c, r, w)
        if w:
            exp = cgrom_kanji(cg, ch)
        elif ch == "~":
            continue
        else:
            exp = cgrom_ank(cg, ch)
        if got != exp:
            fails += 1
            hi, lo = jis_of(ch) if w else (0, ord(ch))
            print("  (A) NG %s (JIS %02x%02x) at col %d row %d: VRAM のグリフが CGROM の正しい位置と違う" % (ch, hi, lo, c, r))
    print("(A) CGROM アドレス: %d/%d 一致" % (len(layout) - fails - 1, len(layout) - 1))

    # (B) システムフォントとの最近傍 (漢字・かな 30 字の中で分類する。記号は字形の差が大きいので除く)。
    # 書体が違うので完全には当たらない: 「自分が 3 位以内」を全字に、「1 位」を漢字の 8 割以上に求める。
    ref = render_font(KANJI + KANA)
    failb = 0
    if ref is None:
        print("(B) スキップ (Pillow か日本語フォントが無い)")
    else:
        nk = top1k = nb = 0
        for ch, c, r, w in layout:
            if ch not in ref:
                continue
            nb += 1
            g = _prep(_img(cell_bits(vram, c, r, w)))
            scores = sorted(((corr(g, ref[k]), k) for k in ref), reverse=True)
            rank = [k for _, k in scores].index(ch) + 1
            if ch in KANJI:
                nk += 1
                top1k += (rank == 1)
            if rank > 3:
                failb += 1
                print("  (B) NG %s: 自分は %d 位、最も似ているのは %s" % (ch, rank, scores[0][1]))
        if top1k * 10 < nk * 8:
            failb += 1
            print("  (B) NG 漢字の 1 位率が低い (%d/%d)" % (top1k, nk))
        print("(B) フォント最近傍: 3 位以内 %d/%d、漢字の 1 位 %d/%d" % (nb - (failb if top1k * 10 >= nk * 8 else failb - 1), nb, top1k, nk))
    print("PNG: %s" % os.path.join(outdir, "glyphs.png"))
    sys.exit(1 if fails or failb else 0)


if __name__ == "__main__":
    main()
