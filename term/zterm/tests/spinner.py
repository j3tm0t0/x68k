#!/usr/bin/env python3
"""Claude Code の待ち表示のような「全画面をほぼ同じ内容で描き直す」負荷を作る (zterm の帯域試験用)。
usage: spinner.py SECONDS [FPS=20]
毎回 ESC[H から 30 行を SGR 付きで描き直し、変わるのはスピナーと経過時間と数行だけ。"""
import sys, time, os
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
fps = float(sys.argv[2]) if len(sys.argv) > 2 else 20
out = os.fdopen(1, "wb", 0)
spin = "|/-\\"
t0 = time.time(); n = 0
out.write(b"\x1b[?1049h\x1b[?25l")
while time.time() - t0 < secs:
    lines = []
    for r in range(30):
        if r == 2:
            lines.append("\x1b[38;2;215;119;87m%s\x1b[0m Thinking... (%.1fs, esc to interrupt)" % (spin[n % 4], time.time() - t0))
        elif r % 7 == 3:
            lines.append("\x1b[1;34m## section %d\x1b[0m line %d tokens %d" % (r, n // 10, n * 3))
        else:
            lines.append("\x1b[38;5;250m  the quick brown fox jumps over the lazy dog %02d \x1b[38;5;245m(unchanged text)\x1b[0m" % r)
    out.write(("\x1b[H" + "\r\n".join("\x1b[2K" + l for l in lines)).encode())
    n += 1
    time.sleep(1 / fps)
out.write(b"\x1b[?25h\x1b[?1049l")
