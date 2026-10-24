#!/usr/bin/env python3
"""sx.py -- RS-232C だけで X68000 にツールを送り込む送り手 (Mac / Raspberry Pi、標準ライブラリだけで動く)

つなぎ先:
  --port /dev/ttyUSB0      シリアルポート (termios で直接開く。pyserial は要らない)
  --tcp 127.0.0.1:54368    XEiJ の RS-232C (TCP/IP ⇔ AUX)。XEiJ が回線の速さをまねる
  --baud 9600              いまの X68000 側の速さ (既定 9600。X68000 の SRAM の既定と同じ)

段取り (X68000 で CTTY AUX を打ったあと):
  sx.py stage1             COPY AUX RX.BAS → BASIC RX で rx.x を 16 進で送る (X-BASIC が受けて RX.X を書く)
  sx.py stage2             RX を起動して 19200bps に切り替え、あいさつを確かめる
  sx.py stage3             道具と AUTOEXEC.BAT の追記 (RX -B19200 / CTTY AUX) を送る
  sx.py bootstrap          stage1 から stage3 まで続けて行う
以後 (rx.x のサーバに対して。動いていなければ A> に RX と打って起動する):
  sx.py run 'dir'          COMMAND.X で実行して出力と終了コードを返す
  sx.py put LOCAL [REMOTE] [--append]
  sx.py get REMOTE [LOCAL]
  sx.py speed 19200        rx.x と送り手の両方の速さを変える
  sx.py quit               rx.x を止めて A> に戻す
  sx.py probe              CR を送って A> が返るかを見る
  sx.py term               端末として打ち込む (Ctrl+] で終わる)
"""
import argparse, os, select, socket, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SPEEDS = [75, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400]


class Link:
    """シリアルポートか TCP。読み込みはバッファに溜め、XON/XOFF は text モードのときだけ解釈する"""

    def __init__(self, a):
        self.baud = a.baud
        self.pace = a.pace
        self.log = open(a.log, "ab", 0) if a.log else None
        self.buf = bytearray()
        self.xoff = False
        self.text = True
        self.sent = self.recvd = 0
        # 試験用: SX_CORRUPT=0.05 なら、壊してよい送信 (段階 1 の行と rx.x の枠) の 5% で 1 バイトを壊す
        self.corrupt = float(os.environ.get("SX_CORRUPT", "0"))
        self.may_corrupt = False
        self.corrupted = 0
        if a.tcp:
            h, p = a.tcp.rsplit(":", 1)
            for _ in range(60):
                try:
                    self.s = socket.create_connection((h, int(p)))
                    break
                except OSError:
                    time.sleep(1)
            else:
                sys.exit("sx: cannot connect to %s" % a.tcp)
            self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.fd = self.s.fileno()
            self.tty = False
            # XEiJ は RS-232C の速さをまねて届けるので、こちらで間をあける必要はない (--pace で強制できる)
        else:
            import termios
            self.termios = termios
            self.fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            self.tty = True
            self.pace = False
            self.setbaud(self.baud)

    def setbaud(self, b):
        self.baud = b
        if not self.tty:
            return
        t = self.termios
        at = t.tcgetattr(self.fd)
        at[0] = 0                                   # iflag: XON/XOFF は自分で扱う
        at[1] = 0                                   # oflag
        at[2] = t.CS8 | t.CREAD | t.CLOCAL          # cflag: 8N1、モデム信号は見ない
        at[3] = 0                                   # lflag: raw
        sp = getattr(t, "B%d" % b)
        at[4] = at[5] = sp
        at[6][t.VMIN] = 0
        at[6][t.VTIME] = 0
        t.tcsetattr(self.fd, t.TCSANOW, at)
        t.tcflush(self.fd, t.TCIOFLUSH)

    def _fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], max(0, timeout))
        if not r:
            return False
        d = self.s.recv(4096) if not self.tty else os.read(self.fd, 4096)
        if not d:
            if not self.tty:
                sys.exit("sx: connection closed")
            return False
        if self.log:
            self.log.write(b"R>" + d + b"\n")
        self.recvd += len(d)
        if self.text:
            for c in d:
                if c == 0x13:
                    self.xoff = True
                elif c == 0x11:
                    self.xoff = False
                else:
                    self.buf.append(c)
        else:
            self.buf += d
        return True

    def write(self, data):
        data = bytes(data)
        if self.corrupt and self.may_corrupt and len(data) > 2:
            import random
            if random.random() < self.corrupt:
                i = random.randrange(1, len(data))
                data = data[:i] + bytes([data[i] ^ 0x5A]) + data[i + 1:]
                self.corrupted += 1
        if self.log:
            self.log.write(b"X>" + data + b"\n")
        chunk = 16 if self.pace else 4096
        for i in range(0, len(data), chunk):
            while self.text and self.xoff:
                self._fill(0.1)
            part = data[i:i + chunk]
            t0 = time.time()
            if self.tty:
                while part:
                    try:
                        n = os.write(self.fd, part)
                    except BlockingIOError:
                        n = 0
                    part = part[n:]
                    if part:
                        select.select([], [self.fd], [], 1)
            else:
                self.s.sendall(part)
            if self.pace:   # 10 ビット/バイトで回線の速さをまねる
                dt = len(data[i:i + chunk]) * 10.0 / self.baud - (time.time() - t0)
                if dt > 0:
                    time.sleep(dt)
            self.sent += len(data[i:i + chunk])
        if self.tty:
            self.termios.tcdrain(self.fd)

    def read(self, n, timeout):
        end = time.time() + timeout
        while len(self.buf) < n:
            if not self._fill(end - time.time()) and time.time() >= end:
                break
        d = bytes(self.buf[:n])
        del self.buf[:n]
        return d

    def expect(self, pat, timeout):
        """pat (bytes) が来るまで読む。来たらそこまでを返す。来なければ None"""
        end = time.time() + timeout
        while True:
            i = self.buf.find(pat)
            if i >= 0:
                d = bytes(self.buf[:i + len(pat)])
                del self.buf[:i + len(pat)]
                return d
            if not self._fill(end - time.time()) and time.time() >= end:
                return None

    def expect_any(self, pats, timeout):
        """pats のどれかが来るまで読み、来たもの (bytes) を返す。来なければ None"""
        end = time.time() + timeout
        while True:
            hits = [(self.buf.find(p), p) for p in pats if self.buf.find(p) >= 0]
            if hits:
                i, p = min(hits)
                del self.buf[:i + len(p)]
                return p
            if not self._fill(end - time.time()) and time.time() >= end:
                return None

    def drain(self, quiet=0.3):
        while self._fill(quiet):
            pass
        d = bytes(self.buf)
        self.buf.clear()
        return d


def say(*a):
    print("sx:", *a, file=sys.stderr, flush=True)


def sjis(b):
    return b.decode("cp932", "replace").replace("\r", "")


# ---------------------------------------------------------------- 段階 1: X-BASIC

def hexline(seq, data):
    body = data + bytes([seq & 255, len(data)])
    s = (-sum(body)) & 255
    return b":" + (body + bytes([s])).hex().upper().encode() + b"\r"


def console_cmd(L, line, wait=b"A>", timeout=10):
    """CTTY AUX の COMMAND.X に 1 行打ち、プロンプトが戻るまでの出力を返す"""
    L.text = True
    L.drain(0.2)
    L.write(line.encode("cp932") + b"\r")
    if wait is None:
        return b""
    d = L.expect(wait, timeout)
    if d is None:
        raise SystemExit("sx: no %r after %r (got %r)" % (wait, line, L.drain()))
    return d


def probe(L, tries=3):
    L.text = True
    for _ in range(tries):
        L.drain(0.2)
        L.write(b"\r")
        if L.expect(b">", 2) is not None:
            L.drain(0.3)
            return True
    return False


def stage1(L, a):
    rxx = open(a.rx, "rb").read()
    bas = open(os.path.join(HERE, "rx.bas"), "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    t0 = time.time()
    if not a.manual:
        if not probe(L):
            raise SystemExit("sx: no prompt. X68000 で CTTY AUX を打ったか、速さ (--baud) が合っているかを確かめる")
        say("prompt ok; COPY AUX RX.BAS")
        console_cmd(L, "COPY AUX RX.BAS", wait=b"\n", timeout=5)
        time.sleep(0.3)
    else:
        say("X68000 で COPY AUX RX.BAS を打ってから Enter を押す")
        input()
    for line in bas.split(b"\r\n"):
        if line:
            L.write(line + b"\r\n")
            time.sleep(0.02)
    L.write(b"\x1a")
    if not a.manual:
        if L.expect(b"A>", 15) is None:
            raise SystemExit("sx: COPY did not finish")
        say("RX.BAS sent (%d bytes, %.1fs); BASIC RX" % (len(bas), time.time() - t0))
        console_cmd(L, "BASIC RX", wait=None)
    else:
        say("X68000 で BASIC RX を打つ")
    if L.expect(b"#", 60 if not a.manual else 600) is None:
        raise SystemExit("sx: BASIC did not start")
    L.drain(0.2)
    t1 = time.time()
    seq, i, retries = 0, 0, 0
    n = a.line
    while True:
        data = rxx[i:i + n]
        line = hexline(seq, data)
        for attempt in range(20):
            L.may_corrupt = True
            L.write(line)
            L.may_corrupt = False
            r = L.expect_any([b"K", b"N"], 5)
            if r == b"K":
                break
            retries += 1
            say("line %d resend (%r)" % (seq, r))
            L.drain(0.2)
            L.write(b"\x11")   # 壊れたバイトが 13h (XOFF) になって X68000 の送信が止まっていたら再開させる
        else:
            raise SystemExit("sx: line %d failed" % seq)
        if not data:
            break
        i += len(data)
        seq = (seq + 1) & 255
        if seq % 16 == 0:
            say("%d/%d bytes" % (i, len(rxx)))
    d = L.expect(b"\r", 30)
    j = d.find(b"S") if d else -1
    if j < 0 or len(d) < j + 5:
        raise SystemExit("sx: no checksum from BASIC (%r)" % d)
    got = 0
    for c in d[j + 1:j + 5]:
        got = got * 16 + (c - 48)
    want = sum(rxx) & 0xffff
    if got != want:
        raise SystemExit("sx: RX.X checksum mismatch: X68000 %04X, here %04X" % (got, want))
    if L.expect(b"A>", 20) is None and not a.manual:
        raise SystemExit("sx: BASIC did not exit")
    say("RX.X ok: %d bytes, sum %04X, %d resends, %.1fs (hex part %.1fs)%s" %
        (len(rxx), got, retries, time.time() - t0, time.time() - t1,
         " (corrupted %d lines on purpose)" % L.corrupted if L.corrupt else ""))


# ---------------------------------------------------------------- 段階 2 以降: rx.x

def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
            c &= 0xFFFF
    return c


class RX:
    def __init__(self, L, verbose=False):
        self.L = L
        self.seq = int(time.time() * 7) & 0xff
        self.verbose = verbose
        self.resends = 0

    def frame(self, typ, payload=b""):
        body = bytes([ord(typ), self.seq, len(payload) >> 8, len(payload) & 255]) + payload
        c = crc16(body)
        return b"\x01" + body + bytes([c >> 8, c & 255])

    def recv(self, timeout):
        L = self.L
        end = time.time() + timeout
        while True:
            if L.expect(b"\x01", max(0.01, end - time.time())) is None:
                return None
            h = L.read(4, 1.0)
            if len(h) < 4:
                return None
            n = (h[2] << 8) | h[3]
            if n > 4096:
                continue
            rest = L.read(n + 2, 2.0 + n * 12.0 / L.baud)
            if len(rest) < n + 2:
                return None
            body = h + rest[:n]
            if crc16(body) != (rest[n] << 8 | rest[n + 1]):
                return None
            return chr(h[0]), h[1], rest[:n]

    def call(self, typ, payload=b"", timeout=None, tries=10):
        L = self.L
        L.text = False
        self.seq = (self.seq + 1) & 255
        f = self.frame(typ, payload)
        if timeout is None:
            timeout = 2.0 + (len(f) + 4200) * 10.0 / L.baud
        for _ in range(tries):
            L.may_corrupt = True
            L.write(f)
            L.may_corrupt = False
            r = self.recv(timeout)
            if r and r[1] == self.seq and r[0] in "KE":
                if r[0] == "E":
                    code = int.from_bytes(r[2][:4], "big", signed=True) if len(r[2]) >= 4 else 0
                    raise IOError("rx: %s failed (%d)" % (typ, code))
                return r[2]
            self.resends += 1
            if self.verbose:
                say("resend %s seq %d (%r)" % (typ, self.seq, r))
            L.drain(0.1)
        raise SystemExit("sx: no answer to %s" % typ)

    def hello(self):
        return self.call("H", tries=3).decode()

    def put(self, data, remote, append=False, chunk=2048, progress=True):
        t0 = time.time()
        self.call("O", bytes([1 if append else 0]) + remote.encode("cp932"))
        for i in range(0, len(data), chunk):
            self.call("W", data[i:i + chunk])
            if progress and sys.stderr.isatty():
                el = time.time() - t0
                sys.stderr.write("\rsx: %s %d/%d bytes %.0fs %.2f KB/s " %
                                 (remote, i + len(data[i:i + chunk]), len(data), el,
                                  (i + len(data[i:i + chunk])) / 1024.0 / max(el, 0.001)))
        self.call("C", timeout=30)
        el = time.time() - t0
        if progress and sys.stderr.isatty():
            sys.stderr.write("\n")
        return el

    def get(self, remote):
        size = int.from_bytes(self.call("P", remote.encode("cp932")), "big")
        out = bytearray()
        while True:
            d = self.call("R", (2048).to_bytes(2, "big"))
            if not d:
                break
            out += d
        self.call("C")
        if len(out) != size:
            raise SystemExit("sx: get %s: %d of %d bytes" % (remote, len(out), size))
        return bytes(out)

    def run(self, cmd):
        r = self.call("X", cmd.encode("cp932"), timeout=600, tries=2)
        rc = int.from_bytes(r[:4], "big", signed=True)
        out = self.get(r[4:].decode())
        return rc, out

    def speed(self, bps):
        mode = 0x4C00 | SPEEDS.index(bps)
        self.call("B", mode.to_bytes(2, "big"))
        time.sleep(0.3)
        self.L.setbaud(bps)
        time.sleep(0.2)
        self.L.drain(0.2)
        return self.hello()

    def quit(self):
        self.call("Q")
        self.L.text = True


def connect_rx(L, a, start=True):
    """rx.x のサーバにつなぐ。答えがなければ A> に RX と打って起動する"""
    r = RX(L, a.verbose)
    # 先に COMMAND.X かどうかを見る (XON を送ってから CR。rx.x は SOH 以外を読み捨てる)。
    # COMMAND.X に 2 進の枠を送ると、中の 13h (XOFF) で X68000 の送信が止まることがある
    L.text = True
    L.drain(0.1)
    L.write(b"\x11\r")
    if L.expect(b">", 1.5) is None:
        L.text = False
        L.drain(0.1)
        try:
            h = r.call("H", tries=3, timeout=1.5)
            return r, h.decode()
        except SystemExit:
            raise SystemExit("sx: neither A> nor rx.x answered (CTTY AUX になっていないか、速さが違う)")
    if not start:
        raise SystemExit("sx: rx.x is not running")
    say("starting rx.x")
    L.drain(0.3)
    L.write(b"RX\r")
    L.text = False
    if r.recv(10) is None:
        raise SystemExit("sx: rx.x did not start")
    return r, r.hello()


def stage2(L, a):
    r, h = connect_rx(L, a)
    say("rx.x: %s" % h)
    if a.speed and a.speed != L.baud:
        say("switching to %d bps" % a.speed)
        h = r.speed(a.speed)
        say("rx.x at %d: %s" % (a.speed, h))
    return r


# AUTOEXEC.BAT の末尾に足す。RX -K2 は 2 秒のうちにキーが押されたら 1 を返し、CTTY AUX を飛ばす
AUTOEXEC_ADD = ("REM --- boot/serial: RS-232C 19200bps をコンソールにする (2 秒以内にキーで取りやめ) ---\r\n"
                "RX -B19200 -K2\r\n"
                "IF ERRORLEVEL 1 GOTO RXKEEP\r\n"
                "CTTY AUX\r\n"
                ":RXKEEP\r\n").encode("cp932")


def stage3(L, a, r=None):
    if r is None:
        r, h = connect_rx(L, a)
    files = []
    for spec in a.files:
        local, _, remote = spec.partition("=")
        files.append((local, remote or os.path.basename(local).upper()))
    total, tt = 0, 0.0
    for local, remote in files:
        data = open(local, "rb").read()
        el = r.put(data, remote, chunk=a.chunk)
        back = r.get(remote) if a.verify else data
        if back != data:
            raise SystemExit("sx: verify failed for %s" % remote)
        total += len(data)
        tt += el
        say("%s: %d bytes %.1fs %.2f KB/s%s" % (remote, len(data), el, len(data) / 1024.0 / el,
                                               " (verified)" if a.verify else ""))
    if a.autoexec:
        orig = r.get("A:\\AUTOEXEC.BAT")
        if b"CTTY AUX" in orig.upper():
            say("AUTOEXEC.BAT already has CTTY AUX")
        else:
            # 元の AUTOEXEC.BAT は末尾に 1Ah (EOF) がある。その後ろに足しても実行されないので、外してから足す
            body = orig.split(b"\x1a")[0]
            if body and not body.endswith(b"\n"):
                body += b"\r\n"
            r.put(orig, "A:\\AUTOEXEC.ORG", progress=False)
            r.put(body + AUTOEXEC_ADD, "A:\\AUTOEXEC.BAT", progress=False)
            say("AUTOEXEC.BAT: added RX -B19200 -K2 / CTTY AUX (original saved as AUTOEXEC.ORG)")
    if total:
        say("stage3: %d bytes in %.1fs = %.2f KB/s, resends %d%s" % (total, tt, total / 1024.0 / tt, r.resends,
            " (corrupted %d frames on purpose)" % L.corrupted if L.corrupt else ""))
    return r


def default_files():
    """段階 3 で送るもの: files/ にあるファイルを A:\\ に送る (getlha.sh で files/LHA.X を用意できる)"""
    d = os.path.join(HERE, "files")
    out = []
    if os.path.isdir(d):
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if os.path.isfile(p) and not n.startswith("."):
                out.append(p + "=A:\\" + n.upper())
    if not out:
        say("files/ is empty (getlha.sh で LHA.X を用意できる); sending no tools")
    return out


def term(L):
    import termios, tty
    L.text = False
    old = termios.tcgetattr(0)
    tty.setraw(0)
    try:
        while True:
            r, _, _ = select.select([0, L.fd], [], [])
            if 0 in r:
                c = os.read(0, 64)
                if b"\x1d" in c:
                    break
                L.write(c.replace(b"\n", b"\r"))
            if L.fd in r:
                L._fill(0)
                os.write(1, bytes(L.buf).decode("cp932", "replace").encode())
                L.buf.clear()
    finally:
        termios.tcsetattr(0, termios.TCSADRAIN, old)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--tcp")
    ap.add_argument("--baud", type=int, default=int(os.environ.get("SX_BAUD", "9600")))
    ap.add_argument("--speed", type=int, default=19200, help="stage2 で切り替える速さ (0 なら変えない)")
    ap.add_argument("--pace", dest="pace", action="store_true", default=None)
    ap.add_argument("--no-pace", dest="pace", action="store_false")
    ap.add_argument("--rx", default=os.path.join(HERE, "rx.x"))
    ap.add_argument("--line", type=int, default=64, help="stage1 の 1 行のバイト数")
    ap.add_argument("--chunk", type=int, default=2048, help="rx.x に送る 1 回のバイト数 (最大 4096)")
    ap.add_argument("--manual", action="store_true", help="stage1 で COPY AUX / BASIC を X68000 のキーボードで打つ")
    ap.add_argument("--no-autoexec", dest="autoexec", action="store_false")
    ap.add_argument("--verify", action="store_true", help="送ったファイルを読み返して比べる")
    ap.add_argument("--log")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    if not a.port and not a.tcp:
        a.port = os.environ.get("SX_PORT")
        a.tcp = os.environ.get("SX_TCP")
        if not a.port and not a.tcp:
            ap.error("--port か --tcp が要る")
    L = Link(a)
    c = a.cmd
    if c == "probe":
        ok = probe(L)
        print("prompt" if ok else "no prompt")
        sys.exit(0 if ok else 1)
    if c == "term":
        term(L)
        return
    if c in ("stage1", "bootstrap"):
        stage1(L, a)
    if c in ("stage2", "bootstrap"):
        r = stage2(L, a)
        L.baud = a.speed or L.baud
    if c in ("stage3", "bootstrap"):
        a.files = a.args or default_files()
        t0 = time.time()
        stage3(L, a, r if c == "bootstrap" else None)
        say("stage3 done in %.1fs" % (time.time() - t0))
    if c == "bootstrap":
        r.quit()
        say("bootstrap done; rx.x stopped, A> prompt is on the serial line")
        return
    if c in ("stage1", "stage2", "stage3"):
        return
    r, _ = connect_rx(L, a)
    if c == "run":
        rc, out = r.run(" ".join(a.args))
        sys.stdout.write(sjis(out))
        sys.exit(rc if 0 <= rc < 256 else 1)
    elif c == "put":
        local = a.args[0]
        remote = a.args[1] if len(a.args) > 1 else os.path.basename(local).upper()
        append = "--append" in a.args
        data = open(local, "rb").read()
        el = r.put(data, remote, append=append, chunk=a.chunk)
        say("%s: %d bytes %.1fs %.2f KB/s" % (remote, len(data), el, len(data) / 1024.0 / el))
    elif c == "get":
        d = r.get(a.args[0])
        local = a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0].replace("\\", "/"))
        open(local, "wb").write(d)
        say("%s: %d bytes" % (local, len(d)))
    elif c == "speed":
        say(r.speed(int(a.args[0])))
    elif c == "quit":
        r.quit()
    elif c == "hello":
        print(r.hello())
    else:
        ap.error("unknown command %s" % c)


if __name__ == "__main__":
    main()
