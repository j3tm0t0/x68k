#!/usr/bin/env python3
"""zmux の段階 3 (Z-OS の見えないペインのセッション、0xA7) を XEiJ で試す。
  X68000 (主記憶 12MB): zmux.sys + npd.sys + zmux.x + npd.x -m、コンソールは CTTY AUX。タブ 0 (zmux attach 0) で zos.x を起動する
  Pi (Mac): zmuxd。`zmux attach` (番号なし) で新しい zosh のセッションを開き、echo と exit の終了コードを確かめる。2 つ同時にも開く
usage: ZOS_X=... ZOSH_X=... e2e_zss.py RUNDIR"""
import os, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
ZOS_X = os.environ.get("ZOS_X", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68k-zos-jobs/kernel/zos.x")
ZOSH_X = os.environ.get("ZOSH_X", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68k-zos-jobs/shell/src/zosh.x")
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT), XMEM="12")


def sh(*a, **k):
    return subprocess.run(a, check=True, **k)


def mkfloppy():
    t = os.path.join(run, "tree")
    sh("rm", "-rf", t)
    os.makedirs(os.path.join(t, "SYS"))
    os.makedirs(os.path.join(t, "TMP"))
    xdf = os.environ.get("HUMAN_XDF", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij/HUMAN302.XDF")
    for src, dst in (("/HUMAN.SYS", "HUMAN.SYS"), ("/COMMAND.X", "COMMAND.X"), ("/SYS/RSDRV.SYS", "SYS/RSDRV.SYS")):
        sh(sys.executable, os.path.join(serial, "floppy", "xdfget.py"), xdf, src, os.path.join(t, dst))
    sh("cp", os.path.join(zm, "x68k", "zmux.sys"), os.path.join(t, "SYS", "ZMUX.SYS"))
    sh("cp", os.path.join(top, "net", "npd", "npd.sys"), os.path.join(t, "SYS", "NPD.SYS"))
    for src, dst in ((os.path.join(zm, "x68k", "zmux.x"), "ZMUX.X"), (os.path.join(top, "net", "npd", "npd.x"), "NPD.X"),
                     (os.path.join(serial, "rx.x"), "RX.X"), (ZOS_X, "ZOS.X"), (ZOSH_X, "ZOSH.X")):
        sh("cp", src, os.path.join(t, dst))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(
        b"FILES     = 40\r\nBUFFERS   = 20 1024\r\nPROCESS   = 16 10 20\r\n"
        b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\nDEVICE    = \\SYS\\NPD.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(
        b"ECHO OFF\r\nPATH A:\\\r\nSET HOME=A:/\r\nSET temp=A:\\TMP\r\nRX -B19200\r\nZMUX\r\nNPD -m 192.168.68.2\r\nCTTY AUX\r\n")
    out = os.path.join(run, "zmuxzss.xdf")
    free = subprocess.run([sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), out, xdf, t],
                          capture_output=True, text=True, check=True).stdout.strip()
    print("e2e: floppy %s (%s KB free)" % (out, free))
    return out


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
        sys.exit("e2e: no %r in zmux attach output (%r)" % (pat, bytes(self.buf[-400:])))


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


xdf = mkfloppy()
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)):
        os.remove(os.path.join(run, f))
sh("cp", xdf, os.path.join(run, "fd0.xdf"))
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=env, stdin=subprocess.DEVNULL)
zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "-v"], env=env)
fails = 0
try:
    for _ in range(150):
        time.sleep(1)
        if status().startswith("line up"):
            break
    else:
        sys.exit("e2e: line did not come up")
    con = Attach("0")
    time.sleep(1)
    con.send("\r")
    con.wait("A>", 20)
    con.send("zos\r")                   # 本体の画面で Z-OS (zosh のペイン 1 つ)。タブ 0 の COMMAND.X は zos.x が終わるまで待つ
    for _ in range(60):
        time.sleep(1)
        if "zos yes" in status():
            break
    else:
        sys.exit("e2e: zos.x did not send 0xA7 records: " + status())
    print("e2e: zos.x is up (" + status() + ")")
    t0 = time.time()
    a = Attach()
    a.wait("%", 30)                     # zosh のプロンプト
    print("e2e: new session -> zosh prompt %.1fs" % (time.time() - t0))
    a.send("echo hello-zmux\r")
    out = a.wait("hello-zmux", 20)
    out = a.wait("hello-zmux", 20)
    a.wait("%", 20)
    print("e2e: echo in the session ok")
    b = Attach()
    b.wait("%", 30)
    b.send("echo second\r")
    b.wait("second", 20)
    b.wait("second", 20)
    print("e2e: 2 sessions at once ok (" + status() + ")")
    a.send("exit 3\r")
    out = a.wait("exited (3)", 30)
    print("e2e: exit 3 -> session exited (3)")
    b.send("exit\r")
    b.wait("exited", 30)
    print("e2e: " + status())
    print("ZMUX ZSS E2E PASS")
finally:
    if os.environ.get("KEEP") != "1":
        zd.terminate()
        xe.terminate()
        subprocess.run(["pkill", "-f", run + "/agent.cmd"])
