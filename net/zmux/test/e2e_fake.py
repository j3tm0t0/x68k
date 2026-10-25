#!/usr/bin/env python3
"""XEiJ が無い機械で zmuxd と zmux (TUI) を端から端まで試す。X68000 の代わりは fakex68k.py (線の約束を話す、19200bps の速さの真似)。
  1. zmuxd が HELLO で UP になる。zmux attach 0 で COMMAND.X (のふり) に ver が通る
  2. リモートドライブ (チャネル 2): INIT / MKDIR / CREATE+WRITE / OPEN+READ (照合) / FILES+NFILES / RENAME / DELETE / RMDIR / DSKFRE。
     40KB の READ と WRITE の速さ (19200bps の真似)
  3. zmux (TUI) を pty で動かし、pyte で画面を読む: タブ 0 の状態の行、ver、Ctrl+X ? の説明、F13 → キー入力の注入、
     CSI u の並びがそのまま届く、Ctrl+X c / | / k (0xA7 のふりの zosh)、マウスでタブを選ぶ、Ctrl+X d で切り離し → 配置が残る → つなぎ直し
usage: e2e_fake.py [RUNDIR]   (ZMUX=zmux のバイナリ、BAUD=0 で速さの真似を切る)"""
import os, struct, subprocess, sys, tempfile, time, threading

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import fakex68k  # noqa: E402
from tuipty import Tui as _Tui, wait_until  # noqa: E402

zm = os.path.dirname(here)
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
BAUD = int(os.environ.get("BAUD", "19200"))
run = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="zmux-e2e-")
os.makedirs(run, exist_ok=True)
PORT = int(os.environ.get("XPORT", "54400"))
sock = "/tmp/zmux-fk-%d.sock" % os.getpid()
home = os.path.join(run, "home")
os.makedirs(home, exist_ok=True)
drive = os.path.join(run, "drive")
os.makedirs(drive, exist_ok=True)
env = dict(os.environ, ZMUX_SOCK=sock, HOME=home, TERM="xterm-256color", LANG="C.UTF-8")
fails = []


def Tui(cols=80, rows=24):
    return _Tui(ZBIN, env, cols, rows)


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ("  " + extra if extra else ""), flush=True)
    if not cond:
        fails.append(name)


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


def query(cmd):
    import socket
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    s.sendall((cmd + "\n").encode())
    out = b""
    while not out.endswith(b"\n"):
        d = s.recv(4096)
        if not d:
            break
        out += d
    s.close()
    return out.decode().strip()


class Attach:
    def __init__(self, *args):
        self.p = subprocess.Popen([ZBIN, "attach"] + list(args), stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.buf = bytearray()
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        while True:
            d = self.p.stdout.read1(4096)
            if not d:
                return
            self.buf += d

    def send(self, s):
        self.p.stdin.write(s if isinstance(s, bytes) else s.encode())
        self.p.stdin.flush()

    def wait(self, pat, t=10):
        pat = pat if isinstance(pat, bytes) else pat.encode()
        end = time.time() + t
        while time.time() < end:
            i = self.buf.find(pat)
            if i >= 0:
                out = bytes(self.buf[:i + len(pat)])
                del self.buf[:i + len(pat)]
                return out
            time.sleep(0.05)
        return None

    def close(self):
        self.p.kill()


# ---------------------------------------------------------------- 1. 線とコンソール
fake = fakex68k.Fake(PORT, BAUD)
zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % PORT, "--baud", "19200", "--drive", drive], env=env,
                      stderr=open(os.path.join(run, "zmuxd.log"), "w"))
try:
    t0 = time.time()
    for _ in range(200):
        time.sleep(0.1)
        if status().startswith("line up"):
            break
    check("line up", status().startswith("line up"), "%.1fs: %s" % (time.time() - t0, status()))
    a = Attach("0")
    time.sleep(0.3)
    fake.prompt()
    check("attach 0: prompt", a.wait("A>") is not None)
    a.send("ver\r")
    out = a.wait("A>")
    check("attach 0: ver over the line", out is not None and b"fake X68000" in out, repr(out)[-80:])
    a.close()

    # ---------------------------------------------------------------- 2. リモートドライブ
    import importlib.util
    E_NOMORE = -18
    def nb(dir_, name):
        b = bytearray(88)
        p = bytearray([9])
        for seg in [s for s in dir_.split("\\") if s]:
            p += seg.encode("cp932") + b"\t"
        b[2:2 + len(p)] = p
        n, _, e = name.partition(".")
        n1 = n[:8].encode("cp932").ljust(8, b" ")
        b[67:75] = n1
        n2 = n[8:18].encode("cp932")
        b[78:78 + len(n2)] = n2
        b[75:78] = e[:3].encode("cp932").ljust(3, b" ")
        return bytes(b)
    def rd(cmd):
        return fake.rd_xchg(cmd)
    check("rd: init", rd(b"\x00") == b"\x00")
    check("rd: mkdir SUB", rd(b"\x02" + nb("", "SUB")) == b"\x00" and os.path.isdir(os.path.join(drive, "SUB")))
    data = os.urandom(40000)
    fcb = 0x1000
    check("rd: create", rd(b"\x09\x20\x00" + struct.pack(">I", fcb) + nb("\\SUB", "DATA.BIN")) == b"\x00")
    t1 = time.time(); pos = 0
    while pos < len(data):
        n = min(4084, len(data) - pos)
        r = rd(b"\x0d" + struct.pack(">IIH", fcb, pos, n) + data[pos:pos + n])
        if struct.unpack(">h", r[:2])[0] != n:
            check("rd: write", False, repr(r)); break
        pos += n
    tw = time.time() - t1
    check("rd: close", rd(b"\x0b" + struct.pack(">I", fcb)) == b"\x00")
    check("rd: written file matches", open(os.path.join(drive, "SUB", "DATA.BIN"), "rb").read() == data)
    r = rd(b"\x0a\x00" + struct.pack(">I", 0x2000) + nb("\\SUB", "DATA.BIN"))
    check("rd: open", r[0] == 0 and struct.unpack(">I", r[1:5])[0] == len(data), repr(r))
    t1 = time.time(); got = b""
    while len(got) < len(data):
        r = rd(b"\x0c" + struct.pack(">IIH", 0x2000, len(got), 4084))
        n = struct.unpack(">h", r[:2])[0]
        if n <= 0:
            break
        got += r[2:2 + n]
    tr = time.time() - t1
    check("rd: read back matches", got == data)
    print("rd: 40000 bytes: write %.2f KB/s, read %.2f KB/s (%s)" % (len(data) / 1024 / tw, len(data) / 1024 / tr,
          "19200bps の真似" if BAUD else "速さの真似なし"))
    rd(b"\x0b" + struct.pack(">I", 0x2000))
    r = rd(b"\x07\x37" + struct.pack(">I", 0x5000) + nb("\\SUB", "????????.???"))
    check("rd: files", r[0] == 0 and r[11:19] == b"DATA.BIN" and struct.unpack(">I", r[7:11])[0] == len(data), repr(r[:20]))
    r = rd(b"\x08" + struct.pack(">I", 0x5000))
    check("rd: nfiles end", len(r) >= 1 and struct.unpack("b", r[:1])[0] == E_NOMORE)
    check("rd: rename", rd(b"\x04" + nb("\\SUB", "DATA.BIN") + nb("\\SUB", "DATA2.BIN")) == b"\x00" and os.path.exists(os.path.join(drive, "SUB", "DATA2.BIN")))
    check("rd: delete", rd(b"\x05" + nb("\\SUB", "DATA2.BIN")) == b"\x00" and not os.path.exists(os.path.join(drive, "SUB", "DATA2.BIN")))
    check("rd: rmdir", rd(b"\x03" + nb("", "SUB")) == b"\x00" and not os.path.exists(os.path.join(drive, "SUB")))
    r = rd(b"\x10")
    check("rd: dskfre", len(r) == 12 and struct.unpack(">H", r[8:10])[0] == 128, repr(r))
    st = status()
    check("status shows drive stats", "drive 1" in st and "rdreq" in st, st[-60:])

    # ---------------------------------------------------------------- 3. TUI
    fake.zss_announce()        # zos.x が動いていることにする
    time.sleep(0.5)
    t = Tui()
    check("tui: status line with tab 0", t.wait(" 0:COMMAND.X "), repr(t.lines()[-1]))
    fake.prompt()
    check("tui: console prompt", t.wait("A>"), repr(t.lines()[:3]))
    n0 = len(fake.con_in)
    seen = t.text().count("fake X68000")      # つなぐと直前の出力 (スクロールバック) が流れてくるので、その分は数えない
    t.send("ver\r")
    check("tui: ver typed into tab 0 reaches the X68000 and the output is drawn",
          wait_until(lambda: fake.con_in[n0:n0 + 4] == b"ver\r") and wait_until(lambda: t.text().count("fake X68000") > seen), repr(bytes(fake.con_in[n0:])))
    # CSI u (拡張キー) はそのまま
    n0 = len(fake.con_in)
    t.send("\x1b[97;5u")
    check("tui: CSI u sequence passes through verbatim", wait_until(lambda: fake.con_in[n0:] == b"\x1b[97;5u"), repr(bytes(fake.con_in[n0:])))
    # keys.toml: F13 → XF1 の注入 (チャネル 3)
    t.send("\x1b[25~")
    check("tui: F13 on tab 0 becomes an XF1 key injection", wait_until(lambda: 0x5500 in fake.keys), repr(fake.keys))
    # Ctrl+X ? の説明。次のキーは閉じるだけで X68000 へは行かない
    n0 = len(fake.con_in)
    t.send("\x18?")
    check("tui: C-x ? shows the help", t.wait("前置キー"), repr(t.lines()[8:14]))
    t.send("q")
    ok = t.wait_gone("前置キー")
    time.sleep(0.6)
    check("tui: help closes, the key is not forwarded", ok and len(fake.con_in) == n0)
    # Ctrl+X Ctrl+X = Ctrl+X そのもの
    n0 = len(fake.con_in)
    t.send("\x18\x18")
    check("tui: C-x C-x sends a literal C-x", wait_until(lambda: fake.con_in[n0:] == b"\x18"), repr(bytes(fake.con_in[n0:])))
    # 新しいタブ (zosh のふり)
    t.send("\x18c")
    check("tui: C-x c opens a zosh tab", t.wait(" 1:zosh 1 ") and t.wait("z$"), repr(t.lines()[-1]))
    t.send("echo hello-zosh\r")
    check("tui: typing goes to the zosh pane", t.wait("hello-zosh"), repr(t.lines()[:4]))
    # 左右に割る → 2 つ目のセッション
    t.send("\x18|")
    check("tui: C-x | splits (second zosh session)", t.wait(" 1:zosh 1+1 "), repr(t.lines()[-1]))
    check("tui: two panes drawn with borders", t.wait("zosh 2") and "zosh 1" in t.text(), repr(t.lines()[0]))
    # 上下に割る
    t.send("\x18-")
    check("tui: C-x - splits again", t.wait(" 1:zosh 1+2 "), repr(t.lines()[-1]))
    # ペインを閉じる (今のペイン = 3 つ目)
    t.send("\x18k")
    check("tui: C-x k closes the pane and kills its session", t.wait(" 1:zosh 1+1 ") and wait_until(lambda: query("sessions").split() == ["1", "2"]), query("sessions"))
    # マウスでタブ 0 を選び、打った字がコンソールへ
    t.send("\x1b[<0;3;24M\x1b[<0;3;24m")
    time.sleep(0.3)
    n0 = len(fake.con_in)
    t.send("echo via-mouse\r")
    check("tui: mouse click on a tab label selects it", t.wait("via-mouse") and b"echo via-mouse" in fake.con_in[n0:], repr(t.lines()[-1]))
    # 数字でタブ 1、p / n
    t.send("\x181")
    time.sleep(0.3)
    t.send("\x18p")
    time.sleep(0.3)
    t.send("\x18n")
    time.sleep(0.3)
    # 切り離し → 配置が zmuxd に残る
    t.send("\x18d")
    check("tui: C-x d detaches", t.wait_exit())
    lay = query("layout get")
    check("tui: layout kept in zmuxd", lay == "0;(v 1 2)", lay)
    check("tui: sessions survive the detach", query("sessions").split() == ["1", "2"], query("sessions"))
    # つなぎ直し
    t = Tui()
    check("tui: reattach restores the tabs", t.wait(" 0:COMMAND.X ") and t.wait(" 1:zosh 1+1 "), repr(t.lines()[-1]))
    t.send("\x181")
    check("tui: the zosh panes show their scrollback", t.wait("hello-zosh"), repr(t.lines()[:3]))
    # zosh が exit するとペインが消える
    t.send("exit 3\r")
    check("tui: exit closes the pane", t.wait(" 1:zosh 2 ") or t.wait(" 1:zosh 1 "), repr(t.lines()[-1]))
    check("tui: exit message in the status line", t.wait("が終わった"), repr(t.lines()[-1]))
    t.send("\x18d")
    t.wait_exit()
    # 端末の大きさ: 小さい端末でも動く
    t = Tui(cols=40, rows=10)
    check("tui: 40x10 terminal", t.wait(" 0:COMMAND.X "), repr(t.lines()[-1]))
    t.send("\x18d")
    t.wait_exit()
    print("ZMUX FAKE E2E %s (%d failures) run=%s" % ("PASS" if not fails else "FAIL", len(fails), run))
finally:
    zd.terminate()
    fake.close()
sys.exit(1 if fails else 0)
