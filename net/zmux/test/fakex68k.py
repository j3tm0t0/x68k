#!/usr/bin/env python3
"""XEiJ の代わりの「にせの X68000」。zmux の線 (net/zmux/DESIGN.md §1) を TCP で話す (XEiJ と同じく、こちらがサーバー)。
zmux.sys / zmux.x / npd.x -m / zos.x / zrmt.sys の代わりに:
  - チャネル 15: 5 秒ごとに HELLO、HELLO_ACK で UP。KICK で 0xA7 の交換をする
  - チャネル 0: COMMAND.X の CTTY AUX のふり (プロンプト "A>"、ver / dir / echo、打った字をそのまま返す、Shift_JIS)
  - チャネル 1: zos.x の 0xA7 (見えないペインの zosh) のふり: OPEN → ACCEPT、INPUT → そのまま OUTPUT に返す、"exit N" → EXIT
  - チャネル 2: zrmt.sys のふり。rd_xchg(要求) で 1 つ送って応答を待つ
  - チャネル 3: 注入されたキーを記録する (keys)
線の速さの真似: baud を与えると、1 バイト 10 ビット分の時間をかけて送る (受けも同じ時間で遅らせる)。
e2e_fake.py から import して使う。単体でも動く: fakex68k.py [PORT] [BAUD]"""
import socket, struct, sys, threading, time, collections

DLE, STX, ETX = 0x10, 0x02, 0x03
MAXLEN, WIN = 1024, 4
F_NAK, F_BUSY, F_UNSEQ, F_END = 1, 2, 4, 8
CH_CON, CH_NP, CH_RD, CH_KEY, CH_CTL = 0, 1, 2, 3, 15
OP_HELLO, OP_HELLO_ACK, OP_BYE, OP_KICK = 1, 2, 3, 4


def crc16(data, c=0xffff):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c


def encode(chan, seq, ack, flags, data):
    h = bytes([chan, seq, ack, flags, len(data) >> 8, len(data) & 255])
    c = crc16(data, crc16(h))
    body = h + data + bytes([c >> 8, c & 255])
    out = bytearray([DLE, STX])
    for b in body:
        if b == DLE:
            out.append(DLE)
        out.append(b)
    out += bytes([DLE, ETX])
    return bytes(out)


class Fake:
    def __init__(self, port=54368, baud=0, verbose=False):
        self.port, self.baud, self.verbose = port, baud, verbose
        self.ls = socket.socket()
        self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", port))
        self.ls.listen(1)
        self.conn = None
        self.lock = threading.RLock()
        self.up = False
        self.tx_seq = self.tx_base = self.rx_expect = 0
        self.win = collections.deque()          # (chan, flags, data)
        self.queue = collections.deque()        # (chan, data, end)
        self.t_sent = time.time()
        self.t_hello = 0
        self.t_last_rx = time.time()
        self.t_last_tx = time.time()
        self.need_ack = False
        self.nak = False
        self.con_in = bytearray()               # チャネル 0 で届いた字 (全部)
        self.con_line = bytearray()
        self.keys = []                          # チャネル 3 で届いたキー
        self.kick = False
        self.rd_res = None
        self.rd_buf = bytearray()
        self.rd_event = threading.Event()
        self.npblk = bytearray()
        self.zseq = 1
        self.zepoch = 0x5a5a1234
        self.sess = {}                          # i -> dict(gen, win, out queue)
        self.stats = {"rx": 0, "tx": 0, "resent": 0, "bad": 0}
        self.ups = 0
        self.stop = False
        self.sent_bytes = 0
        self.recv_bytes = 0
        threading.Thread(target=self._serve, daemon=True).start()

    def log(self, *a):
        if self.verbose:
            print("fake:", *a, file=sys.stderr, flush=True)

    # ---------------------------------------------------------------- 線
    def _serve(self):
        while not self.stop:
            c, _ = self.ls.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with self.lock:
                self.conn = c
                self.t_hello = 0
            threading.Thread(target=self._reader, args=(c,), daemon=True).start()
            self._run(c)

    def _reader(self, c):
        state, buf = 0, bytearray()
        while True:
            try:
                d = c.recv(4096)
            except OSError:
                d = b""
            if not d:
                with self.lock:
                    if self.conn is c:
                        self.conn = None
                        self.up = False
                return
            self.recv_bytes += len(d)
            if self.baud:
                time.sleep(len(d) * 10.0 / self.baud)
            for b in d:
                if state == 0:
                    if b == DLE: state = 1
                elif state == 1:
                    if b == STX: buf = bytearray(); state = 2
                    else: state = 0
                elif state == 2:
                    if b == DLE: state = 3
                    else: buf.append(b)
                else:
                    if b == DLE: buf.append(DLE); state = 2
                    elif b == STX: buf = bytearray(); state = 2
                    elif b == ETX:
                        state = 0
                        self._frame(bytes(buf))
                    else:
                        state = 0
                        self.stats["bad"] += 1

    def _frame(self, f):
        if len(f) < 8:
            self.stats["bad"] += 1; return
        ln = (f[4] << 8) | f[5]
        if ln != len(f) - 8 or crc16(f[:-2]) != ((f[-2] << 8) | f[-1]):
            self.stats["bad"] += 1
            with self.lock:
                if self.up: self.nak = True
            return
        chan, seq, ack, flags = f[0] & 15, f[1], f[2], f[3]
        data = f[6:6 + ln]
        self.stats["rx"] += 1
        with self.lock:
            if self.up:
                self.t_last_rx = time.time()
            if flags & F_UNSEQ:
                if chan == CH_CTL and data:
                    if data[0] == OP_HELLO_ACK:
                        if not self.up:
                            self._go_up()
                    elif data[0] == OP_HELLO:
                        self._go_up()
                        self._emit(CH_CTL, 0, F_UNSEQ, bytes([OP_HELLO_ACK, 1, 4, 0]))
                    elif data[0] == OP_BYE:
                        self.up = False
                    elif data[0] == OP_KICK:
                        self.kick = True
                return
            if not self.up:
                return
            n = (ack - self.tx_base + 1) & 255
            if n <= len(self.win):
                for _ in range(n):
                    self.win.popleft()
                self.tx_base = (self.tx_base + n) & 255
                if n: self.t_sent = time.time()
            if flags & F_NAK and self.win:
                self.t_sent = 0
            if ln == 0:
                return
            if seq != self.rx_expect:
                self.nak = True
                return
            self.rx_expect = (self.rx_expect + 1) & 255
            self.need_ack = True
        self._data(chan, data, flags)

    def _go_up(self):
        self.up = True
        self.ups += 1
        self.tx_seq = self.tx_base = self.rx_expect = 0
        self.win.clear()
        self.need_ack = self.nak = False
        self.t_last_rx = time.time()
        self.log("up")

    def _emit(self, chan, seq, flags, data):
        c = self.conn
        if not c:
            return
        b = encode(chan, seq, (self.rx_expect - 1) & 255, flags, data)
        try:
            c.sendall(b)
        except OSError:
            return
        self.sent_bytes += len(b)
        self.stats["tx"] += 1
        self.need_ack = False
        self.t_last_tx = time.time()
        if self.baud:
            time.sleep(len(b) * 10.0 / self.baud)

    def send(self, chan, data, end=False):
        with self.lock:
            self.queue.append((chan, bytes(data), end))

    def _run(self, c):
        while self.conn is c:
            with self.lock:
                now = time.time()
                if self.up and now - self.t_last_rx > 10:
                    self.up = False
                    self.log("down (no frames for 10s)")
                if not self.up and now - self.t_hello > 5:
                    self._emit(CH_CTL, 0, F_UNSEQ, bytes([OP_HELLO, 1, 4, 0]))
                    self.t_hello = now
                if self.up:
                    if self.win and now - self.t_sent > 1.0:
                        s = self.tx_base
                        for ch, fl, d in list(self.win):
                            self._emit(ch, s, fl, d)
                            s = (s + 1) & 255
                            self.stats["resent"] += 1
                        self.t_sent = time.time()
                    while len(self.win) < WIN and self.queue:
                        d = bytearray(); fl = 0; ch = self.queue[0][0]
                        while self.queue and self.queue[0][0] == ch and len(d) < MAXLEN:
                            c0, b, end = self.queue[0]
                            take = min(MAXLEN - len(d), len(b))
                            d += b[:take]
                            if take == len(b):
                                self.queue.popleft()
                                if end: fl = F_END; break
                            else:
                                self.queue[0] = (c0, b[take:], end)
                        if not self.win: self.t_sent = time.time()
                        self._emit(ch, self.tx_seq, fl, bytes(d))
                        self.win.append((ch, fl, bytes(d)))
                        self.tx_seq = (self.tx_seq + 1) & 255
                    if self.need_ack or self.nak or now - self.t_last_tx > 2:
                        self._emit(CH_CON, 0, F_NAK if self.nak else 0, b"")
                        self.nak = False
                    if self.kick:
                        self.kick = False
                        self._zss_exchange_send()
            time.sleep(0.005)

    # ---------------------------------------------------------------- チャネル
    def _data(self, chan, data, flags):
        if chan == CH_CON:
            self.con_in += data
            for b in data:
                self._con_key(b)
        elif chan == CH_KEY:
            for i in range(0, len(data) - 1, 2):
                self.keys.append((data[i] << 8) | data[i + 1])
        elif chan == CH_RD:
            self.rd_buf += data
            if flags & F_END:
                self.rd_res = bytes(self.rd_buf)
                self.rd_buf = bytearray()
                self.rd_event.set()
        elif chan == CH_NP:
            self.npblk += data
            if flags & F_END:
                self._zss_response(bytes(self.npblk))
                self.npblk = bytearray()

    # コンソール: COMMAND.X のふり
    def _con_key(self, b):
        if b == 13:
            self.send(CH_CON, b"\r\n")
            line = bytes(self.con_line).decode("cp932", "replace").strip()
            self.con_line = bytearray()
            self._command(line)
        elif b == 8 or b == 0x7f:
            if self.con_line:
                self.con_line.pop()
                self.send(CH_CON, b"\x08 \x08")
        elif b == 3:
            self.con_line = bytearray()
            self.send(CH_CON, b"^C\r\nA>")
        elif b >= 0x20 or b >= 0x80:
            self.con_line.append(b)
            self.send(CH_CON, bytes([b]))

    def _command(self, line):
        out = ""
        if line == "":
            pass
        elif line.lower() == "ver":
            out = "Human68k version 3.02 (fake X68000 for zmux tests)\r\n"
        elif line.lower().startswith("echo "):
            out = line[5:] + "\r\n"
        elif line.lower().startswith("dir"):
            out = " ドライブA: のボリュームラベルはありません\r\nHUMAN   .SYS     58000 26-10-25 01:00\r\nCOMMAND .X       28000 26-10-25 01:00\r\n  2 個\r\n"
        else:
            out = "コマンドまたはファイル名が違います\r\n"
        self.send(CH_CON, out.encode("cp932") + b"A>")

    def prompt(self):
        self.send(CH_CON, b"A>")

    # リモートドライブ: zrmt.sys のふり (要求 1 つ → 応答 1 つ)
    def rd_xchg(self, cmd, timeout=10.0):
        self.rd_event.clear()
        self.rd_res = None
        self.send(CH_RD, cmd, end=True)
        if not self.rd_event.wait(timeout):
            raise TimeoutError("no remote drive response")
        return self.rd_res

    # 0xA7 (zos.x のふり)
    def _zss_exchange_send(self):
        """KICK が来た (か自分から) → X6NR の要求に 0xA7 の 1 レコード (こちらからのメッセージ) を載せて送る"""
        msgs = bytearray()
        for i, s in self.sess.items():
            while s["out"]:
                b = s["out"].popleft()
                msgs += bytes([b[0], i, s["gen"], 0, len(b[1]) >> 8, len(b[1]) & 255]) + b[1] + (b"\0" if len(b[1]) & 1 else b"")
        for i in [i for i, s in self.sess.items() if s.get("dead")]:
            del self.sess[i]
        rec = bytes([0xa7, 0, 0, 0, self.zseq >> 8, self.zseq & 255, 0, 0]) + struct.pack(">I", self.zepoch) + bytes(msgs)
        self.zseq = (self.zseq + 1) & 0xffff
        body = struct.pack(">H", len(rec)) + rec + (b"\0" if len(rec) & 1 else b"")
        blk = b"X6NR" + struct.pack(">IHHI", self.zseq, len(body), 4080, 0x7f000001) + body
        self.queue.append((CH_NP, blk, True))

    def _zss_response(self, blk):
        if blk[:4] != b"X6NR":
            return
        n = struct.unpack(">H", blk[8:10])[0]
        more = struct.unpack(">H", blk[10:12])[0]
        p = 16
        end = 16 + n
        while p + 16 <= end:
            ln = struct.unpack(">H", blk[p:p + 2])[0]
            rec = blk[p + 16:p + 16 + ln]
            p += 16 + ((ln + 1) & ~1)
            if not rec or rec[0] != 0xa7 or len(rec) < 12:
                continue
            q = 12
            while q + 6 <= len(rec):
                t, i, gen = rec[q], rec[q + 1], rec[q + 2]
                ml = struct.unpack(">H", rec[q + 4:q + 6])[0]
                body = rec[q + 6:q + 6 + ml]
                q += 6 + ((ml + 1) & ~1)
                self._zss_msg(t, i, gen, body)
        with self.lock:
            if more or any(s["out"] for s in self.sess.values()):
                self.kick = True

    def _zss_msg(self, t, i, gen, body):
        E_OPEN, E_INPUT, E_WINCH, E_SIGNAL, E_EOF, E_CLOSED, E_CREDIT = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
        if t == E_OPEN:
            s = self.sess[i] = {"gen": gen, "out": collections.deque(), "line": bytearray(), "cols": struct.unpack(">H", body[:2])[0]}
            s["out"].append((0x01, struct.pack(">I", 4096)))                   # ACCEPT, window
            s["out"].append((0x03, ("zosh (fake) session %d\r\nz$ " % (i + 1)).encode()))   # OUTPUT
            self.log("zss open", i, gen)
            return
        s = self.sess.get(i)
        if not s or s["gen"] != gen:
            return
        if t == E_INPUT:
            for b in body:
                if b == 13:
                    line = bytes(s["line"]).decode("utf-8", "replace").strip()
                    s["line"] = bytearray()
                    if line.startswith("exit"):
                        code = int(line[4:] or 0)
                        s["out"].append((0x05, struct.pack(">I", code)))             # EXIT (送ったあとで消す)
                        s["dead"] = True
                        return
                    s["out"].append((0x03, ("\r\n" + (line[5:] if line.startswith("echo ") else "zosh: %s: not found" % line if line else "") + "\r\nz$ ").encode()))
                else:
                    s["line"].append(b)
                    s["out"].append((0x03, bytes([b])))
        elif t == E_WINCH:
            s["cols"] = struct.unpack(">H", body[:2])[0]
        elif t in (E_EOF, E_CLOSED):
            s["out"].append((0x05, struct.pack(">I", 0)))
            s["dead"] = True
        elif t == E_CREDIT:
            pass

    def zss_announce(self):
        """zos.x が動いていることを zmuxd に知らせる (0xA7 の要求を 1 回送る)"""
        with self.lock:
            self._zss_exchange_send()

    def close(self):
        self.stop = True
        try:
            if self.conn: self.conn.close()
            self.ls.close()
        except OSError:
            pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 54368
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    f = Fake(port, baud, verbose=True)
    print("fakex68k: listening on 127.0.0.1:%d (baud %s)" % (port, baud or "unlimited"))
    while True:
        time.sleep(1)
