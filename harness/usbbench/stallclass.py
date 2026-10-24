#!/usr/bin/env python3
"""stallclass.py -- release-check の 1 ケースの「10 秒を超えた操作」を分ける

usage: stallclass.py <zfat.log> <case.uart> <case.dmesg> [<case.pcap>|-] [<窓の始め秒> <窓の終わり秒>]
出力 (1 行目): <不合格の数> <SMB を止めた区間の待ち> <既知の試験台の現象> <usb: stall の数> <usb: clear-halt の数>、2 行目から説明。

分け方:
  - SMB を止めた区間 (zfat の起動からの秒、戻してから 15 秒まで) に重なる → 正しい待ち (NOT READY の繰り返し)
  - 次をすべて満たす 1 回だけ → 既知の試験台の現象 (#38、raspi3 の dwc_otg とハブ。実機の Z では出ない):
      約 30 秒 (25〜40 秒)、終わりの ±5 秒に Linux の USB リセット、UART に "msc: no callback for N ms" (N >= 28000) が ±5 秒、
      その間 445 番が 20 秒以上無通信 (pcap があるとき)、その区間に "usb: stall" / NOT READY / MEDIUM ERROR の行が無い
  - それ以外 (Pico が出した STALL、エラーの返事、リセットの無い止まり、1 ケースに 2 回以上、戻らなかった操作) → 不合格
"""
import re, subprocess, sys, time
from datetime import datetime

zlog, uart, dmesg = sys.argv[1:4]
pcap = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != "-" else None
win = (float(sys.argv[5]), float(sys.argv[6]) + 15) if len(sys.argv) > 6 else None

t0 = None
slow, stall_only, ended = [], [], set()
for l in open(zlog, errors="replace"):
    m = re.search(r"T0 epoch=([\d.]+)", l)
    if m:
        t0 = float(m.group(1))
    m = re.match(r"\s*([\d.]+) SLOW\s+(.*) took ([\d.]+) s", l)
    if m and float(m.group(3)) > 10:
        t1 = float(m.group(1)); slow.append((t1 - float(m.group(3)), t1, m.group(2))); ended.add(m.group(2))
for l in open(zlog, errors="replace"):
    m = re.match(r"\s*([\d.]+) STALL\s+(.*): [\d.]+ s so far", l)
    if m and m.group(2) not in ended:
        stall_only.append(m.group(2)); ended.add(m.group(2))

def day_epoch(hms, ref):
    """UART の HH:MM:SS を、ref (epoch) と同じ日の epoch に。日付は T0 から取るので、真夜中をまたぐ試験では 0 時以降の行が
    1 日早くなる (今は真夜中をまたいで走らせないので放っておく)"""
    d = datetime.fromtimestamp(ref)
    h, m_, s = (float(x) for x in hms.split(":"))
    return datetime(d.year, d.month, d.day).timestamp() + h * 3600 + m_ * 60 + s

ref = t0 or time.time()
uart_lines = []
for l in open(uart, errors="replace"):
    m = re.match(r"(\d\d:\d\d:\d\d) (.*)", l)
    if m:
        uart_lines.append((day_epoch(m.group(1), ref), m.group(2)))
resets = []
for l in open(dmesg, errors="replace"):
    if "reset full-speed USB device" in l:
        m = re.search(r"(\d\d:\d\d:\d\d\.\d+)", l)
        if m:
            resets.append(day_epoch(m.group(1), ref))
gaps = []
if pcap:
    try:
        out = subprocess.run(["sudo", "tcpdump", "-r", pcap, "-tt", "-nn"], capture_output=True, text=True).stdout
        p = None
        for l in out.splitlines():
            t = float(l.split()[0])
            if p and t - p >= 20:
                gaps.append((p, t))
            p = t
    except Exception:
        pcap = None

fail, expected, artefact, notes = 0, 0, 0, []
cand = []
for a, b, what in slow:
    if win and a < win[1] and b > win[0]:
        expected += 1
        continue
    cand.append((a, b, what))
for what in stall_only:
    fail += 1; notes.append("never returned: %s" % what)

def why_not_artefact(a, b, what):
    if t0 is None:
        return "no T0 in the zfat log"
    dur = b - a
    if not 25 <= dur <= 40:
        return "%.1f s, not ~30 s" % dur
    ea, eb = t0 + a, t0 + b
    rs = [r for r in resets if abs(r - eb) <= 5]
    if not rs:
        return "no Linux USB reset at its end"
    r = rs[0]
    nc = [x for x in uart_lines if abs(x[0] - r) <= 5 and re.search(r"msc: no callback for (\d+) ms", x[1])
          and int(re.search(r"msc: no callback for (\d+) ms", x[1]).group(1)) >= 28000]
    if not nc:
        return "no 'msc: no callback for >=28 s' line near the reset"
    bad = [x[1] for x in uart_lines if ea - 2 <= x[0] <= eb + 2 and re.search(r"usb: stall|NOT READY|MEDIUM ERROR", x[1])]
    if bad:
        return "device-side line in the stall: %s" % bad[0][:60]
    if pcap and not [g for g in gaps if g[0] < eb and g[1] > ea and g[1] - g[0] >= 20]:
        return "445 was not silent"
    return None

def host_clear_halt(a, b):
    """止まりの間に "usb: clear-halt" (ホストが出した CLEAR_FEATURE(HALT)) があったか。あっても試験台の現象のまま、印を付けて数える"""
    if t0 is None:
        return False
    return any(t0 + a - 2 <= x[0] <= t0 + b + 2 and "usb: clear-halt" in x[1] for x in uart_lines)

if len(cand) == 1:
    a, b, what = cand[0]
    w = why_not_artefact(a, b, what)
    if w is None:
        artefact = 1
        notes.append("known bench artefact (#38): %s, %.1f s, ended by a Linux USB reset%s" % (
            what, b - a, ", with host clear-halt" if host_clear_halt(a, b) else ""))
    else:
        fail += 1; notes.append("stall %s (%.1f s): %s" % (what, b - a, w))
else:
    for a, b, what in cand:
        fail += 1
        notes.append("stall %s (%.1f s)%s" % (what, b - a, "; %d stalls in one case" % len(cand) if len(cand) > 1 else ""))
# ケース全体の STALL / clear-halt の数: "msc: no callback … stalls S, clear-halts C" の最後の行、無ければ usb: の行を数える
tot = None
for t, l in uart_lines:
    m = re.search(r"stalls (\d+), clear-halts (\d+)", l)
    if m:
        tot = (int(m.group(1)), int(m.group(2)))
if tot is None:
    tot = (sum("usb: stall" in l for t, l in uart_lines), sum("usb: clear-halt" in l for t, l in uart_lines))
print(fail, expected, artefact, tot[0], tot[1])
for n in notes:
    print(n)
