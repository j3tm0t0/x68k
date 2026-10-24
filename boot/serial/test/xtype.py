#!/usr/bin/env python3
"""XEiJ の制御パイプ (RUNDIR/XEiJControl) にキーを打つ。人が打つのと同じキーの並びを送る。
usage: xtype.py RUNDIR TEXT...     ("\\n" は RETURN、"\\e" は ESC)
       xtype.py RUNDIR -f FILE     (FILE の中身をそのまま打つ。改行は RETURN)
環境変数 XTYPE_DELAY (秒、既定 0.3) で 1 打鍵の間隔を変える。"""
import os, sys, time
run = sys.argv[1]; args = sys.argv[2:]
pipe = os.path.join(run, "XEiJControl")
delay = float(os.environ.get("XTYPE_DELAY", "0.3"))
if args[:1] == ["-f"]:
    text = open(args[1], encoding="ascii").read()
else:
    text = " ".join(args).replace("\\n", "\n").replace("\\e", "\x1b")
plain = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
plain.update({" ": "space", "-": "minus", "^": "caret", "\\": "yen", "@": "at", "[": "leftbracket",
              "]": "rightbracket", ";": "semicolon", ":": "colon", ",": "comma", ".": "period",
              "/": "slash", "\n": "return", "\x1b": "esc", "\x08": "bs"})
shifted = {"!": "1", '"': "2", "#": "3", "$": "4", "%": "5", "&": "6", "'": "7", "(": "8", ")": "9",
           "=": "minus", "~": "caret", "|": "yen", "`": "at", "{": "leftbracket", "}": "rightbracket",
           "+": "semicolon", "*": "colon", "<": "comma", ">": "period", "?": "slash", "_": "underline"}
def send(cmd, d=delay):
    with open(pipe, "w") as f:
        f.write(cmd + "\n")
    time.sleep(d)
n = 0
for ch in text:
    if ch.isupper():
        send("presskey shift", 0.05); send("typekey " + ch.lower(), 0.05); send("releasekey shift")
    elif ch in plain:
        send("typekey " + plain[ch])
    elif ch in shifted:
        send("presskey shift", 0.05); send("typekey " + shifted[ch], 0.05); send("releasekey shift")
    else:
        sys.exit("xtype: unsupported char %r" % ch)
    n += 1
print("xtype: %d keys" % n, file=sys.stderr)
