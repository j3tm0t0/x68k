#!/usr/bin/env python3
"""zmux: 画面が CON のまま zmuxd を起動し直しても、線が UP に戻るか (実機 XVI で戻らなかった件) を XEiJ で試す。
  起動フロッピーは XVI と同じく AUTOEXEC.BAT が ZMUX と CTTY AUX (zmux -c は使わない)。
  1. タブ 0 で ctty con → Enter → ver が CON の写しで返る
  2. zmuxd を止める。その間、本体のキーボードで打った ver が画面に出る (写す先が詰まっても CON が止まらない)
  3. すぐ zmuxd を起動し直す (X68000 はまだ UP のつもり) → UP に戻り、タブ 0 から ver
  4. zmuxd を止めて 70 秒待つ (X68000 も素の文字に戻る) → 起動し直す → UP に戻り、タブ 0 から ver
usage: e2e_conre.py RUNDIR   (ZMUX_X68K=DIR で zmux.sys / zmux.x の場所を替えられる。XPORT などは e2e_con.py と同じ)"""
import os, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
XBIN = os.environ.get("ZMUX_X68K", os.path.join(zm, "x68k"))
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2econre-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT))
fails = []
AUX = os.environ.get("MODE") == "aux"        # ctty con にしない (CTTY AUX のまま同じことを試す)
DELAY = float(os.environ.get("DELAY", "10"))  # zmuxd を止めてから起動し直すまでの秒


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
    sh("cp", os.path.join(XBIN, "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
    sh("cp", os.path.join(XBIN, "zmux.x"), os.path.join(t, "ZMUX.X"))
    sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(
        b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 8 10 20\r\n"
        b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200\r\nZMUX\r\nCTTY AUX\r\n")
    out = os.path.join(run, "zmuxconre.xdf")
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
        try:
            self.p.stdin.write(s.encode("cp932"))
            self.p.stdin.flush()
        except BrokenPipeError:
            pass

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


def start_zd(n):
    return subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "-v"], env=env,
                            stderr=open(os.path.join(run, "zmuxd%d.log" % n), "w"))


def wait_up(t):
    end = time.time() + t
    while time.time() < end:
        if status().startswith("line up"):
            return True
        time.sleep(0.5)
    return False


def ver_from_tab0(how):
    a = Attach()
    time.sleep(1)
    a.send("\r")
    time.sleep(2)
    out = a.cmd("ver")
    check("%s: ver typed in tab 0 comes back (%s)" % (how, "AUX" if AUX else "CON mirror"), out is not None and "Human68k version 3.02" in out,
          repr(out)[-100:] if out else repr(bytes(a.buf[-120:])))
    return a


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
    zd = start_zd(1)
    check("line up (CTTY AUX)", wait_up(30), status())
    a = Attach()
    time.sleep(1)
    if not AUX:
        a.buf.clear()
        a.send("ctty con\r")
        time.sleep(3)
    a.close()
    a = ver_from_tab0("first zmuxd")
    for k in range(10):          # CON の出力を写す先に溜めておく (zmuxd を止めたときに詰まる場面)
        a.cmd("echo %d %s" % (k, "x" * 60), 20)
    a.close()

    # 2, 3: zmuxd を止めて、すぐ起動し直す (X68000 は UP のつもり)
    zd.terminate(); zd.wait()
    t0 = time.time()
    if not AUX:
        time.sleep(2)
        sh(sys.executable, os.path.join(serial, "test", "xtype.py"), run, "ver\\n")
        sh(sys.executable, os.path.join(serial, "test", "xtype.py"), run, "echo still-alive\\n")
        t1 = time.time() - t0
        time.sleep(3)
        s = screen()
        check("zmuxd stopped: keys typed on the X68000 keyboard still echo and run (CON is not stuck)", "still-alive" in s.split("echo still-alive")[-1] if "echo still-alive" in s else False,
              "%.1fs to type / %s" % (t1, s[-160:].replace("\n", " | ")))
    time.sleep(max(0, DELAY - (time.time() - t0)))
    print("e2e: restarting zmuxd %.1fs after stopping it" % (time.time() - t0))
    zd = start_zd(2)
    t0 = time.time()
    ok = wait_up(30)
    check("zmuxd restarted after %gs (X68000 still up): line up again" % DELAY, ok, "%.1fs %s" % (time.time() - t0, status()))
    if ok:
        ver_from_tab0("restarted zmuxd").close()

    # 4: 止めて 70 秒 (X68000 も素の文字に戻る) → 起動し直す
    zd.terminate(); zd.wait()
    time.sleep(70)
    zd = start_zd(3)
    t0 = time.time()
    ok = wait_up(40)
    check("zmuxd restarted after the X68000 went raw (60s): line up again", ok, "%.1fs %s" % (time.time() - t0, status()))
    if ok:
        ver_from_tab0("restarted after raw").close()
    raw = [l for n in (1, 2, 3) for l in open(os.path.join(run, "zmuxd%d.log" % n), errors="replace") if "raw " in l]
    print("e2e: raw lines in zmuxd logs: %d%s" % (len(raw), ("\n  " + "\n  ".join(r.strip()[:200] for r in raw[:6])) if raw else ""))
    print("e2e: " + status())
    print("ZMUX CONRE E2E %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
finally:
    if zd:
        zd.terminate()
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
sys.exit(1 if fails else 0)
