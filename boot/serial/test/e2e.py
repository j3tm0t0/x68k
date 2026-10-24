#!/usr/bin/env python3
"""XEiJ で boot/serial を端から端まで試す。
  1. システムフロッピー (HUMAN302.XDF のコピー) だけで Compact / 主記憶 2MB の XEiJ を起動
  2. 段階 1 のキー (stage1.keys) を XEiJ のキーボードから打つ (人が打つのと同じ並び)
  3. sx.py bootstrap (段階 1〜3)
  4. XEiJ をリセットし、AUTOEXEC.BAT の CTTY AUX でシリアルから操作できることを確かめる
     (19200bps の A> に RX を打って rx.x を起動し、run / put / get)
usage: e2e.py RUNDIR"""
import os, random, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
top = os.path.dirname(here)
run = os.path.abspath(sys.argv[1])
port = os.environ.get("XPORT", "54368")
tcp = "127.0.0.1:" + port
env = dict(os.environ, XPORT=port)


def sh(*a, **k):
    return subprocess.run(a, check=True, **k)


def screen():
    return subprocess.run([sys.executable, os.path.join(here, "xscreen.py"), run], capture_output=True,
                          text=True).stdout


link = ["--tcp", tcp]


def sx(*a, baud="9600", check=True):
    cmd = [sys.executable, os.path.join(top, "sx.py")] + link + ["--baud", baud, "--log",
           os.path.join(run, "sx.log")] + list(a)
    print("+ sx.py", " ".join(a), flush=True)
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    sys.stdout.write(p.stderr)
    if check and p.returncode != 0:
        sys.exit("e2e: sx.py %s failed (%d)" % (a[0], p.returncode))
    return p, time.time() - t


if os.path.exists(os.path.join(run, "fd0.xdf")):
    os.remove(os.path.join(run, "fd0.xdf"))
os.makedirs(run, exist_ok=True)
bridge = None
xe = subprocess.Popen([os.path.join(here, "xeij-fd.sh"), run], env=env, stdin=subprocess.DEVNULL)
try:
    t_boot = time.time()
    for _ in range(120):
        time.sleep(1)
        if os.path.exists(os.path.join(run, "XEiJControl")):
            s = screen()
            if s.rstrip().endswith("A>"):
                break
    else:
        sys.exit("e2e: no A> on the screen")
    t_fdboot = time.time() - t_boot
    print("e2e: floppy boot to A> in %.1fs" % t_fdboot)
    if os.environ.get("E2E_PTY") == "1":
        # sx.py をシリアルポート (termios) の経路で動かす: pty <-> XEiJ の TCP
        lf = os.path.join(run, "pty.name")
        bridge = subprocess.Popen([sys.executable, os.path.join(here, "ptybridge.py"), tcp, lf])
        for _ in range(50):
            if os.path.exists(lf) and open(lf).read():
                break
            time.sleep(0.2)
        link[:] = ["--port", open(lf).read()]
        print("e2e: using pty", link[1])
    keys = open(os.path.join(top, "stage1.keys"), encoding="ascii").read()
    t0 = time.time()
    sh(sys.executable, os.path.join(here, "xtype.py"), run, "-f", os.path.join(top, "stage1.keys"))
    print("e2e: stage 1 typed: %d keys" % len(keys))
    sx("bootstrap")
    t_all = time.time() - t0
    print("e2e: bootstrap (typing + stage 1-3) %.1fs" % t_all)

    # リセットして AUTOEXEC.BAT の CTTY AUX を確かめる
    t1 = time.time()
    with open(os.path.join(run, "XEiJControl"), "w") as f:
        f.write("reset\n")
    time.sleep(5)
    for i in range(60):
        p, _ = sx("probe", baud="19200", check=False)
        if p.returncode == 0:
            break
        time.sleep(1)
    else:
        sys.exit("e2e: no A> on RS-232C at 19200 after reset")
    print("e2e: reset to A> on RS-232C in %.1fs" % (time.time() - t1))
    p, _ = sx("run", "ver", baud="19200")
    p, _ = sx("run", "dir", baud="19200")
    if "RX" not in p.stdout or "LHA" not in p.stdout:
        sys.exit("e2e: dir does not show RX.X / LHA.X")
    p, _ = sx("run", "lha a A:\\T.LZH A:\\RX.BAS", baud="19200", check=False)
    p, _ = sx("run", "lha l A:\\T.LZH", baud="19200", check=False)
    if "1 files" not in p.stdout or ".BAS" not in p.stdout.upper():
        sys.exit("e2e: lha.x did not list RX.BAS")
    random.seed(68000)
    blob = bytes(random.getrandbits(8) for _ in range(30000))
    bp = os.path.join(run, "blob.bin")
    open(bp, "wb").write(blob)
    p, t_put = sx("put", bp, "BLOB.BIN", baud="19200")
    p, _ = sx("get", "BLOB.BIN", bp + ".back", baud="19200")
    if open(bp + ".back", "rb").read() != blob:
        sys.exit("e2e: BLOB.BIN round trip differs")
    print("e2e: 30000 bytes put/get round trip ok")
    p, _ = sx("quit", baud="19200")
    p, _ = sx("probe", baud="19200")
    print("e2e: total from first key to the end of the checks %.1fs (+ floppy boot %.1fs)" %
          (time.time() - t0, t_fdboot))
    print("E2E PASS")
finally:
    if bridge:
        bridge.terminate()
    if os.environ.get("KEEP") != "1":
        xe.terminate()
        subprocess.run(["pkill", "-f", run + "/agent.cmd"])
