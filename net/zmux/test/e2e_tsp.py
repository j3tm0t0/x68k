#!/usr/bin/env python3
"""zmux の段階 2 (チャネル 3: tis.x -p のソケット要求を zmuxd が Pi のソケットで答える) を XEiJ で試す。
  X68000: zmux.sys + npd.sys + zmux.x + tis.x -p + npd.x -m、コンソールは CTTY AUX (チャネル 0)
  Pi (Mac): zmuxd、tisnet_peer.py (127.0.0.1:PEER)
  チャネル 0 で tisnet.x を打ち、名前引き・往復・put・get (中身の照合) が PASS になるか。rcmd.x の待ち受けにも Pi からつなぐ
usage: e2e_tsp.py RUNDIR"""
import os, socket, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
zm = os.path.dirname(here)
top = os.path.dirname(os.path.dirname(zm))
serial = os.path.join(top, "boot", "serial")
run = os.path.abspath(sys.argv[1])
XPORT = int(os.environ.get("XPORT", "54368"))
PEER = int(os.environ.get("PEER", "26990"))
RCMDPORT = int(os.environ.get("RCMDPORT", "2323"))
ZBIN = os.environ.get("ZMUX", os.path.join(zm, "target", "release", "zmux"))
RCMD_X = os.environ.get("RCMD_X", os.path.join(serial, "files-net", "RCMD.X"))
SSH_X = os.environ.get("SSH_X", "/private/tmp/claude-501/-Users-moto-Dropbox-src-github-com-j3tm0t0-x68kcli/62eb2a4a-02b9-4915-890f-ce59a34745bb/scratchpad/release-v2026.10.23.1/dl/ssh.x")
os.makedirs(run, exist_ok=True)
sock = "/tmp/zmux-e2e-%d.sock" % os.getpid()
env = dict(os.environ, ZMUX_SOCK=sock, XPORT=str(XPORT))
PUT = int(os.environ.get("PUTSIZE", "30000"))


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
    sh("cp", os.path.join(top, "net", "npd", "npd.sys"), os.path.join(t, "SYS", "NPD.SYS"))
    for src, dst in ((os.path.join(zm, "x68k", "zmux.x"), "ZMUX.X"), (os.path.join(top, "net", "npd", "npd.x"), "NPD.X"),
                     (os.path.join(top, "net", "tistack", "tis.x"), "TIS.X"),
                     (os.path.join(top, "net", "tistack", "tests", "tisnet.x"), "TISNET.X"),
                     (os.path.join(serial, "rx.x"), "RX.X")):
        sh("cp", src, os.path.join(t, dst))
    if os.path.exists(RCMD_X):
        sh("cp", RCMD_X, os.path.join(t, "RCMD.X"))
    if os.path.exists(SSH_X):
        sh("cp", SSH_X, os.path.join(t, "SSH.X"))
    os.makedirs(os.path.join(t, "TMP"))
    open(os.path.join(t, "CONFIG.SYS"), "wb").write(
        b"FILES     = 30\r\nBUFFERS   = 20 1024\r\nPROCESS   = 16 10 19\r\n"
        b"DEVICE    = \\SYS\\RSDRV.SYS\r\nDEVICE    = \\SYS\\ZMUX.SYS\r\nDEVICE    = \\SYS\\NPD.SYS\r\n")
    open(os.path.join(t, "AUTOEXEC.BAT"), "wb").write(
        b"ECHO OFF\r\nPATH A:\\\r\nSET temp=A:\\TMP\r\nRX -B19200\r\nZMUX\r\nTIS -p\r\nNPD -m 192.168.68.2\r\nCTTY AUX\r\n")
    out = os.path.join(run, "zmuxtsp.xdf")
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

    def cmd(self, line, t=120):
        self.send(line + "\r")
        self.wait(line, 10)
        end = time.time() + t
        while time.time() < end and b"A>" not in self.buf:
            time.sleep(0.1)
        if b"A>" not in self.buf:
            # 止まった: ESC (Ctrl+C) で抜けて、X68000 側の統計を見る
            self.send("\x03")
            time.sleep(3)
            self.send("zmux -s\r")
            time.sleep(3)
            print("e2e: STALLED; X68000 zmux stats:\n" + bytes(self.buf[-600:]).decode("cp932", "replace"))
            sys.exit(1)
        out = self.wait("A>", 5)
        return out.decode("cp932", "replace")


def status():
    return subprocess.run([ZBIN, "status"], capture_output=True, text=True, env=env).stdout.strip()


xdf = mkfloppy()
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)):
        os.remove(os.path.join(run, f))
sh("cp", xdf, os.path.join(run, "fd0.xdf"))
peer = subprocess.Popen([sys.executable, os.path.join(top, "net", "tistack", "tests", "tisnet_peer.py"), str(PEER), "0.0.0.0"])
xe = subprocess.Popen([os.path.join(serial, "test", "xeij-fd.sh"), run], env=env, stdin=subprocess.DEVNULL)
zd = subprocess.Popen([ZBIN, "daemon", "--tcp", "127.0.0.1:%d" % XPORT, "-v"], env=env)
fails = 0
try:
    t0 = time.time()
    for _ in range(150):
        time.sleep(1)
        if status().startswith("line up"):
            break
    else:
        sys.exit("e2e: line did not come up: " + status())
    print("e2e: boot to line up %.1fs" % (time.time() - t0))
    a = Attach()
    time.sleep(1)
    a.send("\r")
    a.wait("A>", 20)
    # 192.168.68.1 は Pi 自身 (zmuxd が 127.0.0.1 につなぐ)。X68000 の 127.0.0.0/8 は tis.x の中のループバック
    # 名前 (.local など Pi の getaddrinfo / mDNS で引けるもの): X68000 の gethostbyname → tis.x -p → TSP の RESOLVE → zmuxd。ssh.x の getaddrinfo も同じ道
    names = [n for n in (os.environ.get("RESOLVE_NAME"), socket.gethostname() + ".local", socket.gethostname()) if n]
    rname = None
    for n in names:
        try:
            socket.getaddrinfo(n, None, socket.AF_INET)
            rname = n
            break
        except OSError:
            pass
    extra = []
    if rname:
        extra = [("tisnet resolve " + rname, "PASS"), ("tisnet echo %s %d 20" % (rname, PEER), "PASS")]
    else:
        print("e2e: no resolvable host name here, name tests skipped")
    for line, want in extra + [("tisnet resolve localhost", "PASS"),
                       ("tisnet echo 192.168.68.1 %d 20" % PEER, "PASS"),
                       ("tisnet put 192.168.68.1 %d %d" % (PEER, PUT), "PASS"),
                       ("tisnet get 192.168.68.1 %d %d" % (PEER, PUT), "PASS")]:
        t1 = time.time()
        out = a.cmd(line, int(os.environ.get("CMDWAIT", "300")))
        ok = want in out and "FAIL" not in out
        fails += not ok
        print("e2e: %-40s %s (%.1fs)\n    %s" % (line, "ok" if ok else "NG", time.time() - t1,
              "\n    ".join(l for l in out.replace("\r", "").splitlines()[1:] if l.strip() and l.strip() != "A>")))
    if os.path.exists(SSH_X) and os.environ.get("SSHTEST", "1") == "1":
        # ssh.x の鍵交換とホスト鍵の検証を Pi で計算させる (-oAccel=all、0xA5)。相手は Pi (Mac) の sshd (192.168.68.1 = 127.0.0.1:22)。
        # ユーザ名は存在しないものにして、認証の手前 (鍵交換が済んだところ) まで見る
        t1 = time.time()
        a.send("ssh -y -y -oAccel=all zmuxnouser@192.168.68.1 true\r")
        end = time.time() + 240
        while time.time() < end and not any(k in a.buf for k in (b"assword", b"denied", b"No auth", b"A>", b"rror")):
            time.sleep(0.2)
        out = bytes(a.buf).decode("cp932", "replace")
        acc = status().split(" accel ")[1].split()[0] if " accel " in status() else "?"
        ok = any(k in out for k in ("assword", "denied", "No auth")) and acc not in ("0", "?")
        fails += not ok
        print("e2e: ssh.x -oAccel=all key exchange via zmux: %s (%.1fs, accel requests %s)\n    %s" % ("ok" if ok else "NG", time.time() - t1, acc, repr(out[-500:])))
        for _ in range(3):
            a.send("\x03")
            time.sleep(1)
            a.send("\r")
            time.sleep(1)
            if b"A>" in a.buf:
                break
        a.buf.clear() if hasattr(a.buf, "clear") else None
        a.send("\r")
        a.wait("A>", 30)
    if os.path.exists(SSH_X) and os.environ.get("HOSTKEY", "1") == "1":
        # 初めての相手のホスト鍵の y/n の確認 (CTTY AUX のコンソールから n / y が効く) と、~/.ssh が作れない Human68k での known_hosts の置き場 (~/ssh)
        def drain(t=3):
            time.sleep(t)
            a.buf.clear()
        def ssh_try(answer, label):
            a.buf.clear()
            a.send("ssh zmuxnouser@192.168.68.1 true\r")
            end = time.time() + 240
            while time.time() < end and b"y/n" not in a.buf and b"A>" not in a.buf[20:]:
                time.sleep(0.2)
            asked = b"y/n" in a.buf
            if asked:
                a.send(answer + "\r")
            end = time.time() + 240
            while time.time() < end and not any(k in a.buf for k in (b"assword", b"denied", b"No auth", b"A>", b"rror", b"failed")):
                time.sleep(0.2)
            out = bytes(a.buf).decode("cp932", "replace")
            return asked, out
        a.send("set HOME=A:/home\r"); a.wait("A>", 10)
        a.send("md A:\\home\r"); a.wait("A>", 10)
        for _ in range(3):
            a.send("\x03"); time.sleep(1); a.send("\r"); time.sleep(1)
        asked, out = ssh_try("n", "n")
        ok = asked and any(k in out for k in ("failed", "rror", "A>")) and "assword" not in out
        fails += not ok
        print("e2e: ssh host key prompt, answer n: %s asked=%s %r" % ("ok" if ok else "NG", asked, out[-300:]))
        for _ in range(3):
            a.send("\x03"); time.sleep(1); a.send("\r"); time.sleep(1)
        a.buf.clear(); a.send("\r"); a.wait("A>", 30)
        asked, out = ssh_try("y", "y")
        ok = asked and any(k in out for k in ("assword", "denied", "No auth"))
        fails += not ok
        print("e2e: ssh host key prompt, answer y: %s asked=%s %r" % ("ok" if ok else "NG", asked, out[-300:]))
        for _ in range(3):
            a.send("\x03"); time.sleep(1); a.send("\r"); time.sleep(1)
        a.buf.clear(); a.send("\r"); a.wait("A>", 30)
        a.send("dir A:\\home\\ssh\r")
        out = (a.wait("A>", 20) or b"").decode("cp932", "replace")
        ok = "KNOWN" in out.upper()
        fails += not ok
        print("e2e: known_hosts saved under A:\\home\\ssh: %s %r" % ("ok" if ok else "NG", out[-200:]))
        asked, out = ssh_try("y", "again")
        ok = not asked and any(k in out for k in ("assword", "denied", "No auth"))
        fails += not ok
        print("e2e: second connection asks nothing: %s asked=%s" % ("ok" if ok else "NG", asked))
        for _ in range(3):
            a.send("\x03"); time.sleep(1); a.send("\r"); time.sleep(1)
        a.buf.clear(); a.send("\r"); a.wait("A>", 30)
    if os.path.exists(RCMD_X):
        # rcmd.x を X68000 で待ち受けさせ、Pi (Mac) の 127.0.0.1:2323 からつなぐ (zmuxd が Pi で listen する)
        a.send("rcmd -s A:\\COMMAND.X\r")
        time.sleep(4)
        t1 = time.time()
        r = subprocess.run(["bash", os.path.join(top, "harness", "raspi", "x68.sh"), "ver"], capture_output=True, text=True,
                           env=dict(env, X68_HOST="127.0.0.1", X68_PORT=str(RCMDPORT)), timeout=60)
        ok = "Human68k" in r.stdout and r.returncode == 0
        fails += not ok
        print("e2e: x68 ver via rcmd.x over TSP: %s (%.1fs) %r" % ("ok" if ok else "NG", time.time() - t1, r.stdout.strip()))
        blob = os.path.join(run, "blob30k")
        open(blob, "wb").write(bytes((i * 7) & 255 for i in range(30000)))
        # rcmd.x (backlog 0) は接続ごとに listen し直すので、前の接続の直後はつながらないことがある。つながるまで待つ
        for _ in range(10):
            time.sleep(1)
            t1 = time.time()
            r = subprocess.run(["bash", os.path.join(top, "harness", "raspi", "x68put.sh"), blob, "A:\\BLOB.BIN"], capture_output=True,
                               text=True, env=dict(env, X68_HOST="127.0.0.1", X68_PORT=str(RCMDPORT), X68PUT_RAW="1"), timeout=300)
            if "refused" not in r.stderr:
                break
        print("e2e: x68put 30000 bytes over TSP: rc %d (%.1fs) %s" % (r.returncode, time.time() - t1, (r.stdout + r.stderr).strip()))
        fails += r.returncode != 0
        for _ in range(10):             # rcmd.x は接続ごとに listen し直すので、つながるまで繰り返す
            time.sleep(1)
            subprocess.run(["bash", os.path.join(top, "harness", "raspi", "x68.sh"), "exit"], capture_output=True,
                           env=dict(env, X68_HOST="127.0.0.1", X68_PORT=str(RCMDPORT)), timeout=30)
            if b"A>" in a.buf:
                break
        a.wait("A>", 30)
    print("e2e: " + status())
    print("ZMUX TSP E2E %s" % ("PASS" if not fails else "FAIL (%d)" % fails))
finally:
    if os.environ.get("KEEP") != "1":
        zd.terminate()
        peer.terminate()
        xe.terminate()
        subprocess.run(["pkill", "-f", run + "/agent.cmd"])
sys.exit(1 if fails else 0)
