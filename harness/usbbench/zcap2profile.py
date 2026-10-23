#!/usr/bin/env python3
"""zcap2profile.py -- 採取用ファーム (CFG_TUSB_DEBUG=2 + zcap.c) の UART ログから zhost.py の profile を作る

usage: zcap2profile.py picouart.log [-o profile.json] [--from MS] [--to MS] [--speed 1.0]
ログの行: 'HH:MM:SS [<起動からの ms>] <TinyUSB の行>' (raspi3 の picolog の記録)
  USBD Setup Received bm req vL vH iL iH lL lH   → enum (Set Address と、STALL を解く Clear Feature・BOT Reset は除く)
  SCSI Command [LunN]: <名前>                     → msc (時刻 t 付き)。Read10 の番地と長さは次の 'ZC R' の行から
  ZC C lun=N cdb=..                               → 名前の無い命令 (PREVENT ALLOW など) の CDB
WRITE(10) は試験台では送らない (数だけ数えて comment に書く)。
要約 (命令ごとの数、間隔) を標準エラーに出す。
"""
import argparse, json, re, sys
from collections import Counter

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("-o", "--out")
ap.add_argument("--from", dest="t_from", type=int, default=0)
ap.add_argument("--to", dest="t_to", type=int, default=1 << 62)
args = ap.parse_args()

LINE = re.compile(r"^(?:\d\d:\d\d:\d\d )?\[(\d+)\] (.*)$")
NAMES = {"Test Unit Ready": "tur", "Inquiry": "inquiry", "Read Capacity10": "read_capacity", "Mode_Sense 6": "mode_sense",
         "Request Sense": "request_sense", "Read10": "read10", "Write10": "write10", "Read Format Capacity": "read_format_capacity"}
enum, msc = [], []
mix, writes, boot = Counter(), 0, None
cur = None
for raw in open(args.log, errors="replace"):
    m = LINE.match(raw.rstrip("\n"))
    if not m:
        continue
    t, s = int(m.group(1)), m.group(2).strip()
    if boot is not None and t + 1000 < boot:
        print("zcap2profile: the Pico rebooted at %d ms; starting over" % t, file=sys.stderr)
        enum, msc, mix, writes, cur = [], [], Counter(), 0, None
    boot = t
    if not (args.t_from <= t <= args.t_to):
        continue
    if s.startswith("Stall EP0") and enum and enum[-1].get("op") == "ctrl":
        enum[-1]["stall"] = True         # 元のファームはこの要求を STALL した (Device Qualifier など)。試験台でも同じなら正しい
    elif s.startswith("USBD Bus Reset"):
        enum.append({"op": "comment", "text": "bus reset at %d ms" % t})
    elif s.startswith("USBD Setup Received"):
        b = [int(x, 16) for x in s.split()[3:11]]
        bm, req, val, idx, ln = b[0], b[1], b[2] | b[3] << 8, b[4] | b[5] << 8, b[6] | b[7] << 8
        if (bm, req) in ((0x00, 0x05), (0x02, 0x01), (0x21, 0xff)):
            continue                     # Set Address (libusb の領分) / Clear Feature HALT / BOT Reset (どちらも STALL への返事)
        if (bm, req) == (0x00, 0x09):
            enum.append({"op": "set_configuration", "value": val, "t": t}); continue
        enum.append({"op": "ctrl", "bm": bm, "req": req, "val": val, "idx": idx, "len": ln, "t": t})
    elif s.startswith("SCSI Command"):
        mm = re.match(r"SCSI Command \[Lun(\d)\]: (.*)", s)
        lun, name = int(mm.group(1)), mm.group(2).strip()
        op = NAMES.get(name, "scsi")
        mix["%s lun%d" % (name, lun)] += 1
        cur = {"op": op, "lun": lun, "t": t}
        if op == "write10":
            writes += 1; cur = None; continue
        if op == "scsi":
            cur["name"] = name
            if name.startswith("Prevent/Allow"):   # TinyUSB が自分で答える命令は CDB が残らない。Z は 0 (許す) と見なす
                cur["cdb"] = ["1e", "00", "00", "00", "00", "00"]
        msc.append(cur)
    elif s.startswith("ZC R") and cur is not None and cur["op"] == "read10":
        kv = dict(x.split("=") for x in s.split()[2:])
        if int(kv["off"]) == 0:
            cur["lba"] = int(kv["lba"], 16)
        cur["n"] = cur.get("n", 0) + int(kv["len"]) // 512
    elif s.startswith("ZC C") and cur is not None and cur["op"] == "scsi":
        cur["cdb"] = s.split("cdb=")[1].split()
for c in [c for c in msc if c["op"] == "scsi" and "cdb" not in c]:
    print("zcap2profile: no CDB for '%s' at %d ms; left out" % (c["name"], c["t"]), file=sys.stderr)
msc = [c for c in msc if c["op"] != "scsi" or "cdb" in c]
for c in msc:                            # 番地が取れなかった READ(10) (失敗など) は 1 セクタ
    if c["op"] == "read10":
        c.setdefault("lba", 0); c.setdefault("n", 1)
t0 = msc[0]["t"] if msc else 0
for c in msc:
    c["t"] -= t0
prof = {"comment": "zcap2profile.py %s: %d enum requests, %d MSC commands (%d WRITE(10) left out)" % (args.log, len(enum), len(msc), writes),
        "enum": enum, "msc": msc, "timed": True, "tail_s": 3}
out = open(args.out, "w") if args.out else sys.stdout
json.dump(prof, out, indent=1)
print("zcap2profile: %d enum, %d msc over %.1f s; writes %d" % (len(enum), len(msc), (msc[-1]["t"] / 1000) if msc else 0, writes), file=sys.stderr)
for k, v in mix.most_common():
    print("   %5d  %s" % (v, k), file=sys.stderr)
