#!/usr/bin/env python3
"""記録 (.rec) から個人情報を消す。各語を同じバイト長の別の語に置き換える (桁位置を変えないため)。
usage: scrub.py REC WORD...   (空や 3 バイト未満の語は無視。長い語から順に置換)"""
import sys

path, words = sys.argv[1], [w.encode() for w in sys.argv[2:] if len(w.encode()) >= 3]
d = open(path, "rb").read()
for w in sorted(set(words), key=len, reverse=True):
    if w.startswith(b"/"):
        rep = b"/tmp/" + b"x" * max(0, len(w) - 5)
    else:
        rep = (b"user" + b"x" * len(w))
    d = d.replace(w, rep[:len(w)])
open(path, "wb").write(d)
