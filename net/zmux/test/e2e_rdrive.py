#!/usr/bin/env python3
"""zmux のリモートドライブ (チャネル 2、zrmt.sys) を XEiJ で試す。
  1. HUMAN.SYS / COMMAND.X / RSDRV.SYS / zmux.sys / zrmt.sys / zmux.x / rx.x の起動フロッピー (CONFIG.SYS に ZRMT.SYS)
  2. XEiJ (Compact、主記憶 2MB) で起動。zmuxd を --drive DIR で起動 (DIR に HELLO.TXT と BIG.BIN 64KB)
  3. zmux attach 0 の COMMAND.X で: dir / type / copy (Pi → X68000、X68000 → Pi、照合) / md / del / rd。copy の速さを測る
usage: e2e_rdrive.py RUNDIR   (XEIJ_TOOLS / HUMAN_XDF は e2e.py と同じ)"""
import os, re, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT))
drive = os.path.join(run, "drive")
fails = []
REAL = bool(os.environ.get("REAL"))
SMALL = REAL or bool(os.environ.get("SMALL"))     # 空きの少ないフロッピーへ小さいファイルを写す場面


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
    sh("cp", os.path.join(zm, "x68k", "zrmt.sys"), os.path.join(t, "SYS", "ZRMT.SYS"))
    sh("cp", os.path.join(zm, "x68k", "zmux.x"), os.path.join(t, "ZMUX.X"))
    sh("cp", os.path.join(serial, "rx.x"), os.path.join(t, "RX.X"))
    if REAL:
        # 実機の XVI の構成: contee78 (78125bps)、npd.sys、tis.x -p、npd.x -m、rx -K2、フロッピーの空きは 19KB
        net = os.path.join(top, "net")
        no = os.environ.get("REAL_NO", "").split(",")      # 切り分け用: contee / npd (npd.sys) / tis (tis.x -p) / npdx (npd.x -m)
        sh("cp", os.path.join(top, "harness", "contee", "contee78.sys"), os.path.join(t, "SYS", "CONTEE.SYS"))
        sh("cp", os.path.join(net, "npd", "npd.sys"), os.path.join(t, "SYS", "NPD.SYS"))
        sh("cp", os.path.join(net, "npd", "npd.x"), os.path.join(t, "NPD.X"))
        sh("cp", os.path.join(net, "tistack", "tis.x"), os.path.join(t, "TIS.X"))
        open(os.path.join(t, "CONFIG.SYS"), "wb").write(
            b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 8 10 20\r\n"
            b"DEVICE    = \\SYS\\RSDRV.SYS\r\n" + (b"" if "contee" in no else b"DEVICE    = \\SYS\\CONTEE.SYS\r\n") + b"DEVICE    = \\SYS\\ZMUX.SYS\r\n"
            + (b"" if "npd" in no else b"DEVICE    = \\SYS\\NPD.SYS\r\n") + b"DEVICE    = \\SYS\\ZRMT.SYS\r\n")
        open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(
            b"ECHO OFF\r\nPATH A:\\\r\nRX -B78125 -K2\r\nZMUX\r\n" + (b"" if "tis" in no else b"TIS -p\r\n")
            + (b"" if "npdx" in no or "npd" in no else b"NPD -m 192.168.68.2\r\n") + b"CTTY AUX\r\n")
        open(os.path.join(t, "SYS", "CONTEE.OLD"), "wb").write(os.urandom(3000))
        used = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(t) for f in fs)
        pad = max(0, 1232 * 1024 - 8 * 1024 - used - 19 * 1024 - 20 * 1024) if not os.environ.get("NOPAD") else 0
        open(os.path.join(t, "PAD.DAT"), "wb").write(b"\0" * pad)
    else:
        open(os.path.join(t, "CONFIG.SYS"), "wb").write(
            b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 8 10 20\r\n"
            b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\nDEVICE    = \\SYS\\ZRMT.SYS\r\n")
        open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(
            b"ECHO OFF\r\nPATH A:\\\r\nRX -B19200\r\nZMUX\r\nCTTY AUX\r\n")
    out = os.path.join(run, "zmuxrd.xdf")
    free = subprocess.run([sys.executable, os.path.join(serial, "floppy", "mkxdf.py"), out, xdf, t],
                          capture_output=True, text=True, check=True).stdout.strip()
    print("e2e: floppy %s (%s KB free)" % (out, free))
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

    def cmd(self, line, t=120):
        """コマンドを打ち、次のプロンプトまでの出力を返す (かかった秒も)"""
        t0 = time.time()
        self.send(line + "\r")
        out = self.wait("A>", t)
        return out, time.time() - t0

    def close(self):
        self.p.kill()


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


def screen():
    return subprocess.run([sys.executable, os.path.join(serial, "test", "xscreen.py"), run], capture_output=True, text=True).stdout


os.makedirs(drive, exist_ok=True)
for f in os.listdir(drive):
    p = os.path.join(drive, f)
    if os.path.isdir(p):
        subprocess.run(["rm", "-rf", p])
    else:
        os.remove(p)
open(os.path.join(drive, "HELLO.TXT"), "wb").write(b"hello from the Pi\r\n")
big = os.urandom(65536)
open(os.path.join(drive, "BIG.BIN"), "wb").write(big)
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
        s = screen()
        if "zmux: thread started" in s:
            break
    else:
        sys.exit("e2e: zmux did not start:\n" + s)
    print("e2e: boot to zmux %.1fs" % (time.time() - t0))
    m = re.search(r"zrmt\.sys[^\n]*?([A-Z]): ", s)
    dl = m.group(1) if m else "C"
    check("zrmt.sys installed (drive %s:)" % dl, "zrmt.sys" in s and "組み込みません" not in s, "\n".join(l for l in s.splitlines() if "zrmt" in l))
    zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "--drive", drive, "-v"], env=env,
                          stderr=open(os.path.join(run, "zmuxd.log"), "w"))
    for _ in range(100):
        time.sleep(0.2)
        if status().startswith("line up"):
            break
    check("line up", status().startswith("line up"), status())
    a = Attach()
    time.sleep(0.5)
    a.send("\r")
    a.wait("A>", 10)
    out, _ = a.cmd("dir %s:" % dl)
    check("dir %s: lists the Pi directory" % dl, out is not None and "HELLO" in out and "BIG" in out, repr(out)[-200:])
    out, _ = a.cmd("type %s:\\HELLO.TXT" % dl)
    check("type reads a file", out is not None and "hello from the Pi" in out, repr(out)[-120:])
    if SMALL:
        # 実機の構成: 空きの少ないフロッピーへ小さいファイルを写す (C: → A:、A: → A:)。エラーの窓 (あれば) が出ていないか画面も見る
        open(os.path.join(drive, "SMALL.SYS"), "wb").write(os.urandom(5000))
        for line, name in (("copy %s:\\SMALL.SYS A:\\SYS\\CONTEE.NEW" % dl, "C: to A:"),
                           ("copy A:\\SYS\\CONTEE.NEW A:\\SYS\\CONTEE.BAK", "A: to A:"),
                           ("copy A:\\SYS\\CONTEE.OLD A:\\SYS\\CONTEE.OL2", "A: to A: (2)"),
                           ("copy A:\\SYS\\CONTEE.BAK %s:\\BACK.SYS" % dl, "A: to C:")):
            out, _ = a.cmd(line, 120)
            s = screen()
            check("copy " + name, out is not None and "コピーしました" in out and "無効" not in out and "コピーはされませんでした" not in out and "無効なコマンド" not in s,
                  repr(out)[-100:] + " | " + s.replace("\n", " ")[-120:])
        back = os.path.join(drive, "BACK.SYS")
        check("round trip bytes match", os.path.exists(back) and open(back, "rb").read() == open(os.path.join(drive, "SMALL.SYS"), "rb").read())
        print("e2e: " + status())
        a.close()
        print("ZMUX RDRIVE E2E %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
        raise SystemExit(1 if fails else 0)
    out, t_rd = a.cmd("copy %s:\\BIG.BIN NUL" % dl, 300)
    check("copy BIG.BIN NUL (read, 64KB)", out is not None and "エラー" not in out, "%.1fs = %.2f KB/s" % (t_rd, 64 / t_rd))
    out, t_fd = a.cmd("copy %s:\\BIG.BIN A:\\BIG.BIN" % dl, 300)
    check("copy to the floppy", out is not None and "エラー" not in out, "%.1fs (フロッピーの書き込み込み)" % t_fd)
    out, _ = a.cmd("copy A:\\BIG.BIN A:\\BIG2.BIN", 300)
    check("copy A: to A: under zmux (floppy write with the console on AUX)", out is not None and "エラー" not in out and "無効" not in out and "コピーしました" in out, repr(out)[-120:])
    s = screen()
    check("no error dialog on the screen", "無効なコマンド" not in s and "デバイスドライバ" not in s, s[-200:])
    out, t_wr = a.cmd("copy A:\\BIG.BIN %s:\\BACK.BIN" % dl, 300)
    back = os.path.join(drive, "BACK.BIN")
    check("copy back to the Pi (write) and the bytes match", out is not None and os.path.exists(back) and open(back, "rb").read() == big,
          "%.1fs = %.2f KB/s (フロッピーの読み込み込み)" % (t_wr, 64 / t_wr))
    out, _ = a.cmd("copy A:\\COMMAND.X %s:\\CMD.X" % dl, 300)
    cmdx = open(os.path.join(run, "tree", "COMMAND.X"), "rb").read()
    check("COMMAND.X copied to the Pi matches", os.path.exists(os.path.join(drive, "CMD.X")) and open(os.path.join(drive, "CMD.X"), "rb").read() == cmdx)
    out, _ = a.cmd("md %s:\\SUB" % dl)
    check("md", os.path.isdir(os.path.join(drive, "SUB")), repr(out)[-80:])
    out, _ = a.cmd("copy %s:\\HELLO.TXT %s:\\SUB\\" % (dl, dl))
    check("copy into the subdirectory", os.path.exists(os.path.join(drive, "SUB", "HELLO.TXT")), repr(out)[-80:])
    out, _ = a.cmd("dir %s:\\SUB" % dl)
    check("dir of the subdirectory", out is not None and "HELLO" in out, repr(out)[-120:])
    out, _ = a.cmd("del %s:\\SUB\\HELLO.TXT" % dl)
    check("del", not os.path.exists(os.path.join(drive, "SUB", "HELLO.TXT")), repr(out)[-80:])
    out, _ = a.cmd("rd %s:\\SUB" % dl)
    check("rd", not os.path.exists(os.path.join(drive, "SUB")), repr(out)[-80:])
    out, _ = a.cmd("ren %s:\\BACK.BIN BACK2.BIN" % dl)
    check("ren", os.path.exists(os.path.join(drive, "BACK2.BIN")), repr(out)[-80:])
    out, _ = a.cmd("dir %s:\\NOPE" % dl)
    check("dir of a missing directory gives an error, not a hang", out is not None, repr(out)[-80:])
    print("e2e: " + status())
    a.close()
    print("ZMUX RDRIVE E2E %s (%d failures)" % ("PASS" if not fails else "FAIL", len(fails)))
finally:
    if zd:
        zd.terminate()
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
sys.exit(1 if fails else 0)
