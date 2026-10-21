#!/usr/bin/env python3
"""XEiJ の制御パイプ (build/XEiJControl) にキー入力を送る。
usage: xeijtype.py [--pipe PATH] TEXT...   ("\\n" は RETURN)
       xeijtype.py --raw "typekey ctrl c"   (制御コマンドをそのまま送る)
"""
import sys, time
pipe = "build/XEiJControl"
args = sys.argv[1:]
if args[:1] == ["--pipe"]:
    pipe = args[1]; args = args[2:]
raw = False
if args[:1] == ["--raw"]:
    raw = True; args = args[1:]
# X68000 (JIS 配列) のキー名とシフト状態
plain = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
plain.update({" ": "space", "-": "minus", "^": "caret", "\\": "yen", "@": "at", "[": "leftbracket",
              "]": "rightbracket", ";": "semicolon", ":": "colon", ",": "comma", ".": "period",
              "/": "slash", "\n": "return", "\t": "tab", "\x1b": "esc", "\x08": "bs"})
shifted = {"!": "1", '"': "2", "#": "3", "$": "4", "%": "5", "&": "6", "'": "7", "(": "8", ")": "9",
           "=": "minus", "~": "caret", "|": "yen", "`": "at", "{": "leftbracket", "}": "rightbracket",
           "+": "semicolon", "*": "colon", "<": "comma", ">": "period", "?": "slash", "_": "underline"}
def send(cmd):
    with open(pipe, "w") as f:
        f.write(cmd + "\n")
    time.sleep(0.15)
if raw:
    for a in args: send(a)
    sys.exit()
text = " ".join(args).replace("\\n", "\n").replace("\\e", "\x1b").replace("\\t", "\t")
def shift_type(key):
    # typekey shift X では shift が効かないため、押下と解放を別コマンドにする
    send("presskey shift"); send("typekey " + key); send("releasekey shift")
for ch in text:
    if ch.isupper():
        shift_type(ch.lower())
    elif ch in plain:
        send("typekey " + plain[ch])
    elif ch in shifted:
        shift_type(shifted[ch])
    else:
        sys.stderr.write("xeijtype: unsupported char %r\n" % ch)
