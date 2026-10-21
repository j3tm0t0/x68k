#!/usr/bin/env python3
"""UTF-8 テキストを Human68k 向け Shift_JIS に変換する。
usage: totext.py [--crlf] SRC DST   (--crlf: 改行を CRLF にし末尾に ^Z を付ける)
"""
import sys
args = sys.argv[1:]
crlf = False
if args and args[0] == "--crlf":
    crlf = True; args = args[1:]
src, dst = args
text = open(src, encoding="utf-8").read()
if crlf:
    text = text.replace("\n", "\r\n") + "\x1a"
open(dst, "wb").write(text.encode("cp932"))
