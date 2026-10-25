#!/usr/bin/env python3
"""zmux (tmux のような画面) を XEiJ で試す。zmux を pty で動かし、画面を pyte で読む。
  1. e2e.py と同じ起動フロッピー (ZOS_X / ZOSH_X があれば e2e_zss.py と同じく npd.sys / npd.x / zos.x も入れ、主記憶 12MB)
  2. タブ 0 (COMMAND.X の CTTY AUX): 状態の行、ver、Ctrl+X ? の説明、Ctrl+X d で切り離してつなぎ直す
  3. ZOS_X があれば: タブ 0 で zos を起動 → Ctrl+X c で zosh のタブ、echo、Ctrl+X | で割る、Ctrl+X k、切り離し → 配置が残る
usage: [ZOS_X=... ZOSH_X=...] e2e_tui.py RUNDIR"""
import os, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from tuipty import Tui as _Tui, wait_until  # noqa: E402

zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
ZOS_X = os.environ.get("ZOS_X")
ZOSH_X = os.environ.get("ZOSH_X")
zos = bool(ZOS_X and ZOSH_X and os.path.exists(ZOS_X) and os.path.exists(ZOSH_X))
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()
home = os.path.join(run, "home")
os.makedirs(home, exist_ok=True)
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT), HOME=home, TERM="xterm-256color", LANG="C.UTF-8")
if zos:
    env["XMEM"] = "12"
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ("  " + extra if extra else ""), flush=True)
    if not cond:
        fails.append(name)


def sh(*a, **k):
    return subprocess.run(a, check=True, **k)


def Tui(cols=80, rows=24):
    return _Tui(ZBIN, env, cols, rows)


def mkfloppy():
    t = os.path.join(run, "tree")
    sh("rm", "-rf", t)
    os.makedirs(os.path.join(t, "SYS"))
    os.makedirs(os.path.join(t, "TMP"))
    xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")
    for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
        sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
    sh("cp", os.path.join(zm, "x68k", "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
    sh("cp", os.path.join(zm, "x68k", "zmux.x"), os.path.join(t, "ZMUX.X"))
    sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
    cfg = b"FILES     = 40\r\nBUFFERS   = 20 1024\r\nPROCESS   = 16 10 20\r\nDEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\n"
    auto = b"ECHO OFF\r\nPATH A:\\\r\nSET HOME=A:/\r\nSET temp=A:\\TMP\r\nRX -B19200\r\nZMUX\r\n"
    if zos:
        sh("cp", os.path.join(top, "net", "npd", "npd.sys"), os.path.join(t, "SYS", "NPD.SYS"))
        sh("cp", os.path.join(top, "net", "npd", "npd.x"), os.path.join(t, "NPD.X"))
        sh("cp", ZOS_X, os.path.join(t, "ZOS.X"))
        sh("cp", ZOSH_X, os.path.join(t, "ZOSH.X"))
        cfg += b"DEVICE    = \\SYS\\NPD.SYS\r\n"
        auto += b"NPD -m 192.168.68.2\r\n"
    auto += b"CTTY AUX\r\n"
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(cfg)
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(auto)
    out = os.path.join(run, "zmuxtui.xdf")
    free = subprocess.run([sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), out, xdf, t],
                          capture_output=True, text=True, check=True).stdout.strip()
    print("e2e: floppy %s (%s KB free)" % (out, free))
    return out


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


def screen():
    return subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout


xdf = mkfloppy()
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)):
        os.remove(os.path.join(run, f))
sh("cp", xdf, os.path.join(run, "fd0.xdf"))
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=dict(env, XEIJ_RSBUF="64"), stdin=subprocess.DEVNULL)
zd = None
try:
    for _ in range(150):
        time.sleep(1)
        if "zmux: thread started" in screen():
            break
    else:
        sys.exit("e2e: zmux did not start:\n" + screen())
    zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "-v"], env=env, stderr=open(os.path.join(run, "zmuxd.log"), "w"))
    for _ in range(100):
        time.sleep(0.2)
        if status().startswith("line up"):
            break
    check("line up", status().startswith("line up"), status())
    t = Tui()
    check("tui: tab 0 in the status line", t.wait(" 0:COMMAND.X "), repr(t.lines()[-1]))
    t.send("\r")
    check("tui: COMMAND.X prompt", t.wait("A>"), repr(t.lines()[:3]))
    seen = t.text().count("Human68k version")
    t.send("ver\r")
    check("tui: ver on tab 0", wait_until(lambda: t.text().count("Human68k version") > seen, 20), repr(t.lines()[:5]))
    t.send("\x18?")
    check("tui: C-x ? help", t.wait("前置キー"))
    t.send("q")
    check("tui: help closes", t.wait_gone("前置キー"))
    t.send("\x18d")
    check("tui: C-x d detaches", t.wait_exit())
    t = Tui()
    check("tui: reattach shows the console again", t.wait(" 0:COMMAND.X ") and t.wait("Human68k version"), repr(t.lines()[:3]))
    if zos:
        t.send("zos\r")                 # 本体の画面で Z-OS。タブ 0 は zos.x が終わるまで待ちになる (README の注意)
        check("zos.x sends 0xA7", wait_until(lambda: "zos yes" in status(), 60), status())
        t.send("\x18c")
        check("tui: C-x c opens a zosh tab", t.wait(" 1:zosh 1 ") and t.wait("%", 40), repr(t.lines()[-1]))
        t.send("echo hello-tui\r")
        check("tui: echo in the zosh pane", wait_until(lambda: t.text().count("hello-tui") >= 2, 30), repr(t.lines()[:4]))
        t.send("\x18|")
        check("tui: C-x | splits with a second zosh", t.wait(" 1:zosh 1+1 ", 30), repr(t.lines()[-1]))
        t.send("\x18k")
        check("tui: C-x k closes the pane", t.wait(" 1:zosh 1 ", 30) and wait_until(lambda: "sessions [1:" in status() and "2:" not in status(), 30), status())
        t.send("\x18d")
        check("tui: detach with sessions", t.wait_exit())
        t = Tui()
        check("tui: reattach restores the zosh tab", t.wait(" 1:zosh 1 "), repr(t.lines()[-1]))
        t.send("\x181")
        time.sleep(0.5)
        t.send("exit 3\r")
        check("tui: exit closes the pane", t.wait_gone(" 1:zosh 1 ", 30) and t.wait("が終わった"), repr(t.lines()[-1]))
    else:
        print("e2e: ZOS_X / ZOSH_X not set: zosh tabs skipped")
    t.send("\x18d")
    t.wait_exit()
    print("ZMUX TUI E2E %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
finally:
    if zd:
        zd.terminate()
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
sys.exit(1 if fails else 0)
