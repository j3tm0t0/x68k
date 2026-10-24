#!/usr/bin/env python3
"""XEiJ のテキスト VRAM を読んで画面の文字を表示する (クリップボードを使わない)。
usage: xscreen.py RUNDIR [ROWS]
エージェント (RUNDIR/agent.cmd) に dump を頼み、プレーン 0〜3 の OR を CGROM の 8x16 ANK と照らし合わせる。
ANK 以外 (漢字など) は '?' になる。"""
import os, sys, time
run = sys.argv[1]
rows = int(sys.argv[2]) if len(sys.argv) > 2 else 31
def dump(name, addr, ln):
    p = os.path.join(run, name)
    if os.path.exists(p): os.remove(p)
    with open(os.path.join(run, "agent.cmd.tmp"), "w") as f:
        f.write("dump %s 0x%x 0x%x\n" % (p, addr, ln))
    os.rename(os.path.join(run, "agent.cmd.tmp"), os.path.join(run, "agent.cmd"))
    for _ in range(100):
        if os.path.exists(p) and os.path.getsize(p) == ln: return open(p, "rb").read()
        time.sleep(0.05)
    sys.exit("xscreen: dump timeout")
cg = os.path.join(run, "cgrom.bin")
font = open(cg, "rb").read() if os.path.exists(cg) else None
if not font or len(font) != 4096 or not any(font[0x41*16:0x42*16]):
    font = dump("cgrom.bin", 0xF3A800, 4096)
    if not any(font[0x41*16:0x42*16]):     # 起動の直後は CGROM がまだ用意されていない
        os.remove(cg)
glyph = {}
for c in range(0x20, 0x7f):
    glyph.setdefault(font[c*16:c*16+16], chr(c))
for c in range(0xa1, 0xe0):   # 半角カナ
    glyph.setdefault(font[c*16:c*16+16], chr(0xff61 + c - 0xa1))
pl = [dump("tv%d.bin" % k, 0xE00000 + k * 0x20000, 128*16*rows) for k in range(4)]
def cell(r, c, planes):
    return bytes(_or(r, c, s, planes) for s in range(16))
def _or(r, c, s, planes):
    v = 0
    for k in planes:
        v |= pl[k][(r*16+s)*128+c]
    return v
TRY = [(0, 1), (0, 1, 2, 3), (0,), (1,), (2,), (3,), (2, 3)]
for r in range(rows):
    line = []
    for c in range(96):
        ch = " "
        for planes in TRY:
            g = cell(r, c, planes)
            if not any(g):
                continue
            ch = glyph.get(g, glyph.get(bytes(255-b for b in g)))
            if ch:
                break
            ch = "?"
        line.append(ch)
    print("".join(line).rstrip())
