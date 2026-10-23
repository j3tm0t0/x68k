#!/usr/bin/env python3
"""rccd.py -- X68000 (Z) の rcc.x から C のソースを受け取り、elf2x68k でコンパイル・リンクして .x を返す

状態を持たない: 1 回の接続 = 1 回のコンパイル。受けたファイルは一時ディレクトリに置き、返したら消す
(後で AWS Lambda などに移すときは handle() をそのまま HTTP の本文の出し入れにつなぐ)。

要求 (Z → ここ)。行は LF、数は 10 進:
  RCC1
  ARG <バイト数>\\n<引数>        gcc に渡す引数 (-O2、-o hello.x、-lm など)。何回でも
  FILE <バイト数> <名前>\\n<中身> ソースとヘッダ。名前はファイル名だけ (ディレクトリは捨てる)
  END
応答 (ここ → Z):
  RCC1 <終了コード> <ログのバイト数> <出力のバイト数>\\n<ログ (gcc の stdout+stderr)><出力の .x>

usage: rccd.py [--port 7790] [--allow 192.168.1.0/24] [--gcc m68k-xelf-gcc] [--timeout 120]
"""
import argparse, ipaddress, os, shutil, socket, socketserver, subprocess, sys, tempfile, time

MAXFILE = 4 << 20       # 1 ファイル 4MB まで
MAXTOTAL = 16 << 20

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=7790)
ap.add_argument("--allow", default="192.168.0.0/16,127.0.0.0/8", help="受け付ける相手 (カンマ区切りの CIDR)")
ap.add_argument("--gcc", default="m68k-xelf-gcc")
ap.add_argument("--cflags", default="-m68000 -O2", help="いつも先に付ける引数")
ap.add_argument("--timeout", type=int, default=120)
args = ap.parse_args()
ALLOW = [ipaddress.ip_network(n.strip()) for n in args.allow.split(",") if n.strip()]


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def handle(req_args, files):
    """1 回のコンパイル。req_args: gcc の引数、files: [(名前, 中身)]。戻り値: (終了コード, ログ, 出力)"""
    out = "a.x"
    a = list(req_args)
    if "-o" in a:
        i = a.index("-o")
        if i + 1 < len(a):
            out = os.path.basename(a[i + 1]) or "a.x"
            a[i + 1] = out
    else:
        a += ["-o", out]
    srcs = [n for n, _ in files if n.lower().endswith((".c", ".s", ".S", ".cc", ".cpp"))]
    with tempfile.TemporaryDirectory(prefix="rccd-") as d:
        for n, body in files:
            with open(os.path.join(d, n), "wb") as f:
                f.write(body)
        cmd = [args.gcc] + args.cflags.split() + [x for x in a if x not in srcs] + srcs
        try:
            p = subprocess.run(cmd, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=args.timeout)
            rc, text = p.returncode, p.stdout
        except subprocess.TimeoutExpired as e:
            rc, text = 124, (e.stdout or b"") + b"rccd: timeout\n"
        except FileNotFoundError:
            rc, text = 127, ("rccd: %s not found\n" % args.gcc).encode()
        body = b""
        op = os.path.join(d, out)
        if rc == 0 and os.path.exists(op):
            with open(op, "rb") as f:
                body = f.read()
        return rc, text, body, " ".join(cmd)


class Handler(socketserver.StreamRequestHandler):
    def readline(self):
        l = self.rfile.readline(512)
        if not l.endswith(b"\n"):
            raise ValueError("short line")
        return l[:-1].decode("ascii", "replace")

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
            if self.readline() != "RCC1":
                raise ValueError("bad magic")
            req_args, files, total = [], [], 0
            while True:
                l = self.readline()
                if l == "END":
                    break
                kw, _, rest = l.partition(" ")
                if kw == "ARG":
                    req_args.append(self.readn(int(rest)).decode("cp932", "replace"))
                elif kw == "FILE":
                    n, _, name = rest.partition(" ")
                    n = int(n)
                    if n > MAXFILE or total + n > MAXTOTAL:
                        raise ValueError("too large")
                    total += n
                    name = os.path.basename(name.replace("\\", "/"))
                    if not name or name.startswith("."):
                        raise ValueError("bad name")
                    files.append((name, self.readn(n)))
                else:
                    raise ValueError("bad keyword " + kw)
        except (ValueError, ConnectionError) as e:
            log("%s: bad request (%s)" % (peer, e))
            return
        t1 = time.time()
        rc, text, body, cmd = handle(req_args, files)
        t2 = time.time()
        self.wfile.write(b"RCC1 %d %d %d\n" % (rc, len(text), len(body)) + text + body)
        log("%s: %d files %d B recv %.2fs, gcc %.2fs rc=%d -> %d B  (%s)" %
            (peer, len(files), total, t1 - t0, t2 - t1, rc, len(body), cmd))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    log("rccd: port %d, gcc %s, allow %s" % (args.port, shutil.which(args.gcc) or args.gcc, args.allow))
    with Server(("0.0.0.0", args.port), Handler) as s:
        s.serve_forever()
