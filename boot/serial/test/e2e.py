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
SSHHOST = os.environ.get("E2E_SLIP_HOST", "raspi3")   # slattach のある Pi (疑似端末に sl1 = 192.168.69.1 を作る)
XIP, PEER = "192.168.69.2", "192.168.69.1"
slipbr = None


def reset():
    with open(os.path.join(run, "XEiJControl"), "w") as f:
        f.write("reset\n")


def remote(cmd, check=True, timeout=120):
    p = subprocess.run(["ssh", SSHHOST, cmd], capture_output=True, timeout=timeout)
    out = p.stdout.decode("utf-8", "replace")
    if check and p.returncode != 0:
        sys.exit("e2e: %s on %s failed (%d): %s %s" % (cmd, SSHHOST, p.returncode, out, p.stderr.decode()))
    return p.returncode, out


def slip_up(speed):
    """リセットして AUTOEXEC.BAT の NETUP.BAT で SLIP と rcmd.x が上がるのを待ち、Pi から x68 で確かめる"""
    global slipbr
    reset()
    t = time.time()
    time.sleep(3)
    slipbr = subprocess.Popen([sys.executable, os.path.join(top, "..", "..", "harness", "xeij", "xeijslip.py"),
                               "127.0.0.1", port, SSHHOST, os.path.join(run, "slip.log")])
    for i in range(90):
        rc, out = remote("X68_HOST=%s timeout 20 ~/bin/x68 ver" % XIP, check=False, timeout=60)
        if rc == 0 and "Human68k" in out:
            break
        time.sleep(2)
    else:
        sys.exit("e2e: rcmd.x over SLIP did not answer")
    print("e2e: reset to rcmd over SLIP (%d bps) in %.1fs" % (speed, time.time() - t))
    rc, out = remote("X68_HOST=%s ~/bin/x68 dir" % XIP)
    print(out)
    subprocess.run(["scp", "-q", os.path.join(run, "blob.bin"), SSHHOST + ":/tmp/e2e_blob.bin"], check=True)
    t = time.time()
    rc, out = remote("X68PUT_RAW=1 X68_HOST=%s ~/bin/x68put /tmp/e2e_blob.bin 'A:\\BLOB2.BIN'" % XIP, timeout=300)
    el = time.time() - t
    print(out.strip())
    rc, out = remote("X68_HOST=%s ~/bin/x68 'dir A:\\BLOB2.BIN'" % XIP)
    if "30000" not in out:
        sys.exit("e2e: BLOB2.BIN is not 30000 bytes: " + out)
    print("e2e: x68put 30000 bytes over SLIP %d bps: %.1fs (%.2f KB/s, including the connection)" % (speed, el, 30000 / 1024.0 / el))


def slip_down():
    global slipbr
    if slipbr:
        slipbr.terminate()
        slipbr.wait()
        slipbr = None
        time.sleep(2)


def slip_test():
    p, _ = sx("stage4", "--ip", XIP, "--peer", PEER, "--prune", "--verify", baud="19200")
    slip_up(19200)
    # 38400: コンソールに戻して速さを変える
    remote("X68_HOST=%s ~/bin/x68 'copy A:\\AUTOEXEC.CON A:\\AUTOEXEC.BAT'" % XIP)
    slip_down()
    reset()
    time.sleep(5)
    for i in range(60):
        p, _ = sx("probe", baud="19200", check=False)
        if p.returncode == 0:
            break
        time.sleep(1)
    else:
        sys.exit("e2e: console mode did not come back")
    sx("mode", "slip", "--ip", XIP, "--peer", PEER, "--netspeed", "38400", baud="19200")
    sx("quit", baud="19200")
    slip_up(38400)
    slip_down()


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
    if os.environ.get("XEIJ_RSBUF", "64") == "64":
        if "rsbuf64: RS-232C receive buffer -> 64 bytes" not in s:
            sys.exit("e2e: rsbuf64.x did not shrink the RSDRV receive buffer")
        print("e2e: RSDRV receive buffer is 64 bytes (as on the real machine)")
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
    p, _ = sx("stats", baud="19200")
    p, _ = sx("quit", baud="19200")
    p, _ = sx("probe", baud="19200")
    print("e2e: total from first key to the end of the checks %.1fs (+ floppy boot %.1fs)" %
          (time.time() - t0, t_fdboot))
    if os.environ.get("E2E_SLIP") == "1":
        slip_test()
    print("E2E PASS")
finally:
    if bridge:
        bridge.terminate()
    if slipbr:
        slipbr.terminate()
    if os.environ.get("KEEP") != "1":
        xe.terminate()
        subprocess.run(["pkill", "-f", run + "/agent.cmd"])
