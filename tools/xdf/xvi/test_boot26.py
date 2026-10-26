#!/usr/bin/env python3
"""BOOT26.XDF を XVI と同じ手順で XEiJ で試す。
usage: test_boot26.py RUNDIR BOOT26.XDF XVIA.XDF
  1. HUMAN302 で起動し、B: に DOS/V の 1.44MB の空ディスク、C: に BOOT26.XDF (2HQ に入れたもの) を置いて
     `xdfwrite c:\\BOOT26.XDF b:` (自動で初期化) と `chkdsk b:` を打つ。B: がイメージと一致するか
  2. 書いた B: から起動した画面と、XVIA.XDF (元の A:) から起動した画面が同じか (CONFIG.SYS と AUTOEXEC.BAT を
     CTTY AUX の手前まで走らせる。RS-232C の相手が無いので、その先は比べない)"""
import os, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "../tests"))
import xeij_xdf as t
run, boot, xvia = (os.path.abspath(a) for a in sys.argv[1:4])
t.run = run
os.makedirs(run, exist_ok=True)
fd1, c2hq = os.path.join(run, "fd1.xdf"), os.path.join(run, "c.2hq")
t.mk2hq(fd1, {})
t.mk2hq(c2hq, {"BOOT26.XDF": boot})
s = t.xeij("write", {1: fd1, 2: c2hq}, ["xdfwrite c:\\BOOT26.XDF b:", "chkdsk b:"])
t.check("written and verified", "OK: 1261568 bytes written and verified" in s)
# CHKDSK の数字の行: 全クラスタ、隠しファイル、ディレクトリ、ファイル、空き。壊れていれば足りないか、エラーの行が増える
nums = [int(l.split()[0]) for l in s.split("chkdsk b:")[1].split("TESTDONE")[0].splitlines()
        if l.strip() and l.split()[0].isdigit()]
t.check("chkdsk b: adds up (%s)" % nums, len(nums) == 5 and nums[0] == sum(nums[1:]))
t.check("B: == BOOT26.XDF", open(fd1, "rb").read() == open(boot, "rb").read())
screens = []
for name, img in (("boot-written", fd1), ("boot-xvia", xvia)):
    out = subprocess.run([sys.executable, os.path.join(here, "../tests/xeij_boot.py"), os.path.join(run, name), img, "45"],
                         capture_output=True, text=True).stdout
    print("== %s\n%s" % (name, out))
    screens.append(out)
t.check("boot screen same as the original A:", screens[0] == screens[1] and "npd: netpipe thread started" in screens[0])
print("ALL PASS" if not t.fails else "FAILED: " + ", ".join(t.fails))
sys.exit(1 if t.fails else 0)
