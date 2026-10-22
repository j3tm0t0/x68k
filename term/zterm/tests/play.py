#!/usr/bin/env python3
"""記録した端末出力 (term/vt100/tests/fixtures/*.rec) を、決まった速さで標準出力に流す (zterm の帯域試験用)。
usage: play.py FILE [CHUNK=256] [DELAY=0.02] [REPEAT=1]
Claude Code の応答の流し込みのように、小さな塊が細かい間隔で届く状況を作る。"""
import sys, time, os
f = sys.argv[1]
chunk = int(sys.argv[2]) if len(sys.argv) > 2 else 256
delay = float(sys.argv[3]) if len(sys.argv) > 3 else 0.02
rep = int(sys.argv[4]) if len(sys.argv) > 4 else 1
d = open(f, "rb").read()
out = os.fdopen(1, "wb", 0)
for _ in range(rep):
    for i in range(0, len(d), chunk):
        out.write(d[i:i + chunk])
        time.sleep(delay)
