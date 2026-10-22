#!/usr/bin/env python3
"""pyte (参照レンダラ) とエミュレータの画面を、文字と属性 (前景・背景・下線) で比べる。
usage: compare.py REC COLS ROWS MINE.txt MINE.attr [EXPECT.txt]
  EXPECT.txt があれば文字はそれと比べ、属性は比べない (pyte が xterm と違う動きをする手書きフィクスチャ用)。
  MINE.txt / MINE.attr は vthost (vtcat) の出力 (vt_dump_text / vt_dump_attr)。
属性の期待値はエミュレータの規則で pyte の属性から作る:
  ・16 色 (30-37/90-97/40-47/100-107) はそのまま、bold は前景 0-7 を 8-15 に、reverse は前景と背景を入れ替える
  ・256 色・24bit 色 (pyte は 16 進 RGB で持つ) はパレット 16 色への「妥当な」縮退か調べる:
    色相 6 分割 + 明るさで選んだ色と同じか、元の色との距離が最寄りのパレット色との距離 + SLACK 以内なら可
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


def hue_family(rgb):
    """彩度があれば色相 6 分割 + 明るさ、無ければ明るさ 4 段 (vt100.c nearest16 と同じ考え方を独立に書いたもの)"""
    import colorsys
    r, g, b = rgb
    mx, mn = max(rgb), min(rgb)
    if mx - mn < 64:
        l = (r * 3 + g * 6 + b) / 10
        return 0 if l < 48 else 8 if l < 128 else 7 if l < 214 else 15
    h = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)[0] * 360
    idx = [1, 3, 2, 6, 4, 5][int(((h + 30) % 360) // 60)]
    return idx + 8 if mx >= 200 else idx


def ok_colour(got, exp_idx, exp_rgb):
    """16 色ならそのもの。RGB なら、色相・明るさで選んだ色か、RGB 距離で最寄りに近い (SLACK 以内) 色なら可"""
    if exp_idx is not None:
        return got == exp_idx
    if got == hue_family(exp_rgb):
        return True
    best = min(dist(p, exp_rgb) for p in PAL)
    return dist(PAL[got], exp_rgb) <= best + SLACK


class Screen(pyte.Screen):
    """pyte は '?' 付きの CSI (CSI ? 6 n、CSI ? 4 m など) を private=True で呼び、受け付けないメソッドだと
    例外で止まる。private 版は xterm でも別物なので無視する。"""
    def __getattribute__(self, name):
        f = pyte.Screen.__getattribute__(self, name)
        if name in ("report_device_status", "select_graphic_rendition", "erase_in_display", "erase_in_line",
                    "insert_characters", "delete_characters", "cursor_position", "set_margins", "clear_tab_stop",
                    "report_device_attributes", "define_charset", "cursor_up", "cursor_down", "cursor_forward",
                    "cursor_back", "erase_characters", "insert_lines", "delete_lines", "cursor_to_column",
                    "cursor_to_line", "linefeed", "index", "reverse_index", "tab", "backspace", "bell",
                    "carriage_return", "shift_in", "shift_out", "save_cursor", "restore_cursor", "draw"):
            def g(*a, **k):
                if k.pop("private", False):
                    return None
                return f(*a, **k)
            return g
        return f


def for_pyte(data):
    """pyte が誤読する (xterm では画面に影響しない) シーケンスを、pyte に渡す前に取り除く:
      ・CSI の前置 > < = (DA2、modifyOtherKeys CSI > 4;2 m、kitty CSI < u / CSI > 5 u など) — pyte は > を無視して
        CSI > 4;2 m を SGR 4 (下線) と読み、< は終端扱いで 'u' を表示してしまう
      ・中間バイト付き CSI (DECRQM CSI ? 2026 $ p、DECSCUSR CSI 2 SP q など)
      ・DCS / APC / PM / SOS 文字列 (pyte は中身を表示してしまう)"""
    import re
    data = re.sub(rb"\x1b\[[<>=][0-9;:]*[\x20-\x2f]*[\x40-\x7e]", b"", data)
    data = re.sub(rb"\x1b\[\??[0-9;:]*[\x20-\x2f]+[\x40-\x7e]", b"", data)
    data = re.sub(rb"\x1b[P_^X].*?(\x1b\\|\x07)", b"", data, flags=re.S)
    return data


def my_cells(line, attr_row, cols):
    """vt_dump_text の 1 行 (全角の右半分は出さない) を桁ごとの文字に戻す (右半分は "")"""
    out, it = [], iter(line)
    for c in range(cols):
        fl = int(attr_row[c].split(",")[2], 16)
        out.append("" if fl & 8 else next(it, " "))
    return out


def text_ok(mine, ref, mc, rc, wide_ref):
    """1 桁の比較。エミュレータの割り切り (未対応の字は ASCII / DEC 罫線 / '?' に寄せる) を許す:
      ・pyte が半角の非 ASCII で、こちらが ASCII か罫線 → 代用 (fallback)
      ・pyte が全角で、こちらが "??" か「代用字 + 空白」(2 桁) → 代用
      ・pyte が結合文字列 (é = e + U+0301) で、こちらが先頭の字 → 合成しない割り切り"""
    if mine == ref:
        return "same"
    if ref and ord(ref[0]) >= 0x80:
        if wide_ref:
            return "fallback" if mine and (ord(mine) < 0x80 or 0x2500 <= ord(mine) <= 0x25ff) else None
        if len(ref) > 1 and mine == ref[0]:
            return "fallback"
        if mine and (ord(mine) < 0x80 or 0x2500 <= ord(mine) <= 0x25ff or 0x23ba <= ord(mine) <= 0x23bd):
            return "fallback"
    if ref == "" and mine in ("?", " ") and mc and rc and ord(rc[0]) >= 0x80 and mc != rc:   # 全角の代用の右側
        return "fallback"
    return None


def main():
    rec, cols, rows, mine_txt, mine_attr = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    screen = Screen(cols, rows)
    pyte.ByteStream(screen).feed(for_pyte(open(rec, "rb").read()))
    mine = open(mine_txt, encoding="utf-8").read().split("\n")
    attrs = [ln.split() for ln in open(mine_attr).read().split("\n")]
    errs = []
    if len(sys.argv) > 6:
        exp = open(sys.argv[6], encoding="utf-8").read().split("\n")
        for r in range(rows):
            if mine[r] != exp[r]:
                errs.append("text row %2d\n   mine:   |%s|\n   expect: |%s|" % (r, mine[r], exp[r]))
        for e in errs[:8]:
            print("   " + e)
        sys.exit(1 if errs else 0)
    from wcwidth import wcwidth
    nfb = 0
    for r in range(rows):
        mc = my_cells(mine[r] if r < len(mine) else "", attrs[r], cols)
        bad = []
        for c in range(cols):
            ref = screen.buffer[r][c].data
            prev = screen.buffer[r][c - 1].data if c else ""
            wide_ref = bool(ref) and wcwidth(ref[0]) == 2
            k = text_ok(mc[c], ref, mc[c - 1] if c else "", prev, wide_ref)
            if k is None:
                bad.append(c)
            elif k == "fallback":
                nfb += 1
        if bad:
            errs.append("text row %2d (col %s)\n   mine: |%s|\n   pyte: |%s|" % (r, ",".join(map(str, bad[:6])),
                        mine[r] if r < len(mine) else "", screen.display[r]))
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
    if nfb:
        print("   (代用表示 %d 桁)" % nfb)
    if errs:
        for e in errs[:8]:
            print("   " + e)
        if len(errs) > 8:
            print("   ... %d more" % (len(errs) - 8))
        sys.exit(1)


if __name__ == "__main__":
    main()
