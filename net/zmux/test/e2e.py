#!/usr/bin/env python3
"""zmux の段階 1 (線 + コンソール) を XEiJ で試す。
  1. HUMAN.SYS / COMMAND.X / RSDRV.SYS / zmux.sys / zmux.x / rx.x だけの起動フロッピーを作る
     (AUTOEXEC.BAT: RX -B19200 → ZMUX → CTTY AUX)
  2. XEiJ (Compact、主記憶 2MB、RSDRV の受信バッファは実機と同じ 64 バイト) で起動
  3. zmuxd が無い間: 素の文字で COMMAND.X が答える (sx.py probe)
  4. zmuxd (zmux daemon --tcp) を起動: X68000 の HELLO で UP になり、`zmux attach 0` で ver / dir が通る
  5. バイトを壊す中継を挟んで、大きな出力 (dir を繰り返す) が欠けずに届く
  6. zmuxd を止める: 10 秒で X68000 が素の文字に戻り、sx.py probe が答える
usage: e2e.py RUNDIR"""
import os, re, socket, subprocess, sys, threading, time, random

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
RELAY = XPORT + 1                        # 壊す中継 (zmuxd はここにつなぐ)
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()   # unix ソケットの名前は短くないといけない
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT))


def sh(*a, **k):
    return subprocess.run(a, check=True, **k)


def mkfloppy():
    t = os.path.join(run, "tree")
    sh("rm", "-rf", t)
    for d in ("SYS",):
        os.makedirs(os.path.join(t, d))
    xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")
    for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
        sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
    sh("cp", os.path.join(zm, "x68k", "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
    sh("cp", os.path.join(zm, "x68k", "zmux.x"), os.path.join(t, "ZMUX.X"))
    sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
    ib = os.path.join(zm, "x68k", "idlebench.x")
    if os.path.exists(ib):
        sh("cp", ib, os.path.join(t, "IDLEBENCH.X"))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(
        b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 8 10 20\r\n"
        b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(
        b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200\r\nZMUX\r\nCTTY AUX\r\n")
    out = os.path.join(run, "zmux.xdf")
    free = subprocess.run([sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), out, xdf, t],
                          capture_output=True, text=True, check=True).stdout.strip()
    print("e2e: floppy %s (%s KB free)" % (out, free))
    return out


def relay(rate):
    """XEiJ (XPORT) と zmuxd (RELAY) の間で、rate の割合でバイトを壊す"""
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", RELAY))
    ls.listen(1)
    st = {"bad": 0, "on": False}

    def serve():
        while True:
            c, _ = ls.accept()
            x = socket.create_connection(("127.0.0.1", XPORT))
            def pump(a, b):
                while True:
                    try:
                        d = a.recv(4096)
                    except OSError:
                        return
                    if not d:
                        b.close()
                        return
                    if st["on"]:
                        d = bytearray(d)
                        for i in range(len(d)):
                            if random.random() < rate:
                                d[i] ^= 0x5A
                                st["bad"] += 1
                    try:
                        b.sendall(bytes(d))
                    except OSError:
                        return
            threading.Thread(target=pump, args=(c, x), daemon=True).start()
            threading.Thread(target=pump, args=(x, c), daemon=True).start()
    threading.Thread(target=serve, daemon=True).start()
    return st


class Attach:
    """zmux attach 0 を子プロセスで動かし、出力を溜める"""
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
        self.p.stdin.write(s.encode())
        self.p.stdin.flush()

    def wait(self, pat, t):
        end = time.time() + t
        while time.time() < end:
            i = self.buf.find(pat.encode())
            if i >= 0:
                out = bytes(self.buf[:i + len(pat)])
                del self.buf[:i + len(pat)]
                return out
            time.sleep(0.05)
        sys.exit("e2e: no %r in zmux attach output (%r)" % (pat, bytes(self.buf[-300:])))

    def close(self):
        self.p.kill()


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


def sx_probe():
    return subprocess.run([sys.executable, os.path.join(serial, "sx.py"), "--tcp", "127.0.0.1:%d" % XPORT, "--baud", "19200", "probe"],
                          capture_output=True, text=True).returncode == 0


xdf = mkfloppy()
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)):
        os.remove(os.path.join(run, f))
sh("cp", xdf, os.path.join(run, "fd0.xdf"))
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=dict(env, XEIJ_RSBUF="64"), stdin=subprocess.DEVNULL)
zd = None
try:
    t0 = time.time()
    for _ in range(120):
        time.sleep(1)
        s = subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout
        if "zmux: thread started" in s:
            break
    else:
        sys.exit("e2e: zmux did not start:\n" + s)
    print("e2e: boot to zmux %.1fs" % (time.time() - t0))
    time.sleep(2)
    if not sx_probe():
        sys.exit("e2e: no prompt on ZCON in raw mode (zmuxd not running)")
    print("e2e: raw mode: COMMAND.X answers on ZCON without zmuxd")
    st = relay(float(os.environ.get("CORRUPT", "0.0003")))   # 1 バイトあたりの割合 (1KB のフレームの 3 割弱が壊れる)
    zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % RELAY, "-v"], env=env)
    for _ in range(100):
        time.sleep(0.2)
        if status().startswith("line up"):
            break
    else:
        sys.exit("e2e: line did not come up: " + status())
    t_up = time.time()
    print("e2e: line up (" + status() + ")")
    a = Attach()
    time.sleep(0.5)
    a.send("\r")
    a.wait("A>", 10)
    a.send("ver\r")
    out = a.wait("A>", 10)
    if b"Human68k version 3.02" not in out:
        sys.exit("e2e: ver: %r" % out)
    print("e2e: ver over zmux ok")
    # 線が UP で何も流れていない間に、常駐スレッドがどれだけ CPU を食うか (MHz 換算。小さいほど食っている)
    if os.path.exists(os.path.join(zm, "x68k", "idlebench.x")):
        a.send("zmux -s\r")
        p0 = a.wait("A>", 30)
        a.send("idlebench\r")
        out = a.wait("A>", 30)
        a.send("zmux -s\r")
        p1 = a.wait("A>", 30)
        def polls(b):
            m = re.search(rb"polls (\d+)", b)
            return int(m.group(1)) if m else -1
        def tcalls(b):
            m = re.search(rb"thread calls (\d+)", b)
            return int(m.group(1)) if m else -1
        print("e2e: polls during idlebench (3s): %d, zmux.x thread wake-ups: %d" % (polls(p1) - polls(p0), tcalls(p1) - tcalls(p0)))
        print("e2e: " + out.decode("ascii", "replace").strip().splitlines()[-2].strip() if b"IDLEBENCH" in out else "e2e: idlebench: %r" % out)
    # 壊す中継を有効にして、大きな出力を受ける
    st["on"] = True
    t1 = time.time()
    a.send("dir a:\\sys\r")
    a.wait("A>", 30)
    a.send("type a:\\config.sys\r")
    out = a.wait("A>", 30)
    if b"ZMUX.SYS" not in out:
        sys.exit("e2e: type config.sys over a lossy line: %r" % out)
    for k in range(5):
        a.send("dir\r")
        out = a.wait("A>", 60)
        if b"AUTOEXEC" not in out or b"COMMAND" not in out:
            sys.exit("e2e: dir #%d over a lossy line: %r" % (k, out))
    st["on"] = False
    print("e2e: corruption: %d bytes corrupted, output intact (%.1fs). %s" % (st["bad"], time.time() - t1, status()))
    a.close()
    zd.terminate()
    zd.wait()
    zd = None
    t2 = time.time()
    for _ in range(40):
        time.sleep(1)
        if sx_probe():
            break
    else:
        sys.exit("e2e: X68000 did not return to raw mode")
    print("e2e: zmuxd stopped -> raw mode again in %.1fs" % (time.time() - t2))
    print("ZMUX E2E PASS")
finally:
    if zd:
        zd.terminate()
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
