#!/usr/bin/env python3
"""Claude Code の入力欄に似せた小さな TUI (zterm の予測エコーの試験用。本物の claude は使わない)。

記録済みの app_claude フィクスチャの書き方をまねる:
  - 代替画面は使わない (主画面の下の方に枠付きの入力欄)
  - 描き直しのたびに ESC[?25l で隠し、ESC[H から CR と相対移動 (ESC[nB / ESC[nC) で入力行へ行き、
    "❯ " + 入力を描いて ESC[K、最後に ESC[<rows>;1H ESC[<r>;<c>H ESC[?25h で入力位置にカーソルを出す
  - 入力欄の下に状態行 (0.5 秒ごとに回るスピナーと打鍵数)。キーと関係なく描き直される
  - Enter で入力を上の履歴に流し (スクロール)、入力欄を空にする
  - BS・←→ (行の途中への挿入・削除)、Ctrl-C か Ctrl-D で終わる
usage: fakeclaude.py [--sync] [--slow MS]
  --sync     描き直しを ?2026 (同期更新) で囲む
  --slow MS  キーから描き直しまで MS ミリ秒待つ (遅いアプリの真似: 予測が先に出るのが見える)
"""
import os, sys, termios, tty, select, time, shutil

sync = "--sync" in sys.argv
slow = 0.0
if "--slow" in sys.argv:
    slow = float(sys.argv[sys.argv.index("--slow") + 1]) / 1000

out = os.fdopen(1, "wb", 0)
fd = 0
cols, rows = shutil.get_terminal_size((96, 32))
GREY = "\x1b[38;2;153;153;153m"
ORANGE = "\x1b[38;2;215;119;87m"
RST = "\x1b[39m"
SPIN = "·✢✳✶✻✽"
history = []
text = ""
pos = 0
keys = 0
tick = 0


def w(s):
    out.write(s.encode())


def redraw():
    box_top = rows - 6          # 1 起算の行番号
    inp = box_top + 1
    s = "\x1b[?2026h" if sync else ""
    s += "\x1b[?25l\x1b[H\r"
    # 履歴 (上の方)
    shown = history[-(box_top - 2):]
    for i in range(box_top - 2):
        s += (shown[i] if i < len(shown) else "") + "\x1b[K\r\n"
    s += "\r\n"
    s += GREY + "─" * cols + RST + "\r\n"                 # 枠の上
    s += GREY + "❯ " + RST + text + "\x1b[K\r\n"     # 入力行 (Claude Code は ❯ の後に NBSP)
    s += GREY + "─" * cols + RST + "\r\n"                 # 枠の下
    s += "  " + ORANGE + SPIN[tick % len(SPIN)] + RST + GREY + f" Thinking… keys {keys}" + RST + "\x1b[K\r\n"
    s += GREY + "  ? for shortcuts" + RST + "\x1b[K"
    s += f"\x1b[{rows};1H\x1b[{inp};{3 + pos}H\x1b[?25h"
    if sync:
        s += "\x1b[?2026l"
    w(s)


old = termios.tcgetattr(fd)
tty.setraw(fd)
try:
    w("\x1b[?2004h")
    redraw()
    last_tick = time.time()
    buf = b""
    while True:
        r, _, _ = select.select([fd], [], [], 0.5)
        if time.time() - last_tick >= 0.5:
            tick += 1
            last_tick = time.time()
            redraw()
        if not r:
            continue
        buf += os.read(fd, 64)
        while buf:
            if buf[:1] in (b"\x03", b"\x04"):
                raise SystemExit
            if buf[:3] in (b"\x1b[D", b"\x1bOD"):
                pos = max(0, pos - 1); buf = buf[3:]
            elif buf[:3] in (b"\x1b[C", b"\x1bOC"):
                pos = min(len(text), pos + 1); buf = buf[3:]
            elif buf[:1] == b"\x1b":
                buf = buf[1:]
            elif buf[:1] in (b"\x7f", b"\x08"):
                if pos > 0:
                    text = text[:pos - 1] + text[pos:]; pos -= 1
                buf = buf[1:]
            elif buf[:1] == b"\r":
                history.append(GREY + "> " + RST + text)
                text = ""; pos = 0; buf = buf[1:]
            else:
                # UTF-8 を 1 字ずつ
                n = 1
                b0 = buf[0]
                if b0 >= 0xf0: n = 4
                elif b0 >= 0xe0: n = 3
                elif b0 >= 0xc0: n = 2
                if len(buf) < n:
                    break
                ch = buf[:n].decode("utf-8", "replace"); buf = buf[n:]
                if ch.isprintable():
                    text = text[:pos] + ch + text[pos:]; pos += 1
            keys += 1
        if slow:
            time.sleep(slow)
        redraw()
except SystemExit:
    pass
finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
    w("\x1b[?2004l\r\n")
