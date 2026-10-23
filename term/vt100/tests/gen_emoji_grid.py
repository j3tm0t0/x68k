#!/usr/bin/env python3
"""絵文字表 (emojilist.txt) の全部を並べたフィクスチャ fixtures/emoji_grid.rec (96x32) を作る。
シェルを通さない合成のバイト列。1 行 30 字 (空白区切り)、行頭に最初の字の番号。最後の行で、wcwidth 1 の
テキスト表示絵文字が 1 桁のまま (表に入れていない) ことも見せる。
U+FE0F 付きは pyte が桁を崩すので入れない (vt100.c は U+FE0F を捨てる)。
usage: gen_emoji_grid.py   (tests/ で)"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
cps = [int(l.split()[0], 16) for l in open(os.path.join(HERE, "..", "emojilist.txt"), encoding="utf-8")
       if l.strip() and not l.startswith("#")]
out = ["\x1b[H\x1b[2J\x1b[1memojitab: %d\x1b[0m (Twemoji 15.1.0, CC-BY 4.0)\r\n\r\n" % len(cps)]
for i in range(0, len(cps), 30):
    out.append("%03d " % i + " ".join(chr(c) for c in cps[i:i + 30]) + "\r\n")
out.append("\r\nwcwidth 1: [❤] [⚠] [☀] [➡] [✔]"
           "  mix: sushi\U0001f363 beer\U0001f37a ok\U0001f197 \x1b[7m\U0001f525rev\x1b[0m\r\n")
open(os.path.join(HERE, "fixtures", "emoji_grid.rec"), "wb").write("".join(out).encode("utf-8"))
