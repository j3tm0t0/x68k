#!/usr/bin/env python3
"""zhost.py -- X68000 Z の USB ホストの真似 (raspi3 から机の上の Pico (p2w など) に): Pico のファームが Z で動くかを、Z を使わずに確かめる

Linux の usb-storage / usbhid を外し (要 sudo)、libusb で Z と同じ順の要求を投げる:
  1. 列挙: 記述子 (device / config / string / HID report) を読み、SET_CONFIGURATION、HID ごとに SET_IDLE・SET_PROTOCOL、
     MSC の GET_MAX_LUN (順と回数は profile の "enum")
  2. HID: 割り込みの IN を endpoint ごとの間隔で読み続ける (スレッド。Z はキーボードとマウスを読み続ける)
  3. MSC (BOT): profile の "msc" の命令を順に (TEST UNIT READY・INQUIRY・READ CAPACITY・MODE SENSE・READ(10)...)。
     HID を読みながら。READ(10) の番地は仮想 FAT の番地 (HDS は 0x803fa0 + ID x 0x800000 + LBA、x68kzremotedrv と同じ)
  4. 判定: どれかの転送が timeout (既定 5 秒) を超えたら HANG、STALL / エラーは記録して続ける。最後に要約
profile は JSON (profiles/*.json)。Z で採った記録 (capture) から作る。無ければ既定の推測 (profiles/z-guess.json)
usage: sudo ~/venv-usbbench/bin/python zhost.py [-p profile.json] [-t timeout_s] [-v] [--vidpid cafe:4002]
"""
import argparse, json, os, struct, sys, threading, time
import usb.core, usb.util

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--profile", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "profiles", "z-guess.json"))
ap.add_argument("-t", "--timeout", type=float, default=5.0)
ap.add_argument("--vidpid", default="cafe:4002")
ap.add_argument("-v", "--verbose", action="store_true")
args = ap.parse_args()
prof = json.load(open(args.profile))
T0 = time.time()
events, hangs, errors = [], [], []
lock = threading.Lock()

def log(kind, msg):
    t = time.time() - T0
    with lock:
        events.append((t, kind, msg))
        if args.verbose or kind in ("HANG", "ERR", "STALL", "INFO"):
            print("%8.3f %-5s %s" % (t, kind, msg), flush=True)

vid, pid = (int(x, 16) for x in args.vidpid.split(":"))
dev = usb.core.find(idVendor=vid, idProduct=pid)
if dev is None:
    sys.exit("zhost: no %s" % args.vidpid)
cfg0 = dev.get_active_configuration()
for intf in cfg0:
    if dev.is_kernel_driver_active(intf.bInterfaceNumber):
        dev.detach_kernel_driver(intf.bInterfaceNumber)
        log("INFO", "detached the kernel driver from interface %d" % intf.bInterfaceNumber)
TO = int(args.timeout * 1000)

def ctrl(bm, req, val, idx, n_or_data, what):
    t = time.time()
    try:
        r = dev.ctrl_transfer(bm, req, val, idx, n_or_data, timeout=TO)
        log("CTRL", "%s -> %s (%.1f ms)" % (what, len(r) if hasattr(r, "__len__") else r, (time.time() - t) * 1000))
        return r
    except usb.core.USBTimeoutError:
        log("HANG", "%s: control timeout %.1f s" % (what, args.timeout)); hangs.append(what)
    except usb.core.USBError as e:
        log("STALL" if "Pipe" in str(e) else "ERR", "%s: %s" % (what, e)); errors.append(what)

# ---- 1. 列挙 ----
hid_ifs, msc = [], None
for intf in cfg0:
    if intf.bInterfaceClass == 3:
        ep = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        hid_ifs.append((intf.bInterfaceNumber, intf.bInterfaceProtocol, ep))
    elif intf.bInterfaceClass == 8:
        msc = (intf.bInterfaceNumber,
               usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT),
               usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN))
log("INFO", "HID interfaces %s, MSC %s" % ([(i, p, hex(e.bEndpointAddress), e.bInterval) for i, p, e in hid_ifs], msc and msc[0]))
for step in prof.get("enum", []):
    op = step["op"]
    if op == "get_descriptor":
        typ, idx, lang, n = step["type"], step.get("index", 0), step.get("lang", 0), step.get("len", 255)
        rcpt = 0x81 if typ == 0x22 else 0x80
        wi = step.get("interface", 0) if typ == 0x22 else lang
        ctrl(rcpt, 6, (typ << 8) | idx, wi, n, "GET_DESCRIPTOR type 0x%02x index %d len %d" % (typ, idx, n))
    elif op == "set_configuration":
        try:
            dev.set_configuration(step.get("value", 1)); log("CTRL", "SET_CONFIGURATION %d" % step.get("value", 1))
        except usb.core.USBError as e:
            log("ERR", "SET_CONFIGURATION: %s" % e); errors.append("set_configuration")
    elif op == "hid_set_idle":
        for i, p, e in hid_ifs:
            ctrl(0x21, 0x0a, step.get("duration", 0) << 8, i, None, "SET_IDLE if%d" % i)
    elif op == "hid_set_protocol":
        for i, p, e in hid_ifs:
            if p in step.get("protocols", [1, 2]):
                ctrl(0x21, 0x0b, step.get("value", 0), i, None, "SET_PROTOCOL %d if%d" % (step.get("value", 0), i))
    elif op == "hid_get_report":
        for i, p, e in hid_ifs:
            ctrl(0xa1, 0x01, (1 << 8), i, step.get("len", 8), "GET_REPORT input if%d" % i)
    elif op == "hid_set_report_led":
        for i, p, e in hid_ifs:
            if p == 1:
                ctrl(0x21, 0x09, (2 << 8), i, bytes([step.get("leds", 0)]), "SET_REPORT LED if%d" % i)
    elif op == "msc_get_max_lun":
        if msc:
            ctrl(0xa1, 0xfe, 0, msc[0], 1, "GET_MAX_LUN")
    elif op == "sleep":
        time.sleep(step["s"])

# ---- 2. HID を読み続ける ----
stop = threading.Event()
hidstat = {}
def hid_reader(i, p, ep, interval_ms):
    n = to = 0
    while not stop.is_set():
        t = time.time()
        try:
            dev.read(ep.bEndpointAddress, ep.wMaxPacketSize, timeout=max(1, interval_ms))
            n += 1
        except usb.core.USBTimeoutError:
            to += 1          # NAK のまま (変化なし): ふつう
        except usb.core.USBError as e:
            log("ERR", "HID if%d read: %s" % (i, e)); errors.append("hid%d" % i); time.sleep(0.1)
        hidstat[i] = (n, to)
        dt = time.time() - t
        if dt < interval_ms / 1000:
            time.sleep(interval_ms / 1000 - dt)
for i, p, ep in hid_ifs:
    iv = prof.get("hid_interval_ms", {}).get(str(p), ep.bInterval)
    threading.Thread(target=hid_reader, args=(i, p, ep, iv), daemon=True).start()

# ---- 3. MSC (Bulk-Only Transport) ----
tag = [0x5a000000]
def bot(lun, cdb, din=0, dout=b"", what=""):
    tag[0] += 1
    cbw = struct.pack("<IIIBBB", 0x43425355, tag[0], din or len(dout), 0x80 if din else 0, lun, len(cdb)) + bytes(cdb).ljust(16, b"\0")
    t = time.time()
    try:
        dev.write(msc[1].bEndpointAddress, cbw, timeout=TO)
        data = b""
        if din:
            data = bytes(dev.read(msc[2].bEndpointAddress, din, timeout=TO))
        elif dout:
            dev.write(msc[1].bEndpointAddress, dout, timeout=TO)
        csw = bytes(dev.read(msc[2].bEndpointAddress, 13, timeout=TO))
        st = csw[12] if len(csw) == 13 else -1
        ms = (time.time() - t) * 1000
        log("MSC", "%s -> status %d, %d bytes (%.1f ms)" % (what, st, len(data), ms))
        return st, data, ms
    except usb.core.USBTimeoutError:
        log("HANG", "%s: bulk timeout %.1f s" % (what, args.timeout)); hangs.append(what)
    except usb.core.USBError as e:
        log("STALL" if "Pipe" in str(e) else "ERR", "%s: %s" % (what, e)); errors.append(what)
    return None, b"", (time.time() - t) * 1000

lat = {}
def cmd(c):
    op, lun = c["op"], c.get("lun", 0)
    if op == "tur":
        return bot(lun, [0, 0, 0, 0, 0, 0], what="TEST UNIT READY")
    if op == "inquiry":
        return bot(lun, [0x12, 0, 0, 0, 36, 0], 36, what="INQUIRY")
    if op == "read_capacity":
        return bot(lun, [0x25] + [0] * 9, 8, what="READ CAPACITY")
    if op == "mode_sense":
        return bot(lun, [0x1a, 0, c.get("page", 0x3f), 0, c.get("len", 192), 0], c.get("len", 192), what="MODE SENSE(6) page 0x%02x" % c.get("page", 0x3f))
    if op == "request_sense":
        return bot(lun, [0x03, 0, 0, 0, 18, 0], 18, what="REQUEST SENSE")
    if op in ("read10", "read_hds"):
        lba = c["lba"] if op == "read10" else 0x803fa0 + c["id"] * 0x800000 + c["lba"]
        n = c.get("n", 1)
        return bot(lun, struct.pack(">BBIBHB", 0x28, 0, lba, 0, n, 0), n * 512, what="READ(10) 0x%x n=%d" % (lba, n))
    raise SystemExit("unknown msc op %s" % op)

if msc:
    for c in prof.get("msc", []):
        rep = c.get("repeat", 1)
        for k in range(rep):
            if c["op"] == "sleep":
                time.sleep(c["s"]); break
            st, data, ms = cmd(c)
            lat.setdefault(c["op"], []).append(ms)
            if c.get("gap_ms"):
                time.sleep(c["gap_ms"] / 1000)
        if hangs and not prof.get("continue_after_hang"):
            break
time.sleep(prof.get("tail_s", 3))
stop.set()

# ---- 4. 要約 ----
print("== zhost summary (%s, %.1f s)" % (os.path.basename(args.profile), time.time() - T0))
for op, v in lat.items():
    v = sorted(v)
    print("   msc %-14s n=%-5d median %.1f ms  max %.1f ms" % (op, len(v), v[len(v) // 2], v[-1]))
for i, (n, to) in sorted(hidstat.items()):
    print("   hid if%d reports %d, empty polls %d" % (i, n, to))
print("   errors/stalls: %d  %s" % (len(errors), sorted(set(errors))[:8]))
print("RESULT", "HANG" if hangs else ("ERRORS" if errors else "OK"), hangs[:3])
sys.exit(2 if hangs else (1 if errors else 0))
