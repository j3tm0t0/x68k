#!/usr/bin/env python3
"""zmux: 画面が CON のときの写し (CON の出力 → タブ 0、タブ 0 の文字 → キー入力) と、zmux screen を XEiJ で試す。
  1. HUMAN.SYS / COMMAND.X / RSDRV.SYS / zmux.sys / zmux.x / rx.x の起動フロッピー。AUTOEXEC.BAT は ZMUX -c で、CTTY AUX にしない
  2. XEiJ (Compact、主記憶 2MB) で起動し、zmuxd を --tcp でつなぐ
  3. zmux attach 0 (タブ 0) で ver を打つ → キー入力として届き、CON に出た答えがタブ 0 に返る。XEiJ の画面にも出ている
  4. zmux screen: ほどいたプレーンがテキスト VRAM (エージェントの dump) と 1 バイトも違わない、-t の文字に ver の答えがある
  5. タブ 0 で ctty aux → AUX のコンソールで echo / ver (これまでの動き)、ctty con → また写しで ver
  6. zmux -m off で写しが止まる、-m on で戻る
usage: e2e_con.py RUNDIR   (XEIJ_TOOLS / HUMAN_XDF / XPORT は e2e_rdrive.py と同じ)"""
import os, re, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2econ-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT), XDG_CACHE_HOME=os.path.join(run, "cache"))
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ("  " + extra if extra else ""), flush=True)
    if not cond:
        fails.append(name)


def sh(*a, **k):
    return subprocess.run(a, check=True, **k)


def mkfloppy():
    t = os.path.join(run, "tree")
    sh("rm", "-rf", t)
    os.makedirs(os.path.join(t, "SYS"))
    xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")
    for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
        sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
    sh("cp", os.path.join(zm, "x68k", "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
    sh("cp", os.path.join(zm, "x68k", "zmux.x"), os.path.join(t, "ZMUX.X"))
    sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
    bx = os.path.join(here, "buserr.x")
    if not os.path.exists(bx) or os.path.getmtime(bx) < os.path.getmtime(os.path.join(here, "buserr.c")):
        sh("m68k-xelf-gcc", "-m68000", "-O2", "-o", bx, os.path.join(here, "buserr.c"))
    sh("cp", bx, os.path.join(t, "BUSERR.X"))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(
        b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 8 10 20\r\n"
        b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200\r\nZMUX -c\r\n")
    out = os.path.join(run, "zmuxcon.xdf")
    sh(sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), out, xdf, t, capture_output=True)
    return out


class Attach:
    def __init__(self):
        self.p = subprocess.Popen([ZBIN, "attach", "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.buf = bytearray()
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        while True:
            d = self.p.stdout.read1(4096)
            if not d:
                return
            self.buf += d

    def send(self, s):
        self.p.stdin.write(s.encode("cp932"))
        self.p.stdin.flush()

    def wait(self, pat, t=30):
        pat = pat.encode("cp932")
        end = time.time() + t
        while time.time() < end:
            i = self.buf.find(pat)
            if i >= 0:
                out = bytes(self.buf[:i + len(pat)])
                del self.buf[:i + len(pat)]
                return out.decode("cp932", "replace")
            time.sleep(0.05)
        return None

    def cmd(self, line, t=60, prompt="A>"):
        self.buf.clear()
        self.send(line + "\r")
        return self.wait(prompt, t)

    def close(self):
        self.p.kill()


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


def screen():
    return subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout


def dump(name, addr, ln):
    p = os.path.join(run, name)
    if os.path.exists(p):
        os.remove(p)
    with open(os.path.join(run, "agent.cmd.tmp"), "w") as f:
        f.write("dump %s 0x%x 0x%x\n" % (p, addr, ln))
    os.rename(os.path.join(run, "agent.cmd.tmp"), os.path.join(run, "agent.cmd"))
    for _ in range(200):
        if os.path.exists(p) and os.path.getsize(p) == ln:
            return open(p, "rb").read()
        time.sleep(0.05)
    return None


def zmux_screen(*args):
    t0 = time.time()
    r = subprocess.run([ZBIN, "screen"] + list(args), capture_output=True, env=env, timeout=600)
    return r, time.time() - t0


xdf = mkfloppy()
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)):
        os.remove(os.path.join(run, f))
sh("cp", xdf, os.path.join(run, "fd0.xdf"))
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=dict(env, XEIJ_RSBUF="64"), stdin=subprocess.DEVNULL)
zd = None
try:
    for _ in range(120):
        time.sleep(1)
        s = screen()
        if "zmux: thread started" in s:
            break
    else:
        sys.exit("e2e: zmux did not start:\n" + s)
    check("zmux.x hooked CON", "CON デバイスが見つからない" not in s, s[-200:])
    zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "-v"], env=env,
                          stderr=open(os.path.join(run, "zmuxd.log"), "w"))
    for _ in range(150):
        time.sleep(0.2)
        if status().startswith("line up"):
            break
    check("line up with the console on CON (zmux -c)", status().startswith("line up"), status())
    time.sleep(1)
    a = Attach()
    time.sleep(1)
    # 1 回目は COMMAND.X がまだ CON を読みに来ていないかもしれない (CON を読みに来た後でキー入力になる)。空の行で起こす
    a.send("\r")
    time.sleep(2)
    out = a.cmd("ver")
    check("ver typed in tab 0 runs on CON and the answer comes back to tab 0", out is not None and "Human68k version 3.02" in out, repr(out)[-120:] if out else repr(bytes(a.buf[-200:])))
    s = screen()
    check("... and it is on the XEiJ screen", "Human68k version 3.02" in s, s[-200:])
    word = "".join(chr(97 + (k * 7) % 26) for k in range(120))
    out = a.cmd("echo " + word)
    check("120 chars typed in tab 0 arrive intact (CON)", out is not None and out.count(word) >= 2, repr(out)[-100:] if out else "")
    out = a.cmd("echo ABC^XYZ{}~_?+*=@")
    check("symbols typed in tab 0 (key codes from the table)", out is not None and out.count("ABC^XYZ{}~_?+*=@") >= 2, repr(out)[-80:] if out else "")

    # zmux screen: ほどいた中身とテキスト VRAM の dump を比べる (プレーン 0〜3、768x512 = 96 バイト x 512 ラスタ)
    rawf = os.path.join(run, "scr.raw")
    r, t = zmux_screen("-p", "0123", "-r", rawf, "-o", os.path.join(run, "scr.png"))
    print("e2e: zmux screen: %s (%.1fs)" % (r.stderr.decode(errors="replace").strip().replace("\n", " / "), t))
    got = open(rawf, "rb").read() if os.path.exists(rawf) else b""
    want = b""
    for k in range(4):
        d = dump("tvram%d.bin" % k, 0xE00000 + k * 0x20000, 128 * 512)
        want += b"".join(d[y * 128:y * 128 + 96] for y in range(512)) if d else b""
    # カーソルの点滅 (IOCS がテキスト VRAM の 1 字分を反転する) は、写す間と dump の間で変わりうる。違いが 1 字分 (同じ桁、16 ラスタの中) なら同じとみなす
    diff = [i for i, (x, y) in enumerate(zip(got, want)) if x != y]
    cells = {((i % (96 * 512)) // 96 // 16, i % 96) for i in diff}
    rows = sorted({(i % (96 * 512)) // 96 for i in diff})
    cursor_only = len({c for _, c in cells}) <= 1 and (not rows or rows[-1] - rows[0] < 16)
    check("zmux screen -p 0123: the planes equal the text VRAM dump (4 x 48KB, cursor cell aside)", len(got) == 4 * 96 * 512 and len(want) == len(got) and cursor_only,
          "got %d want %d diff %d bytes in cells %s" % (len(got), len(want), len(diff), sorted(cells)[:6]))
    png = open(os.path.join(run, "scr.png"), "rb").read() if os.path.exists(os.path.join(run, "scr.png")) else b""
    check("PNG written (768x512)", png[:8] == b"\x89PNG\r\n\x1a\n" and png[16:24] == (768).to_bytes(4, "big") + (512).to_bytes(4, "big"))
    r, t = zmux_screen("-t")
    txt = r.stdout.decode("utf-8", "replace")
    print("e2e: zmux screen -t (%.1fs, CGROM の取り込み込み):\n%s" % (t, "\n".join(txt.splitlines()[-8:])))
    check("zmux screen -t has the ver answer", "Human68k version 3.02" in txt, r.stderr.decode(errors="replace")[-200:])
    r, t2 = zmux_screen("-t")
    check("zmux screen -t again uses the cached CGROM and is quicker", "Human68k version 3.02" in r.stdout.decode("utf-8", "replace") and t2 < t,
          "%.1fs -> %.1fs" % (t, t2))
    rawc = os.path.join(run, "scrclip.raw")
    r, _ = zmux_screen("-p", "0", "-y", "1000", "-n", "100", "-r", rawc)
    check("zmux screen clips the region at the bottom of VRAM", r.returncode == 0 and os.path.getsize(rawc) == 96 * 24, r.stderr.decode(errors="replace")[-120:])

    # CTTY AUX にして、これまでの動き (AUX のコンソール)
    a.buf.clear()
    a.send("ctty aux\r")
    out = a.wait("A>", 30)
    check("ctty aux typed in tab 0: the AUX prompt comes back", out is not None, repr(bytes(a.buf[-120:])))
    time.sleep(1)
    out = a.cmd("ver")
    check("ver under CTTY AUX", out is not None and "Human68k version 3.02" in out, repr(out)[-100:] if out else "")
    out = a.cmd("echo " + word)
    check("120 chars under CTTY AUX arrive intact", out is not None and out.count(word) >= 2, repr(out)[-80:] if out else "")
    out = a.cmd("zmux -s")
    m = re.search(r"console \(1=CON[^)]*\) (\d+)", out or "")
    check("zmux -s: console is AUX now (bit0 = 0) and CON is hooked (bit2)", m is not None and int(m.group(1)) & 5 == 4, repr(out)[-300:] if out else "")
    mm = re.search(r"CON bytes mirrored (\d+)", out or "")
    check("zmux -s: CON bytes were mirrored", mm is not None and int(mm.group(1)) > 100, mm.group(0) if mm else "")
    print("e2e: zmux -s:\n" + "\n".join(l for l in (out or "").splitlines() if l.startswith(("CON", "tab 0", "console", "screen", "AUX"))))

    # CON に戻す
    a.buf.clear()
    a.send("ctty con\r")
    time.sleep(3)
    a.send("\r")
    time.sleep(2)
    out = a.cmd("ver")
    check("ctty con again: ver typed in tab 0 comes back through the mirror", out is not None and "Human68k version 3.02" in out, repr(out)[-100:] if out else "")

    # -m off: 写しとキー入力を止める。キー入力も止まるので、-m on は XEiJ のキーボード (Control パイプ) から打つ
    a.cmd("zmux -m off", 10)
    a.buf.clear()
    a.send("ver\r")
    time.sleep(4)
    check("zmux -m off: tab 0 gets nothing", b"Human68k version" not in a.buf, repr(bytes(a.buf[-80:])))
    sh(sys.executable, os.path.join(serial, "test", "xtype.py"), run, "zmux -m on\\n")
    time.sleep(5)
    s = screen()
    check("zmux -m on typed on the X68000 keyboard", "CON mirror on" in s, s[-200:])
    out = a.cmd("ver")
    check("after -m on: ver from tab 0 again", out is not None and "Human68k version 3.02" in out, repr(out)[-100:] if out else "")
    # Human68k のエラーの箱 (中止<A> 再実行<R> 無視<I>) は VRAM に直接描かれ、本体のキーを待つ。空のドライブ B: で出す。
    # zmux screen で箱が読めて、タブ 0 (CON のとき) / zmux key (AUX のとき) の A で中止できる
    def errbox(how, line="dir b:", want="無視"):
        a.buf.clear()
        a.send(line + "\r")
        txt = ""
        t0 = time.time()
        for _ in range(6):
            time.sleep(3)
            r, t = zmux_screen("-t", "-p", "0123", "-o", os.path.join(run, "box-%s.png" % how), "-r", os.path.join(run, "box-%s.raw" % how))
            txt = r.stdout.decode("utf-8", "replace")
            if "中止" in txt:
                break
        box = [l for l in txt.splitlines() if "中止" in l or "ディスク" in l or "エラー" in l]
        check("%s: zmux screen -t shows the Human68k error box while it waits for a key" % how, "中止" in txt and want in txt,
              " / ".join(box)[:200] if box else txt[-300:])
        print("e2e: error box (%s, %.1fs):\n%s" % (how, time.time() - t0, "\n".join(box)))
        return txt

    errbox("CON")
    a.buf.clear()
    a.send("a")
    out = a.wait("A>", 30)
    check("CON: A typed in tab 0 answers the box (中止) and the prompt comes back", out is not None, repr(bytes(a.buf[-120:])))
    out = a.cmd("ctty aux", 30)
    time.sleep(1)
    errbox("AUX")
    a.buf.clear()
    subprocess.run([ZBIN, "key", "a"], env=env, capture_output=True)
    out = a.wait("A>", 30)
    check("AUX: zmux key a answers the box and the AUX prompt comes back", out is not None, repr(bytes(a.buf[-120:])))
    errbox("AUX bus error", "buserr", "バスエラー")
    a.buf.clear()
    subprocess.run([ZBIN, "key", "a"], env=env, capture_output=True)
    out = a.wait("A>", 30)
    check("AUX: zmux key a answers the bus error box", out is not None, repr(bytes(a.buf[-120:])))
    out = a.cmd("ver")
    check("AUX: ver after the error box", out is not None and "Human68k version 3.02" in out, repr(out)[-80:] if out else "")
    print("e2e: " + status())
    a.close()
    print("ZMUX CON E2E %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
finally:
    if zd:
        zd.terminate()
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
sys.exit(1 if fails else 0)
