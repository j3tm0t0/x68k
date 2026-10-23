#!/usr/bin/env python3
"""rccd.py -- X68000 (Z) の rcc.x (gcc の前置き) から入力のファイルを受け取り、elf2x68k でコンパイルして結果を返す

状態を持たない: 1 回の接続 = 1 回の gcc。受けたファイルは一時ディレクトリに置き、返したら消す。
handle() は (引数, ファイル) -> (終了コード, 標準出力, メッセージ, 出力ファイル) の関数なので、HTTP (AWS Lambda など) や
Pico の受け手 (ID 6 の記録) に移すときも、同じ中身を運ぶだけでよい。形式は README.md の「やり取り」。

Z のパスの扱い: 相対パスは一時ディレクトリの中の作業ディレクトリ (W) から、ドライブ付き ("A:\\inc\\x.h") は
W の外の "_A_/inc/x.h" に置く。gcc の引数も同じように書き換え、メッセージの中は Z の書き方に戻す。

usage: rccd.py [--port 7790] [--allow 192.168.0.0/16,127.0.0.0/8] [--gcc m68k-xelf-gcc] [--cflags "-m68000"] [--timeout 120]
"""
import argparse, ipaddress, os, re, shutil, socket, socketserver, subprocess, sys, tempfile, time

MAXFILE = 4 << 20       # 1 ファイル 4MB まで
MAXTOTAL = 16 << 20
DEPTH = 4               # W の深さ ("../" を 4 段まで受ける)

# Z の gcc (真里子版 + LIBC) だけのライブラリ: elf2x68k では libc に入っている / 名前が違う
LIBMAP = {"-lgcc2": None, "-ldos": None, "-liocs": None, "-lnetwork": "-lsocket", "-lsignal": None}

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=7790)
ap.add_argument("--allow", default="192.168.0.0/16,127.0.0.0/8", help="受け付ける相手 (カンマ区切りの CIDR)")
ap.add_argument("--gcc", default="m68k-xelf-gcc")
ap.add_argument("--cflags", default="-m68000", help="いつも先に付ける引数")
ap.add_argument("--timeout", type=int, default=120)
args = ap.parse_args()
ALLOW = [ipaddress.ip_network(n.strip()) for n in args.allow.split(",") if n.strip()]


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


class BadPath(ValueError):
    pass


def local_path(zpath):
    """Z のパス -> 一時ディレクトリの中の相対パス (W からの相対で返す)"""
    p = zpath.replace("\\", "/")
    m = re.match(r"^([A-Za-z]):/?(.*)$", p)
    if m:
        rel = "../" * DEPTH + "_%s_/" % m.group(1).upper() + m.group(2)
    elif p.startswith("/"):
        rel = "../" * DEPTH + "_ROOT_" + p
    else:
        rel = p
    if rel.endswith(".C"):
        rel = rel[:-2] + ".c"               # Human68k の名前は大文字。gcc は .C を C++ と見る
    norm = os.path.normpath(os.path.join("w/" * DEPTH, rel))
    if norm.startswith("..") or os.path.isabs(norm):
        raise BadPath(zpath)
    return rel


def zspell(text, top, names=()):
    """gcc のメッセージの中の一時ディレクトリのパスを Z の書き方に戻す。names: [(gcc に渡したパス, Z で書いたパス)]"""
    for lp, zp in sorted(names, key=lambda x: -len(x[0])):
        if lp != zp:
            text = text.replace(lp + ":", zp + ":")
    w = os.path.join(top, *(["w"] * DEPTH))
    text = text.replace(w + "/", "")
    text = re.sub(re.escape(top) + r"/_([A-Z])_/", lambda m: m.group(1) + ":/", text)
    text = re.sub(r"(\.\./){%d}_([A-Z])_/" % DEPTH, lambda m: m.group(2) + ":/", text)
    return text.replace(top + "/_ROOT_", "")


def outputs_expected(a):
    """gcc の引数から、作られる出力 (Z のパス) を決める。Z の gcc と同じ: -o が無ければ a.x、-c なら元の名前の .o"""
    out, mode, srcs = None, "link", []
    i = 0
    while i < len(a):
        x = a[i]
        if x == "-o" and i + 1 < len(a):
            out = a[i + 1]
            i += 2
            continue
        if x in ("-c", "-S", "-E"):
            mode = x
        elif x in ("-I", "-D", "-U", "-L", "-include", "-x", "-isystem"):
            i += 2
            continue
        elif not x.startswith("-") and re.search(r"\.(c|cc|cpp|s)$", x, re.I):
            srcs.append(x)
        i += 1
    if mode == "-E":
        return [out] if out else []
    if out:
        return [out]
    if mode in ("-c", "-S"):
        ext = ".o" if mode == "-c" else ".s"
        return [re.sub(r"\.[^./\\]*$", ext, re.split(r"[/\\:]", s)[-1]) for s in srcs]
    return ["a.x"]


def handle(zargs, files):
    """1 回の gcc。zargs: Z で打った引数、files: [(Z のパス, 中身)]。戻り値: (終了コード, 標準出力, メッセージ, [(Z のパス, 中身)], コマンド)"""
    with tempfile.TemporaryDirectory(prefix="rccd-") as top:
        w = os.path.join(top, *(["w"] * DEPTH))
        os.makedirs(w)
        for zp, body in files:
            lp = os.path.join(w, local_path(zp))
            os.makedirs(os.path.dirname(lp), exist_ok=True)
            with open(lp, "wb") as f:
                f.write(body)
        cmd, i, names = [args.gcc] + args.cflags.split(), 0, []
        while i < len(zargs):
            x = zargs[i]
            if x in LIBMAP:
                if LIBMAP[x]:
                    cmd.append(LIBMAP[x])
            elif x in ("-o", "-I", "-L", "-include") and i + 1 < len(zargs):
                if x != "-L":           # Z のライブラリのディレクトリは使わない
                    cmd += [x, local_path(zargs[i + 1])]
                i += 2
                continue
            elif x.startswith("-I") and len(x) > 2:
                cmd.append("-I" + local_path(x[2:]))
            elif x.startswith("-L"):
                pass
            elif not x.startswith("-"):
                cmd.append(local_path(x))
                names.append((local_path(x), x))
            else:
                cmd.append(x)
            i += 1
        try:
            p = subprocess.run(cmd, cwd=w, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            rc, out, err = 124, e.stdout or b"", (e.stderr or b"") + b"rcc: the compiler timed out\n"
        except FileNotFoundError:
            rc, out, err = 127, b"", ("rcc: %s not found on the server\n" % args.gcc).encode()
        err = zspell(err.decode("utf-8", "replace"), top, names).encode("utf-8")
        outs = []
        if rc == 0:
            for zp in outputs_expected(zargs):
                lp = os.path.join(w, local_path(zp))
                if os.path.exists(lp):
                    with open(lp, "rb") as f:
                        outs.append((zp, f.read()))
        return rc, out, err, outs, " ".join(cmd)


class Handler(socketserver.StreamRequestHandler):
    def readline(self):
        l = self.rfile.readline(1024)
        if not l.endswith(b"\n"):
            raise ValueError("short line")
        return l[:-1].decode("cp932", "replace")

    def readn(self, n):
        b = self.rfile.read(n)
        if len(b) != n:
            raise ValueError("short body")
        return b

    def handle(self):
        peer = ipaddress.ip_address(self.client_address[0])
        if not any(peer in n for n in ALLOW):
            log("refused %s" % peer)
            return
        t0 = time.time()
        try:
            if self.readline() != "RCC2":
                raise ValueError("bad magic")
            zargs, files, total = [], [], 0
            while True:
                l = self.readline()
                if l == "END":
                    break
                kw, _, rest = l.partition(" ")
                if kw == "ARG":
                    zargs.append(self.readn(int(rest)).decode("cp932", "replace"))
                elif kw == "CWD":
                    self.readn(int(rest))
                elif kw == "FILE":
                    n, _, name = rest.partition(" ")
                    n = int(n)
                    if n > MAXFILE or total + n > MAXTOTAL:
                        raise ValueError("too large")
                    total += n
                    local_path(name)        # 外に出るパスはここで断る
                    files.append((name, self.readn(n)))
                else:
                    raise ValueError("bad keyword " + kw)
        except (ValueError, ConnectionError) as e:
            log("%s: bad request (%s)" % (peer, e))
            try:
                msg = ("rcc: bad request (%s)\n" % e).encode()
                self.wfile.write(b"RCC2 126 0 %d 0\n" % len(msg) + msg)
            except OSError:
                pass
            return
        t1 = time.time()
        rc, out, err, outs, cmd = handle(zargs, files)
        t2 = time.time()
        b = b"RCC2 %d %d %d %d\n" % (rc, len(out), len(err), len(outs)) + out + err
        for zp, body in outs:
            b += b"OUT %d %s\n" % (len(body), zp.encode("cp932", "replace")) + body
        self.wfile.write(b)
        log("%s: %d files %d B recv %.2fs, gcc %.2fs rc=%d -> %s  (%s)" %
            (peer, len(files), total, t1 - t0, t2 - t1, rc, ", ".join("%s %d B" % (z, len(x)) for z, x in outs) or "-", cmd))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    log("rccd: port %d, gcc %s, allow %s" % (args.port, shutil.which(args.gcc) or args.gcc, args.allow))
    with Server(("0.0.0.0", args.port), Handler) as s:
        s.serve_forever()
