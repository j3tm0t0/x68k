#!/usr/bin/env python3
"""zfat.py -- Z の Linux と同じく Pico の仮想 FAT を vfat でマウントし、X68000Z/image の HDS をファイルとして叩く (raspi3、root)

Z の中の Linux は HDS を生の LBA ではなく、仮想 FAT の X68000Z/image/<ID>RMTHDS.HDS というファイルとして読み書きする
(ページキャッシュ・先読み・書き戻し付き)。それを真似て、読んだ中身を元の HDS (SMB の共有の実ファイル) の写し (shadow) と比べる。

  1. マウント (-o rw,noatime、Z の Linux の正確な指定は不明)。--boot: 機器が現れたらすぐ (HDS が開く前に) マウントし、
     ファイルの大きさが仮 (0xfffffe00) から変わるかを見る
  2. ランダムな読み書き: 512 バイト単位、1〜128 セクタ。O_DIRECT と、ふつう (ページキャッシュ経由、ときどき drop_caches) を混ぜる。
     書いたら shadow も更新。読んだら shadow と比べ、違えば「どのセクタが、何に (0 / 別のセクタの中身 / 不明) なっていたか」
  3. 裏で idle の輪: 2 秒ごとに stat と先頭セクタの読み (Z の Linux の TEST UNIT READY の頃合い)
  4. 見張り: 1 回の操作が --stall 秒を超えたら STALL、--hang 秒で HANG
  5. ときどき: fsync してから SMB の実ファイル (--src) の書いた範囲が shadow と同じか (書き込みの道の端から端まで)
  6. 終わりに umount、生のデバイスから FAT の鎖を辿って大きさと合うか (raspi3 に fsck.vfat が無いので自前)
usage: sudo python3 zfat.py --src /srv/x68000z/pzltest.hds [--ops 3000] [--seed N] [--boot] [--write 0.2] [--direct 0.5]
終了コード: 0 = OK、1 = 中身の違い / エラー、2 = HANG
"""
import argparse, glob, mmap, os, random, subprocess, sys, threading, time, zlib

ap = argparse.ArgumentParser()
ap.add_argument("--src", required=True, help="元の HDS (SMB の共有の実ファイル)")
ap.add_argument("--dev", default="/dev/disk/by-id/usb-X68000Z_Remote_Drive_Mas_123456789012-0:0")
ap.add_argument("--mnt", default="/mnt/zpico")
ap.add_argument("--file", default="X68000Z/image/0RMTHDS.HDS")
ap.add_argument("--opts", default="rw,noatime")
ap.add_argument("--ops", type=int, default=3000)
ap.add_argument("--seed", type=int, default=None)
ap.add_argument("--write", type=float, default=0.2, help="書き込みの割合")
ap.add_argument("--direct", type=float, default=0.5, help="O_DIRECT の割合")
ap.add_argument("--maxsec", type=int, default=128)
ap.add_argument("--span", type=int, default=0, help="叩く範囲 (バイト、0 = HDS 全体)")
ap.add_argument("--stall", type=float, default=10)
ap.add_argument("--hang", type=float, default=60)
ap.add_argument("--boot", action="store_true", help="機器が現れたらすぐマウント (起動の窓)")
ap.add_argument("--idle", type=float, default=2.0)
ap.add_argument("--duration", type=float, default=0, help="この秒数で打ち切る (--ops より先に)")
ap.add_argument("--rate", type=float, default=0, help="1 秒あたりの操作の上限 (0 = 全速)")
args = ap.parse_args()
seed = args.seed if args.seed is not None else int(time.time())
rnd = random.Random(seed)
T0 = time.time()
lock = threading.Lock()
bad = []
def log(kind, msg):
    with lock:
        print("%8.2f %-6s %s" % (time.time() - T0, kind, msg), flush=True)

# ---- 1. マウント ----
os.makedirs(args.mnt, exist_ok=True)
subprocess.run(["umount", args.mnt], stderr=subprocess.DEVNULL)
if args.boot:
    # 先に起動しておき、Pico のリセット (swdflash / 電源) で機器が消えて、また現れたらすぐマウント
    t = time.time()
    while os.path.exists(args.dev) and time.time() - t < 60:
        time.sleep(0.02)
    log("INFO", "device gone" if not os.path.exists(args.dev) else "device never went away (60 s)")
    while not os.path.exists(args.dev):
        time.sleep(0.02)
    log("INFO", "device appeared")
for i in range(200):
    r = subprocess.run(["mount", "-t", "vfat", "-o", args.opts, args.dev, args.mnt], capture_output=True, text=True)
    if r.returncode == 0:
        break
    if i % 20 == 0:
        log("INFO", "mount: %s" % r.stderr.strip())
    time.sleep(0.1)
else:
    sys.exit("zfat: mount failed")
path = os.path.join(args.mnt, args.file)
st0 = os.stat(path)
log("INFO", "mounted; %s size 0x%x; seed %d; files %s" % (args.file, st0.st_size, seed,
    sorted(os.path.relpath(p, args.mnt) for p in glob.glob(args.mnt + "/X68000Z/image/*"))))

shadow = bytearray(open(args.src, "rb").read())
span = min(args.span or len(shadow), len(shadow))
log("INFO", "source %s %d bytes crc %08x; span %d" % (args.src, len(shadow), zlib.crc32(shadow), span))
written = set()                          # 書いたセクタ
# Pico は読むときだけ HDS の LBA 2 を hdsboot に、0x20 / 0x21 を元の 2 / 3 に差し替える (書き込みは差し替えない): 比べない
MASK = {2, 0x20, 0x21}

fd_b = os.open(path, os.O_RDWR)
fd_d = os.open(path, os.O_RDWR | os.O_DIRECT)
abuf = mmap.mmap(-1, args.maxsec * 512)  # O_DIRECT 用 (ページ境界)

def classify(got, off):
    """違ったセクタの中身が何か"""
    if got == bytes(512):
        return "zeros"
    i = shadow.find(got)
    if i >= 0 and i % 512 == 0:
        return "data of sector 0x%x" % (i // 512)
    return "unknown (crc %08x)" % zlib.crc32(got)

# ---- 4. 見張り ----
cur = {"op": None, "t": 0}
hung = threading.Event()
def watchdog():
    warned = None
    while not hung.is_set():
        time.sleep(0.5)
        with lock:
            op, t = cur["op"], cur["t"]
        if op is None:
            continue
        d = time.time() - t
        if d > args.hang:
            log("HANG", "%s: no return for %.0f s" % (op, d)); hung.set(); os._exit(2)
        if d > args.stall and warned != (op, t):
            log("STALL", "%s: %.1f s so far" % (op, d)); warned = (op, t)
threading.Thread(target=watchdog, daemon=True).start()

def timed(what, f):
    with lock:
        cur["op"], cur["t"] = what, time.time()
    t = time.time()
    try:
        return f()
    finally:
        with lock:
            cur["op"] = None
        dt = time.time() - t
        if dt > 2:
            log("SLOW", "%s took %.1f s" % (what, dt))

# ---- 3. idle の輪 ----
stop = threading.Event()
sizes = {}
def idle():
    fd = os.open(path, os.O_RDONLY)
    while not stop.is_set():
        try:
            s = os.fstat(fd).st_size
            if s not in sizes:
                sizes[s] = time.time() - T0
                log("INFO", "file size now 0x%x" % s)
            os.pread(fd, 512, 0)
        except OSError as e:
            log("ERR", "idle: %s" % e)
        stop.wait(args.idle)
    os.close(fd)
idle_th = threading.Thread(target=idle, daemon=True); idle_th.start()

# ---- 2. ランダムな読み書き ----
stats = {"read": 0, "write": 0, "direct": 0, "eio": 0, "mismatch_sectors": 0}
def do_read(off, n, direct):
    if direct:
        k = os.preadv(fd_d, [memoryview(abuf)[:n]], off); got = bytes(abuf[:k])
    else:
        got = os.pread(fd_b, n, off)
    return got
def do_write(off, data, direct):
    if direct:
        abuf[:len(data)] = data; return os.pwritev(fd_d, [memoryview(abuf)[:len(data)]], off)
    k = os.pwrite(fd_b, data, off); os.fdatasync(fd_b); return k

t_start = time.time()
for i in range(args.ops):
    if args.duration and time.time() - t_start > args.duration:
        log("INFO", "duration reached after %d ops" % i); break
    if args.rate:
        d = t_start + i / args.rate - time.time()
        if d > 0:
            time.sleep(d)
    n = rnd.randint(1, args.maxsec) * 512
    off = rnd.randrange(0, (span - n) // 512 + 1) * 512
    direct = rnd.random() < args.direct
    kind = "W" if rnd.random() < args.write else "R"
    what = "%s%s off=0x%x n=%d" % (kind, "d" if direct else "b", off, n // 512)
    try:
        if kind == "W":
            data = rnd.randbytes(n)
            k = timed(what, lambda: do_write(off, data, direct))
            if k != n:
                log("ERR", "%s: short write %d" % (what, k)); bad.append(what)
            shadow[off:off + n] = data
            written.update(range(off // 512, (off + n) // 512))
            stats["write"] += 1
        else:
            got = timed(what, lambda: do_read(off, n, direct))
            stats["read"] += 1
            if len(got) != n:
                log("ERR", "%s: short read %d" % (what, len(got))); bad.append(what)
            want = shadow[off:off + len(got)]
            if got != want:
                secs = [s for s in range(len(got) // 512) if got[s * 512:(s + 1) * 512] != want[s * 512:(s + 1) * 512]
                        and off // 512 + s not in MASK]
            else:
                secs = []
            if secs:
                stats["mismatch_sectors"] += len(secs)
                s0 = secs[0]
                log("BAD", "%s: %d/%d sectors differ, first 0x%x = %s%s" % (
                    what, len(secs), n // 512, off // 512 + s0, classify(got[s0 * 512:(s0 + 1) * 512], off),
                    " (written before)" if off // 512 + s0 in written else ""))
                bad.append(what)
        stats["direct"] += direct
    except OSError as e:
        stats["eio"] += 1
        log("ERR", "%s: %s" % (what, e)); bad.append(what)
    if not direct and i % 50 == 49:      # Z の Linux もキャッシュを追い出す: ページキャッシュ経由の読みが本当に USB まで行くように
        open("/proc/sys/vm/drop_caches", "w").write("1\n")
    if i % 500 == 499:
        # ---- 5. SMB の実ファイルと比べる (書き込みの道の端から端まで) ----
        os.fsync(fd_b); time.sleep(1.5)
        real = open(args.src, "rb").read()
        diff = [s for s in sorted(written) if real[s * 512:(s + 1) * 512] != shadow[s * 512:(s + 1) * 512]]
        if diff:
            log("BAD", "after %d ops: %d written sectors differ on the SMB file, first 0x%x" % (i + 1, len(diff), diff[0]))
            bad.append("smb")
        log("INFO", "%d ops: %s; written sectors %d, SMB file %s" % (i + 1, stats, len(written), "same" if not diff else "DIFFERENT"))

stop.set(); idle_th.join(args.idle + 5)
os.fsync(fd_b); os.close(fd_b); os.close(fd_d)
time.sleep(2)
real = open(args.src, "rb").read()
diff = [s for s in sorted(written) if real[s * 512:(s + 1) * 512] != shadow[s * 512:(s + 1) * 512]]
st1 = os.stat(path)
r = subprocess.run(["umount", args.mnt], capture_output=True, text=True)

def fatcheck():
    """マウントを外したあと、生のデバイスから: BPB → X68000Z/image の項目 → FAT の鎖の長さが大きさと合うか"""
    import struct
    f = open(args.dev, "rb", buffering=0)
    def sec(n, k=1):
        f.seek(n * 512); return f.read(512 * k)
    b = sec(0)
    bps, spc, rsv, nfat = struct.unpack_from("<HBHB", b, 11)
    fatsz, rootclus = struct.unpack_from("<I", b, 36)[0], struct.unpack_from("<I", b, 44)[0]
    data0 = rsv + nfat * fatsz
    clus = lambda c: data0 + (c - 2) * spc
    fatcache = {}
    def fat(c):
        s_ = rsv + c * 4 // 512
        if s_ not in fatcache:
            fatcache[s_] = sec(s_)
        return struct.unpack_from("<I", fatcache[s_], c * 4 % 512)[0] & 0x0fffffff
    def find(dirclus, name):
        d = sec(clus(dirclus), spc)
        for o in range(0, len(d), 32):
            if d[o] == 0:
                break
            if d[o:o + 11] == name:
                hi, lo = struct.unpack_from("<H", d, o + 20)[0], struct.unpack_from("<H", d, o + 26)[0]
                return (hi << 16) | lo, struct.unpack_from("<I", d, o + 28)[0]
        return None, None
    x, _ = find(rootclus, b"X68000Z    ")
    im, _ = find(x, b"IMAGE      ") if x else (None, None)
    name = os.path.basename(args.file).upper().replace(".", " ")
    name = (name.split()[0].ljust(8) + name.split()[1].ljust(3)).encode()
    c, size = find(im, name) if im else (None, None)
    if c is None:
        return "no directory entry for %s" % args.file
    want = (size + spc * 512 - 1) // (spc * 512)
    tentative = size == 0xfffffe00     # HDS が開く前に作られた項目 (fork は作り直さない): 鎖は本当の大きさで返る
    n, c0, breaks, last = 0, c, 0, None
    while 2 <= c < 0x0ffffff8 and n <= want + 0x40000:
        nx = fat(c)
        if nx != c + 1 and nx < 0x0ffffff8:
            breaks += 1
        n += 1; last = (c, nx); c = nx
    end = "ends at cluster 0x%x -> 0x%x" % last if last else "empty"
    # x68kzremotedrv の仮想 FAT は大きさより 1 クラスタ長い鎖を返す (fork 670fb84 で確認): 短いときだけ不合格
    return "%s: start cluster 0x%x size 0x%x, chain %d clusters (want %d, %s)%s%s" % (
        args.file, c0, size, n, want, end,
        "" if n == want else (" (+%d extra)" % (n - want) if n > want else
                              " (dir keeps the tentative size; chain follows the real size)" if tentative else " SHORT"),
        ", %d non-contiguous links" % breaks if breaks else "")
try:
    fc = fatcheck()
except Exception as e:
    fc = "fatcheck failed: %s" % e

print("== zfat summary (seed %d, %.0f s)" % (seed, time.time() - T0))
print("   %s" % stats)
print("STATS mismatch=%d eio=%d smbdiff=%d" % (stats["mismatch_sectors"], stats["eio"], len(diff)))
print("   file size 0x%x -> 0x%x; sizes seen %s" % (st0.st_size, st1.st_size, {hex(k): round(v, 1) for k, v in sizes.items()}))
print("   SMB file after the run: %s (%d written sectors)" % ("same" if not diff else "%d DIFFER" % len(diff), len(written)))
print("   umount: rc %s; FAT: %s" % (r.returncode, fc))
if "SHORT" in fc or "failed" in fc or "no directory" in fc:
    bad.append("fat")
if diff:
    bad.append("smb-final")
print("RESULT", "BAD" if bad else "OK", bad[:5])
sys.exit(1 if bad else 0)
