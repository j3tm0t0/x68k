#!/usr/bin/env python3
"""pty レコーダ: 固定ウィンドウサイズで子プロセスを動かし、出力バイト列をそのまま記録する。
macOS の `script` と違いヘッダ/フッタを混ぜないので、決定的なフィクスチャが作れる。

usage: record.py OUT COLS ROWS TERM SCRIPT -- CMD [ARGS...]
  SCRIPT は「入力イベント」を書いた小さなテキスト。1 行 1 イベント:
    wait MS            MS ミリ秒待つ (出力を読みながら)
    send TEXT          TEXT を子に送る (\\r \\n \\t \\e \\xNN を解釈)
    key up/down/...    よく使うキーのエスケープを送る
    until MS           出力が MS ミリ秒途切れるまで読む (最大 10s)
"""
import sys, os, pty, time, select, termios, struct, fcntl, signal

def parse_escapes(s):
    out = bytearray(); i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i+1]
            m = {"r": b"\r", "n": b"\n", "t": b"\t", "e": b"\x1b", "\\": b"\\", "0": b"\x00"}
            if n in m: out += m[n]; i += 2; continue
            if n == "x" and i + 3 < len(s): out.append(int(s[i+2:i+4], 16)); i += 4; continue
        out += c.encode("utf-8"); i += 1
    return bytes(out)

KEYS = {"up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
        "home": b"\x1b[H", "end": b"\x1b[F", "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~",
        "enter": b"\r", "esc": b"\x1b", "space": b" ", "tab": b"\t"}

args = sys.argv[1:]
dd = args.index("--")
out, cols, rows, term, script = args[0], int(args[1]), int(args[2]), args[3], args[4]
cmd = args[dd+1:]
events = [ln.strip() for ln in open(script) if ln.strip() and not ln.startswith("#")]

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = term
    os.environ["COLUMNS"] = str(cols); os.environ["LINES"] = str(rows)
    os.environ["LANG"] = "en_US.UTF-8"
    os.execvp(cmd[0], cmd)

# set window size on the pty master
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
captured = bytearray()

def drain(ms):
    end = time.time() + ms / 1000.0
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if r:
            try: d = os.read(fd, 65536)
            except OSError: return False
            if not d: return False
            captured.extend(d)
    return True

def drain_until_quiet(ms, hardcap=10.0):
    start = time.time()
    while time.time() - start < hardcap:
        r, _, _ = select.select([fd], [], [], ms / 1000.0)
        if r:
            try: d = os.read(fd, 65536)
            except OSError: break
            if not d: break
            captured.extend(d)
        else:
            break

for ev in events:
    p = ev.split(None, 1)
    op = p[0]
    arg = p[1] if len(p) > 1 else ""
    if op == "wait": drain(int(arg))
    elif op == "until": drain_until_quiet(int(arg))
    elif op == "send": os.write(fd, parse_escapes(arg))
    elif op == "key":
        for k in arg.split(): os.write(fd, KEYS.get(k, b""))
    else: pass

drain_until_quiet(300)
try: os.kill(pid, signal.SIGKILL)
except OSError: pass
try: os.waitpid(pid, 0)
except OSError: pass
open(out, "wb").write(bytes(captured))
sys.stderr.write("recorded %d bytes -> %s\n" % (len(captured), out))
