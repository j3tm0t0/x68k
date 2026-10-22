#!/usr/bin/env python3
"""手書きのフィクスチャ (境界条件・制御シーケンス) を tests/fixtures/syn_*.rec に作る。96x32。
pyte が xterm と違う動きをする場面 (右端の全角など) は、期待画面を NAME.expect に手で書く
(run.sh は .expect があれば pyte の代わりにそれと文字を比べる)。
usage: gen_synth.py [FIXTUREDIR]"""
import os, sys

D = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
COLS, ROWS = 96, 32
E = "\x1b"
CSI = E + "["
fx = {}      # 名前 -> バイト列
expect = {}  # 名前 -> 期待画面 (行のリスト、足りない行は空白)


def at(r, c):
    return CSI + "%d;%dH" % (r, c)


# ---- 右端の全角・半角 (xterm の動き。pyte は右端 1 桁に全角を置いてしまうので期待画面を手で書く) ----
s = CSI + "H" + CSI + "2J"
s += at(1, 95) + "AB" + "C"                 # 95,96 桁に AB、遅延折り返しで C は次行頭
s += at(3, 96) + "漢"                       # 右端 1 桁に全角: 空けて次行頭へ
s += at(5, 95) + "字" + "x"                 # 95-96 にちょうど全角、x は次行頭
s += at(7, 1) + "日本語" + at(7, 2) + "a"    # 全角の右半分を半角で上書き: 左半分は空白に
s += at(8, 1) + "日本語" + at(8, 3) + "b"    # 2 字目の左半分を上書き: 右半分も空白に
s += at(9, 1) + "あいう" + at(9, 2) + "か"   # 半分ずらして全角で上書き: 両隣を割る
s += at(10, 94) + "ab漢"                     # 94,95 に ab、全角は 96 に入らないので次行
s += at(12, 1)
fx["syn_edge"] = s
ex = [""] * ROWS
ex[0] = " " * 94 + "AB"; ex[1] = "C"
ex[3] = "漢"
ex[4] = " " * 94 + "字"; ex[5] = "x"
ex[6] = " a本語"          # 日 の左半分は空白に
ex[7] = "日b 語"          # 本 の右半分は空白に
ex[8] = " か う"
ex[9] = " " * 93 + "ab"; ex[10] = "漢"
expect["syn_edge"] = ex

# ---- スクロール領域 (DECSTBM)・IND/RI・SU/SD・IL/DL・ICH/DCH/ECH ----
s = CSI + "H" + CSI + "2J"
for i in range(1, 33):
    s += at(i, 1) + "line%02d" % i + ("-" * 20)
s += CSI + "5;12r"                              # 領域 5-12
s += at(12, 1) + "\n\nNEW1"                      # 下端で LF: 領域内だけスクロール
s += at(5, 1) + E + "M" + E + "M" + "RI2"        # 上端で RI 2 回
s += CSI + "2S" + CSI + "T"                      # SU 2 / SD 1
s += at(7, 3) + CSI + "2L" + "IL"                # IL 2 (領域内)
s += at(9, 1) + CSI + "M"                        # DL 1
s += CSI + "r"                                   # 領域解除 (カーソルはホーム)
s += at(20, 5) + CSI + "3@" + "ins"              # ICH 3
s += at(21, 5) + CSI + "4P"                      # DCH 4
s += at(22, 5) + CSI + "6X"                      # ECH 6
s += at(23, 90) + CSI + "20@"                    # 右端近くで大きな ICH
s += at(24, 90) + CSI + "20P"                    # 右端近くで大きな DCH
s += CSI + "25;30r" + at(2, 1) + CSI + "L" + "outside"   # 領域外で IL (無視される)
s += at(27, 1) + CSI + "5M"                      # 領域内で DL 5 (領域を越える)
s += at(1, 1) + CSI + "r" + at(32, 1)
fx["syn_region"] = s

# ---- 代替画面 (1049 / 47 / 1047) ----
s = CSI + "H" + CSI + "2J" + "main screen text" + at(5, 10) + "cursor here"
s += CSI + "?1049h" + CSI + "H" + "ALT SCREEN" + at(10, 1) + "alt line"
s += CSI + "?1049l" + "+back"
s += CSI + "?47h" + "alt47" + CSI + "?47l"
s += at(8, 1) + CSI + "?1047h" + "alt1047" + CSI + "?1047l" + "!"
fx["syn_altscreen"] = s

# ---- SGR: 16 色・bold・reverse・underline・256 色・24bit (16 色への縮退) ----
s = CSI + "H" + CSI + "2J"
for i in range(8):
    s += CSI + "%dm F%d " % (30 + i, i) + CSI + "1m B%d " % i + CSI + "0m"
s += "\r\n"
for i in range(8):
    s += CSI + "%dm F%d " % (90 + i, i) + CSI + "%dm b%d " % (40 + i, i) + CSI + "%dm c%d " % (100 + i, i) + CSI + "0m"
s += "\r\n" + CSI + "7mrev" + CSI + "27m " + CSI + "4mul" + CSI + "24m " + CSI + "1;7;4;31;42mall" + CSI + "0m\r\n"
for n in range(256):
    s += CSI + "38;5;%dm#" % n
    if n % 64 == 63:
        s += CSI + "0m\r\n"
for n in range(0, 256, 4):
    s += CSI + "48;5;%dm " % n
s += CSI + "0m\r\n"
for r, g, b in [(215, 119, 87), (153, 153, 153), (102, 102, 102), (40, 40, 40), (255, 107, 128),
                (78, 186, 101), (177, 185, 249), (255, 193, 7), (0, 0, 0), (255, 255, 255), (61, 1, 0), (2, 40, 0)]:
    s += CSI + "38;2;%d;%d;%dm rgb " % (r, g, b) + CSI + "48;2;%d;%d;%dm  " % (r, g, b) + CSI + "0m"
s += "\r\n" + CSI + "38;2;;255;0;0mempty" + CSI + "0m\r\n"
s += CSI + "41m" + CSI + "K" + "bce" + CSI + "0m\r\n"      # 背景色付き消去 (BCE)
fx["syn_sgr"] = s

# ---- 端末への問い合わせ・モード・文字列シーケンス (無視されて画面が崩れないこと) ----
s = CSI + "H" + CSI + "2J" + "begin" + at(2, 1)
s += CSI + "c" + CSI + ">c" + CSI + "=c" + CSI + "5n" + CSI + "6n" + CSI + ">q" + CSI + "?u" + CSI + ">1u" + CSI + "<u" + CSI + "=5;1u"
s += CSI + "?2026h" + CSI + "?2026$p" + CSI + "?2004h" + CSI + "?1000h" + CSI + "?1006h" + CSI + "?1004h" + CSI + ">4;2m" + CSI + ">0;1m"
s += CSI + "18t" + CSI + "22;0t" + CSI + "23;0t" + CSI + "4 q" + CSI + "0 q" + CSI + "!p"
s += E + "]0;title here" + "\x07" + E + "]2;" + "T" * 5000 + E + "\\"
s += E + "]8;;https://example.com/x" + E + "\\" + "link" + E + "]8;;" + E + "\\"
s += E + "]133;A" + "\x07" + E + "]10;?" + "\x07" + E + "]11;?" + E + "\\" + E + "]52;c;aGVsbG8=" + "\x07"
s += E + "Ptmux;" + E + E + "]0;inner" + "\x07" + E + "\\"
s += E + "P+q544e" + E + "\\" + E + "_apc string" + E + "\\" + E + "^pm" + E + "\\"
s += "after-strings" + CSI + "38:5:196mcolon" + CSI + "0m" + E + "%G" + E + " F" + E + "*0" + at(3, 1)
s += CSI + "1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31mmany" + CSI + "0m"
s += CSI + "9" * 400 + "C" + "X" + CSI + "4;" + "9" * 30 + "H" + "Y"
s += at(5, 1) + CSI + "?1049h" + CSI + "?1049l" + CSI + "?25l" + CSI + "?25h" + CSI + "?12l" + CSI + "?7l"
s += at(6, 90) + "0123456789abcdef" + CSI + "?7h" + at(7, 1) + "end"
s += at(32, 1)
fx["syn_queries"] = s
# 応答 (vt_key から吐かれる順)。CSI =c / CSI >q / kitty の CSI ?u 等には答えない
REPLY = {"syn_queries": CSI + "?1;2c" + CSI + ">0;95;0c" + CSI + "0n" + CSI + "2;1R" + CSI + "?2026;0$y" +
         CSI + "8;32;96t" + E + "]10;rgb:aaaa/aaaa/aaaa\x07" + E + "]11;rgb:0000/0000/0000" + E + "\\"}

# ---- 罫線・DEC 特殊グラフィック・タブ・BS・CR ----
s = CSI + "H" + CSI + "2J"
s += "┌──┬──┐\r\n│ab│cd│\r\n├──┼──┤\r\n└──┴──┘\r\n"
s += E + "(0" + "lqqwqqk\r\nx  x  x\r\nmqqvqqj" + E + "(B" + "\r\n"
s += "a\tb\tc\r\n" + CSI + "3g" + E + "H" + "tabs\tx\r\n" + "abc\b\bX\rY\r\n"
s += "\x0e" + "q" + "\x0f" + "q" + E + ")0" + "\x0e" + "lqk" + "\x0f" + "\r\n"
fx["syn_lines"] = s

os.makedirs(D, exist_ok=True)
for name, data in fx.items():
    open(os.path.join(D, name + ".rec"), "wb").write(data.encode("utf-8"))
    ex = os.path.join(D, name + ".expect")
    if name in expect:
        rows = expect[name] + [""] * (ROWS - len(expect[name]))
        with open(ex, "w", encoding="utf-8") as f:
            for ln in rows:
                # 全角は 2 桁占めるが表示文字列では 1 文字 (vt_dump_text と同じ形) なので、桁数で詰める
                w = sum(2 if ord(ch) >= 0x1100 else 1 for ch in ln)
                f.write(ln + " " * (COLS - w) + "\n")
    if name in REPLY:
        open(os.path.join(D, name + ".reply"), "wb").write(REPLY[name].encode())
    # それ以外の .expect は、出力を目で確かめてから vthost の結果を写したもの (gen_synth.py では作らない)
# サイズ表: syn_* は 96x32
sizes = os.path.join(D, "sizes.txt")
lines = [ln for ln in (open(sizes).read().splitlines() if os.path.exists(sizes) else []) if not ln.startswith("syn_")]
lines += ["%s %d %d" % (n, COLS, ROWS) for n in fx]
open(sizes, "w").write("\n".join(sorted(lines)) + "\n")
print("wrote %d fixtures to %s" % (len(fx), D))
