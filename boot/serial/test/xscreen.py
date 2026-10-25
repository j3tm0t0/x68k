#!/usr/bin/env python3
"""XEiJ のテキスト VRAM を読んで画面の文字を表示する (クリップボードを使わない)。
usage: xscreen.py RUNDIR [ROWS]
エージェント (RUNDIR/agent.cmd) に dump を頼み、プレーン 0/1 の OR を CGROM の 8x16 ANK と照らし合わせる。
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
if not font or len(font) != 4096:
    font = dump("cgrom.bin", 0xF3A800, 4096)
glyph = {}
for c in range(0x20, 0x7f):
    glyph.setdefault(font[c*16:c*16+16], chr(c))
for c in range(0xa1, 0xe0):   # 半角カナ
    glyph.setdefault(font[c*16:c*16+16], chr(0xff61 + c - 0xa1))
p0 = dump("tv0.bin", 0xE00000, 128*16*rows)
p1 = dump("tv1.bin", 0xE20000, 128*16*rows)
for r in range(rows):
    line = []
    for c in range(96):
        g = bytes((p0[(r*16+s)*128+c] | p1[(r*16+s)*128+c]) for s in range(16))
        if not any(g): line.append(" ")
        else: line.append(glyph.get(g, glyph.get(bytes(255-b for b in g), "?")))
    print("".join(line).rstrip())
