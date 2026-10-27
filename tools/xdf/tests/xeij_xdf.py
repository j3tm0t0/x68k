#!/usr/bin/env python3
"""xdfwrite.x / xdfread.x を XEiJ (Compact、主記憶 2MB、フロッピー起動) で試す。
usage: xeij_xdf.py RUNDIR

ドライブの並び (実機の Compact XVI と同じく A: と B: がフロッピー):
  A: (装置 0)  起動ディスク。HUMAN302.XDF に xdfwrite.x / xdfread.x と、試すコマンドを足した AUTOEXEC.BAT
  B: (装置 1)  書き込み先。試験ごとに 空 (0 埋め) / HUMAN302 / DOS/V の 1.44MB / 書き込み禁止 / 無し
  C: (装置 2)  2HQ (1440KB) に乱数の XDF (R.XDF) を入れたもの。XVI ではリモートドライブの C: にあたる
  D: (装置 3)  空の 2HQ。xdfread の書き出し先
試験が終わったら XEiJ の FDC.fdcTini を呼んでイメージをファイルに書き戻させ、ホストで中身を比べる。"""
import os, random, shutil, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
xdf = os.path.dirname(here)
top = os.path.abspath(os.path.join(xdf, "../.."))
tools = os.environ.get("XEIJ_TOOLS", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools")
HUMAN = os.path.join(tools, "xeij/HUMAN302.XDF")
SIZE = 1261568
run = os.path.abspath(sys.argv[1]) if __name__ == "__main__" else None
py = [sys.executable, "-I"]


def fs(*a, **kw):
    return subprocess.run(py + [os.path.join(xdf, "xdffs.py")] + list(a), check=True, **kw)


def mk2hq(out, files):
    d = os.path.join(run, "2hq-src")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for name, src in files.items():
        shutil.copy(src, os.path.join(d, name))
    subprocess.run(py + [os.path.join(here, "mk2hq.py"), out, d], check=True, stdout=subprocess.DEVNULL)


def get(img, path):
    d = os.path.join(run, "get")
    shutil.rmtree(d, ignore_errors=True)
    fs("extract", img, d)
    p = os.path.join(d, path)
    return open(p, "rb").read() if os.path.exists(p) else None


def agent(line):
    tmp = os.path.join(run, "agent.cmd.tmp")
    open(tmp, "w").write(line + "\n")
    os.rename(tmp, os.path.join(run, "agent.cmd"))


def xeij(name, fds, cmds, timeout=900):
    """fds: {装置番号: パス}。cmds を AUTOEXEC.BAT の最後に足して起動し、TESTDONE が出たら画面の文字を返す。
    環境変数 ONLY (試験の名前をコンマ区切り) にない試験は飛ばし、その確認も出さない"""
    global skipping
    skipping = bool(os.environ.get("ONLY")) and name not in os.environ["ONLY"].split(",")
    if skipping:
        return ""
    for f in ("cgrom.bin", "agent.cmd", "XEiJControl"):
        if os.path.exists(os.path.join(run, f)): os.remove(os.path.join(run, f))
    add = os.path.join(run, "boot-add")
    shutil.rmtree(add, ignore_errors=True)
    os.makedirs(add)
    for x in ("xdfwrite.x", "xdfread.x"):
        shutil.copy(os.path.join(xdf, x), os.path.join(add, x.upper()))
    body = get(HUMAN, "AUTOEXEC.BAT").split(b"\x1a")[0]
    body += b"".join(("ECHO --- %s\r\n%s\r\n" % (c, c)).encode("cp932") for c in cmds) + b"ECHO TESTDONE\r\n"
    open(os.path.join(add, "AUTOEXEC.BAT"), "wb").write(body)
    fd0 = os.path.join(run, "fd0.xdf")
    fs("build", fd0, HUMAN, "--add", add, stdout=subprocess.DEVNULL)
    java = "/opt/homebrew/opt/openjdk/bin/java"
    if not os.path.exists(java): java = "java"
    args = [java, "-javaagent:%s/xeijagent.jar=%s/agent.cmd" % (tools, run), "-Djava.io.tmpdir=" + run,
            "-jar", os.path.join(tools, "xeij/XEiJ.jar"), "-config=default", "-model=Compact", "-memory=2",
            "-boot=fd0", "-fd0=" + fd0, "-flrinformed=on", "-usecanvas=off", "-pastepipe=on"]
    args += ["-fd%d=%s" % (u, p) for u, p in fds.items()]
    log = os.path.join(run, "xeij-%s.log" % name)
    xe = subprocess.Popen(args, cwd=run, stdin=subprocess.DEVNULL, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    s = ""
    try:
        while time.time() < t0 + timeout:
            time.sleep(2)
            if xe.poll() is not None: sys.exit("xeij_xdf: XEiJ exited (%s)" % log)
            s = subprocess.run(py[:1] + [os.path.join(top, "boot/serial/test/xscreen.py"), run],
                               capture_output=True, text=True).stdout
            if "TESTDONE" in s: break
        else:
            print(s); sys.exit("xeij_xdf: %s: timeout" % name)
        agent("call xeij.FDC fdcTini")       # イメージをファイルに書き戻す
        for _ in range(50):
            time.sleep(0.2)
            if "fdcTini done" in open(log, errors="replace").read(): break
        else:
            sys.exit("xeij_xdf: fdcTini did not finish (%s)" % log)
    finally:
        xe.terminate()
        try: xe.wait(10)
        except subprocess.TimeoutExpired: xe.kill()
    s = s[s.find("ECHO OFF"):]
    print("== %s (%.0fs)\n%s" % (name, time.time() - t0, s.rstrip()))
    return s


fails = []
skipping = False
def check(name, ok):
    if skipping: return
    print("%s %s" % ("PASS" if ok else "FAIL", name))
    if not ok: fails.append(name)



def main():
    os.makedirs(run, exist_ok=True)
    rnd = os.path.join(run, "R.XDF")
    random.seed(26)
    open(rnd, "wb").write(bytes(random.getrandbits(8) for _ in range(SIZE)))
    R = open(rnd, "rb").read()
    small = os.path.join(run, "SMALL.XDF")
    open(small, "wb").write(R[:4096])
    c2hq, d2hq, fd1 = (os.path.join(run, n) for n in ("c.2hq", "d.2hq", "fd1.xdf"))

    # 1. 断る場合 (画面は 31 行なので、試験を分けてスクロールで消えないようにする)
    mk2hq(c2hq, {"R.XDF": rnd, "SMALL.XDF": small}); mk2hq(d2hq, {})
    open(fd1, "wb").write(bytes(SIZE))
    s = xeij("refuse", {1: fd1, 2: c2hq, 3: d2hq},
             ["xdfwrite -s b:", "xdfwrite c:\\R.XDF a:", "xdfwrite c:\\SMALL.XDF b:", "xdfread b: c:\\R.XDF"])
    check("status shows inserted", "(inserted)" in s)
    check("A: refused without --force", "drive A: is the boot disk" in s)
    check("wrong size refused", "SMALL.XDF is 4096 bytes" in s)
    check("xdfread does not overwrite", "R.XDF already exists" in s)
    check("B: untouched", open(fd1, "rb").read() == bytes(SIZE))

    # 2. 0 埋めのディスクに -f (初期化してから書く)、読み返しを D: に
    s = xeij("format", {1: fd1, 2: c2hq, 3: d2hq}, ["xdfwrite -f c:\\R.XDF b:", "xdfread b: d:\\BACK.XDF"])
    check("format + write + verify OK", "OK: 1261568 bytes written and verified" in s)
    check("xdfread OK", "OK: 1261568 bytes\n" in s)
    check("B: image == R.XDF", open(fd1, "rb").read() == R)
    check("D:\\BACK.XDF == R.XDF", get(d2hq, "BACK.XDF") == R)

    # 3. 中身のあるディスクに (初期化なし) 書く
    shutil.copy(HUMAN, fd1); os.chmod(fd1, 0o644)
    mk2hq(d2hq, {})
    s = xeij("overwrite", {1: fd1, 2: c2hq, 3: d2hq}, ["xdfwrite -v c:\\R.XDF b:"])
    check("overwrite OK", "OK: 1261568 bytes written and verified" in s)
    check("B: image == R.XDF (overwrite)", open(fd1, "rb").read() == R)
    check("40 cylinders buffered in 2MB", "buffer: 40 cylinders" in s)

    # 3a. 途中から続きを書く (-c)。C0-C39 だけ書いてあるディスクに -c 40
    open(fd1, "wb").write(R[:40 * 16384] + bytes(SIZE - 40 * 16384))
    mk2hq(d2hq, {})
    s = xeij("resume", {1: fd1, 2: c2hq, 3: d2hq}, ["xdfwrite -c 40 c:\\R.XDF b:"])
    check("resume OK", "OK: 1261568 bytes written and verified" in s)
    check("B: image == R.XDF (resume)", open(fd1, "rb").read() == R)

    # 3b. DOS/V の 1.44MB (512 バイト x 18 セクタ) のディスクに -f 無しで書く (自動で 2HD に初期化する)
    mk2hq(fd1, {})
    mk2hq(d2hq, {})
    s = xeij("dosv", {1: fd1, 2: c2hq, 3: d2hq}, ["xdfwrite c:\\R.XDF b:", "xdfread b: d:\\DOSV.XDF"])
    check("DOS/V disk detected", "B: is not X68000 2HD" in s)
    check("DOS/V disk: format + write + verify OK", "OK: 1261568 bytes written and verified" in s)
    check("DOS/V disk: read back == R.XDF", get(d2hq, "DOSV.XDF") == R)
    check("DOS/V disk: B: image == R.XDF", open(fd1, "rb").read() == R)

    # 4. 書き込み禁止 (中身は R.XDF。HUMAN302.XDF は XEiJ が読み込むときに消されたファイル 2 つを戻すので、読んだものと一致しない)
    shutil.copy(rnd, fd1); os.chmod(fd1, 0o644)
    mk2hq(d2hq, {})
    s = xeij("protected", {1: fd1 + ":R", 2: c2hq, 3: d2hq},
             ["xdfwrite -s b:", "xdfwrite c:\\R.XDF b:", "xdfread b: d:\\WP.XDF"])
    check("write protected refused", "the disk in drive B: is write protected" in s)
    check("B: unchanged", open(fd1, "rb").read() == R)
    check("read of protected disk == R.XDF", get(d2hq, "WP.XDF") == R)

    # 5. ディスク無し
    mk2hq(d2hq, {})
    s = xeij("empty", {2: c2hq, 3: d2hq}, ["xdfwrite -s b:", "xdfwrite c:\\R.XDF b:", "xdfread b: d:\\NONE.XDF"])
    check("no disk (write)", s.count("no disk in drive B:") == 2)
    check("no file created", get(d2hq, "NONE.XDF") is None)

    print("ALL PASS" if not fails else "FAILED: " + ", ".join(fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
