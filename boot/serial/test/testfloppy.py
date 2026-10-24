#!/usr/bin/env python3
"""試験用のシステムフロッピーを作る: AUTOEXEC.BAT の最後に RSBUF64 を足し、RSBUF64.X を置く。
XEiJ が RSDRV.SYS の受信バッファを 1024 バイトに広げるので、起動時に実機と同じ 64 バイトに戻すため。
usage: testfloppy.py XDF RBCLI RSBUF64.X"""
import os, subprocess, sys, tempfile
xdf, rb, tool = sys.argv[1:4]
t = tempfile.mkdtemp()
def rbc(*a):
    subprocess.run([rb] + list(a), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
rbc("get", xdf, "/AUTOEXEC.BAT", t + "/ae")
body = open(t + "/ae", "rb").read().split(b"\x1a")[0]
if b"RSBUF64" not in body:
    open(t + "/ae", "wb").write(body + b"RSBUF64\r\n\x1a")
    rbc("rm", xdf, "/AUTOEXEC.BAT")
    rbc("put", xdf, t + "/ae", "/AUTOEXEC.BAT")
    rbc("put", xdf, tool, "/RSBUF64.X")
# rb-cli は拡張名の欄 (12-21 バイト目) を空白で埋めるが、Human68k は 0 埋めを期待するので直す
d = bytearray(open(xdf, "rb").read())
n = 0
for name in (b"AUTOEXECBAT", b"RSBUF64 X  "):
    i = d.find(name)
    while i >= 0:
        if i % 32 == 0 and d[i + 12:i + 22] == b" " * 10:
            d[i + 12:i + 22] = b"\0" * 10; n += 1
        i = d.find(name, i + 1)
open(xdf, "wb").write(d)
print("testfloppy: AUTOEXEC.BAT + RSBUF64.X (%d entries fixed)" % n, file=sys.stderr)
