#!/usr/bin/env python3
"""rccd_test.py -- rccd.py をホストで試す (elf2x68k の m68k-xelf-gcc が要る)。Z の rcc.x の代わりに同じ形式で頼む"""
import os, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 7799
srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "..", "rccd.py"), "--port", str(PORT), "--allow", "127.0.0.0/8"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1)
fails = 0


def check(name, cond):
    global fails
    print(("PASS " if cond else "FAIL ") + name)
    fails += 0 if cond else 1


def req(zargs, files, cwd="A:\\WORK", sys_=None):
    s = socket.create_connection(("127.0.0.1", PORT))
    b = b"RCC2\nCWD %d\n" % len(cwd) + cwd.encode()
    if sys_:
        b += b"SYS %s\n" % sys_.encode()
    for a in zargs:
        b += b"ARG %d\n" % len(a.encode()) + a.encode()
    for n, d in files:
        b += b"FILE %d %s\n" % (len(d), n.encode()) + d
    s.sendall(b + b"END\n")
    f = s.makefile("rb")
    h = f.readline().split()
    rc, ol, el, no = map(int, h[1:])
    out, err = f.read(ol), f.read(el)
    outs = {}
    for _ in range(no):
        k = f.readline().decode().split(" ", 2)
        outs[k[2].rstrip("\n")] = f.read(int(k[1]))
    return rc, out, err.decode(), outs


hello = b'#include <stdio.h>\r\n#include "msg.h"\r\nint main(void) { printf("%s\\n", MSG); return 0; }\r\n'
msg = b'#define MSG "hi"\r\n'
rc, out, err, outs = req(["-O", "hello.c", "-o", "hello.x"], [("hello.c", hello), ("msg.h", msg)])
check("link: hello.x comes back under the name given by -o", rc == 0 and list(outs) == ["hello.x"] and outs["hello.x"][:2] == b"HU")
rc, out, err, outs = req(["hello.c"], [("hello.c", hello), ("msg.h", msg)])
check("no -o: a.x like the Z's gcc", rc == 0 and list(outs) == ["a.x"])
rc, out, err, outs = req(["-c", "hello.c", "B:\\lib\\util.c"], [("hello.c", hello), ("msg.h", msg), ("B:\\lib\\util.c", b"int u(void){return 1;}\r\n")])
check("-c: one .o per source, in the current directory", rc == 0 and sorted(outs) == ["hello.o", "util.o"])
rc, out, err, outs = req(["-c", "A:\\SUB\\T2.C", "-IA:\\SUB"], [("A:\\SUB\\T2.C", b'#include "t2.h"\r\nint t2(void){return T2;}\r\n'), ("A:\\SUB\\t2.h", b"#define T2 2\r\n")])
check("drive paths and -I with a drive", rc == 0 and list(outs) == ["T2.o"])
rc, out, err, outs = req(["bad.c", "-o", "bad.x"], [("bad.c", b"int main(void) { return syntax error; }\r\n")])
check("error: no output, message names bad.c:1 as the Z wrote it", rc != 0 and not outs and "bad.c:1:" in err)
rc, out, err, outs = req(["-c", "A:\\SUB\\E.C"], [("A:\\SUB\\E.C", b"int f(void) { return x; }\r\n")])
check("error in a drive path: message names A:\\SUB\\E.C:1 as the Z wrote it", rc != 0 and "A:\\SUB\\E.C:1:" in err)
rc, out, err, outs = req(["-E", "-DX=42", "e.c"], [("e.c", b"int v = X;\r\n")])
check("-E: preprocessed text on stdout", rc == 0 and b"int v = 42;" in out)
rc, out, err, outs = req(["hello.c", "-o", "h2.x", "-lgcc2", "-ldos", "-liocs"], [("hello.c", hello), ("msg.h", msg)])
check("the Z gcc's -lgcc2 -ldos -liocs are dropped", rc == 0 and "h2.x" in outs)
rc, out, err, outs = req(["../../../../../etc/passwd"], [("../../../../../../x.c", b"")])
check("a path that leaves the work area is refused", rc == 126)
rc, out, err, outs = req(["hello.c"], [("hello.c", hello), ("msg.h", msg)], sys_="elf2x68k-newlib")
check("SYS naming this server's header set is accepted", rc == 0 and list(outs) == ["a.x"])
rc, out, err, outs = req(["hello.c"], [("hello.c", hello), ("msg.h", msg)], sys_="vbcc-newlib")
check("SYS naming another header set is refused", rc == 126 and "vbcc-newlib" in err and not outs)
srv.terminate()
print("%d failure(s)" % fails)
sys.exit(1 if fails else 0)
