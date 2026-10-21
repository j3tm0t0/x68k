#!/usr/bin/env python3
"""参照レンダラ: VT100 バイト列ファイルを pyte に食わせ、画面を UTF-8 で書き出す。
usage: pyte_ref.py INFILE OUTFILE COLS ROWS
出力は vtdump.c の vt_dump_text と同じ形式 (1 行 = COLS 文字、ROWS 行)。"""
import sys
import pyte

infile, outfile, cols, rows = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(infile, "rb").read()
screen = pyte.Screen(cols, rows)
stream = pyte.ByteStream(screen)
stream.feed(data)
with open(outfile, "w", encoding="utf-8") as f:
    for line in screen.display:
        # pyte は COLS 幅に空白詰めして返す
        f.write(line[:cols].ljust(cols) + "\n")
