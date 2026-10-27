#!/usr/bin/env python3
"""起動フロッピーを XEiJ (Compact、主記憶 2MB) で起動し、SECONDS 秒後の画面の文字を出す。
usage: xeij_boot.py RUNDIR XDF [SECONDS]
XDF は RUNDIR/fd0.xdf に写してから使う (元のファイルは変えない)。2 つのイメージを同じ秒数で起動して画面を比べるのに使う。"""
import os, shutil, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
top = os.path.abspath(os.path.join(here, "../../.."))
tools = os.environ.get("XEIJ_TOOLS", "/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools")
run, xdf = os.path.abspath(sys.argv[1]), sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 40
os.makedirs(run, exist_ok=True)
for f in ("cgrom.bin", "agent.cmd", "XEiJControl"):
    if os.path.exists(os.path.join(run, f)): os.remove(os.path.join(run, f))
fd0 = os.path.join(run, "fd0.xdf")
shutil.copy(xdf, fd0); os.chmod(fd0, 0o644)
java = "/opt/homebrew/opt/openjdk/bin/java"
if not os.path.exists(java): java = "java"
xe = subprocess.Popen([java, "-javaagent:%s/xeijagent.jar=%s/agent.cmd" % (tools, run), "-Djava.io.tmpdir=" + run,
                       "-jar", os.path.join(tools, "xeij/XEiJ.jar"), "-config=default", "-model=Compact", "-memory=2",
                       "-boot=fd0", "-fd0=" + fd0, "-flrinformed=on", "-usecanvas=off", "-pastepipe=on"],
                      cwd=run, stdin=subprocess.DEVNULL, stdout=open(os.path.join(run, "xeij.log"), "w"),
                      stderr=subprocess.STDOUT)
try:
    time.sleep(secs)
    print(subprocess.run([sys.executable, os.path.join(top, "boot/serial/test/xscreen.py"), run],
                         capture_output=True, text=True).stdout.rstrip())
finally:
    xe.terminate()
    try: xe.wait(10)
    except subprocess.TimeoutExpired: xe.kill()
