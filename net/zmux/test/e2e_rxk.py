#!/usr/bin/env python3
"""rx.x -K の「keyboard kept」の判定を XEiJ で試す。
  1. 誰も触らない: RX -K3 が 3 秒待って終わり、「keyboard kept」が出ない (AUTOEXEC の次の行 DONE1 まで進む)
  2. 待っている間にキーを打つ: 「keyboard kept」が出る (DONE2 には進まない)
usage: e2e_rxk.py RUNDIR"""
import os, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
base = os.path.abspath(sys.argv[1])
XPORT = os.environ.get("XPORT", "54368")
env = dict(os.environ, XPORT=XPORT)
xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")


def sh(*a, **k): return subprocess.run(a, check=True, **k)


def case(name, press):
    run = os.path.join(base, name)
    os.makedirs(run, exist_ok=True)
    t = os.path.join(run, "tree"); sh("rm", "-rf", t); os.makedirs(os.path.join(t, "SYS"))
    for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
        sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
    sh("cp", os.environ.get("RX_X", os.path.join(serial, "rx.x")), os.path.join(t, "RX.X"))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(b"FILES = 30\r\nBUFFERS = 20 1024\r\nDEVICE = \\SYS\\RSDRV.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200 -K4\r\nECHO DONE-AFTER-RX\r\n")
    for f in ("fd0.xdf", "cgrom.bin"):
        if os.path.exists(os.path.join(run, f)): os.remove(os.path.join(run, f))
    sh(sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), os.path.join(run, "fd0.xdf"), xdf, t, capture_output=True)
    xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=env, stdin=subprocess.DEVNULL)
    try:
        s = ""
        for _ in range(60):
            time.sleep(1)
            s = subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout
            if "press a key within" in s:
                break
        else:
            sys.exit("rxk: rx did not start:\n" + s)
        if press:
            time.sleep(float(os.environ.get("PRESS_AT", "0.3")))
            sh(sys.executable, os.path.join(serial, "test", "xtype.py"), run, "a")
        time.sleep(7)
        s = subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout
        return s
    finally:
        xe.terminate()
        subprocess.run(["pkill", "-f", "XEiJ.jar.*" + name], capture_output=True)
        time.sleep(2)


s1 = case("idle", False)
k1 = "keyboard kept" in s1
d1 = "DONE-AFTER-RX" in s1
print("rxk: untouched: kept=%s, continued=%s" % (k1, d1))
s2 = case("press", True)
k2 = "keyboard kept" in s2
print("rxk: key pressed: kept=%s" % k2)
if k1 or not d1 or not k2:
    sys.exit("RXK E2E FAIL\n" + s1 + "\n---\n" + s2)
print("RXK E2E PASS")
