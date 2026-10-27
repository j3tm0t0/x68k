#!/usr/bin/env python3
"""mk2hq.py OUT HOSTDIR -- HOSTDIR の中身を入れた 2HQ (1440KB、512 バイト x 18 セクタ) のイメージを作る。
XEiJ のドライブ C: (装置 2) に入れて、XDF イメージ (1232KB) の置き場にする。Human68k は FDDEVICE.X で 2HQ を読む。"""
import os, struct, subprocess, sys
out, host = sys.argv[1:3]
bs = bytearray(512)
bs[0:3] = b"\xeb\xfe\x90"
bs[3:11] = b"XDFTEST "
struct.pack_into("<HBHBHHBHHH", bs, 0x0B, 512, 1, 1, 2, 224, 2880, 0xF0, 9, 18, 2)
bs[510:512] = b"\x55\xaa"
base = out + ".base"
open(base, "wb").write(bs + bytes(2880 * 512 - 512))
subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../xdffs.py"),
                "build", out, base, "--add", host], check=True)
os.remove(base)
