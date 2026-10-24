#!/usr/bin/env python3
"""起動フロッピー (floppy/mkboot.sh) を XEiJ で試す: 起動 → 2 秒の間にキーを押してキーボードを残す →
SHELLCMD (既定 zos) で zosh のプロンプト → ssh -V で dropbear の版が出るか。
usage: floppy_test.py RUNDIR XDF MEM_MB [SHELLCMD]"""
import os, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
run, xdf, mem = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
cmd = sys.argv[4] if len(sys.argv) > 4 else "zos"
for f in ("fd0.xdf", "cgrom.bin"):
    if os.path.exists(os.path.join(run, f)): os.remove(os.path.join(run, f))
os.makedirs(run, exist_ok=True)
env = dict(os.environ, XMEM=mem, XEIJ_RSBUF="1024")
xe = subprocess.Popen([os.path.join(here, "xeij-fd.sh"), run, xdf], env=env, stdin=subprocess.DEVNULL)
def screen():
    return subprocess.run([sys.executable, os.path.join(here, "xscreen.py"), run], capture_output=True, text=True).stdout
def wait(pat, t):
    end = time.time() + t
    while time.time() < end:
        s = screen()
        if pat in s: return s
        time.sleep(0.3)
    sys.exit("floppy_test: no %r on the screen:\n%s" % (pat, screen()))
def key(name):
    open(os.path.join(run, "XEiJControl"), "w").write("typekey %s\n" % name)
def typ(text):
    subprocess.run([sys.executable, os.path.join(here, "xtype.py"), run, text], env=dict(os.environ, XTYPE_DELAY="0.35"), check=True)
try:
    t0 = time.time()
    while not os.path.exists(os.path.join(run, "XEiJControl")): time.sleep(0.5)
    wait("press a key", 120)
    key("space")
    wait("keyboard kept", 10)
    print("floppy_test: boot to the RX prompt %.1fs" % (time.time() - t0))
    time.sleep(1.5)
    t1 = time.time()
    typ(cmd + "\\n")
    wait("xvi:A:/%", 60)
    print("floppy_test: %s -> zosh prompt %.1fs" % (cmd, time.time() - t1))
    time.sleep(1)
    typ("ssh -V\\n")
    s = wait("2022.83", 30)
    print("\n".join(l for l in s.splitlines() if l.strip())[-400:])
    print("FLOPPY PASS (%s MB, %s)" % (mem, cmd))
finally:
    xe.terminate()
    subprocess.run(["pkill", "-f", run + "/agent.cmd"])
