#!/usr/bin/env python3
"""zmux のキー入力の注入 (チャネル 1) を XEiJ で試す。
  タブ 0 (zmux attach 0) で CTTY CON にして COMMAND.X を本体のキーボードに戻し、Pi から `zmux key 'ver\\r'` を注入して
  本体の画面 (テキスト VRAM) に ver の結果が出るか、`zmux key -n XF1` で落ちないか、`zmux key 'ctty aux\\r'` でタブ 0 に戻るかを見る
usage: e2e_key.py RUNDIR"""
import os, subprocess, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
sys.argv = [sys.argv[0], sys.argv[1]]
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.path.join(zm, "target", "release", "zmux")
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT))
os.makedirs(run, exist_ok=True)

def sh(*a, **k): return subprocess.run(a, check=True, **k)
t = os.path.join(run, "tree"); sh("rm", "-rf", t); os.makedirs(os.path.join(t, "SYS"))
xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")
for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
    sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
sh("cp", os.path.join(zm, "x68k", "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
sh("cp", os.path.join(zm, "x68k", "zmux.x"), os.path.join(t, "ZMUX.X"))
sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
open(os.path.join(t, "CONFIG.SYS"), "wb").write(b"FILES = 30\r\nBUFFERS = 20 1024\r\nPROCESS = 8 10 20\r\nDEVICE = \\SYS\\RSDRV.SYS\r\nDEVICE = \\SYS\\ZMUX.SYS\r\n")
open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200\r\nZMUX\r\nCTTY AUX\r\n")
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)): os.remove(os.path.join(run, f))
subprocess.run([sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), os.path.join(run, "fd0.xdf"), xdf, t], check=True, capture_output=True)
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=env, stdin=subprocess.DEVNULL)
zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT], env=env)
def zmux(*a): return subprocess.run([ZBIN] + list(a), capture_output=True, text=True, env=env).stdout.strip()
def screen(): return subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout
try:
    for _ in range(120):
        time.sleep(1)
        if zmux("status").startswith("line up"): break
    else: sys.exit("e2e: line did not come up")
    con = subprocess.Popen([ZBIN, "attach", "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
    buf = bytearray()
    threading.Thread(target=lambda: [buf.extend(d) for d in iter(lambda: con.stdout.read1(4096), b"")], daemon=True).start()
    time.sleep(1)
    con.stdin.write(b"ctty con\r"); con.stdin.flush()
    time.sleep(3)
    print("e2e: tab 0 -> ctty con; " + zmux("key", "ver\\r"))
    time.sleep(3)
    s = screen()
    ok1 = "A>ver" in s.replace(" ", "") or "Human68k version 3.02" in s
    print("e2e: injected 'ver' on the X68000 screen: %s" % ("ok" if ok1 else "NG\n" + s))
    print("e2e: " + zmux("key", "-n", "XF1", "HELP", "OPT1"))
    time.sleep(1)
    del buf[:]
    print("e2e: " + zmux("key", "ctty aux\\r"))
    time.sleep(3)
    con.stdin.write(b"\r"); con.stdin.flush()
    time.sleep(2)
    ok2 = b"A>" in buf
    print("e2e: back to tab 0 after injected 'ctty aux': %s" % ("ok" if ok2 else "NG %r" % bytes(buf)))
    print("ZMUX KEY E2E %s" % ("PASS" if ok1 and ok2 else "FAIL"))
finally:
    zd.terminate(); xe.terminate(); subprocess.run(["pkill", "-f", run + "/agent.cmd"])
