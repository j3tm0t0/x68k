#!/usr/bin/env python3
"""頑健性試験: 壊れた/極端な制御シーケンスを vthost (ASan/UBSan 版) に食わせ、落ちない・止まらないことを確かめる。
usage: fuzz.py VTHOST_ASAN OUTDIR [N]
長い CSI 引数、多すぎる引数、終端の無い OSC/DCS、巨大な IL/DL/ICH/DCH/ECH/SU/SD、壊れた UTF-8、
全角と右端、スクロール領域の出入りなどを乱数で混ぜる。各入力を 1 秒で打ち切り、異常終了か時間切れなら失敗。"""
import os, random, subprocess, sys

E = b"\x1b"
CSI = E + b"["


def num(rnd):
    return rnd.choice([b"", b"0", b"1", b"2", b"5", b"31", b"32", b"95", b"96", b"97", b"255", b"9999",
                       b"65535", b"2147483647", b"4294967296", b"9" * 40, b"-1"])


def piece(rnd):
    k = rnd.randrange(16)
    if k == 0:   # CSI 一般 (前置・中間・終端を乱数で)
        pre = rnd.choice([b"", b"?", b">", b"<", b"=", b"!"])
        params = b";".join(num(rnd) for _ in range(rnd.randrange(0, 40)))
        inter = rnd.choice([b"", b"", b" ", b"$", b"\"", b"'"])
        return CSI + pre + params + inter + bytes([rnd.randrange(0x40, 0x7f)])
    if k == 1:   # 行・文字の挿入削除・スクロール
        return CSI + num(rnd) + rnd.choice([b"L", b"M", b"@", b"P", b"X", b"S", b"T", b"J", b"K", b"r", b"A", b"B", b"C", b"D", b"G", b"d", b"E", b"F"])
    if k == 2:
        return CSI + num(rnd) + b";" + num(rnd) + rnd.choice([b"H", b"r", b"f"])
    if k == 3:   # SGR (256/24bit 引数不足を含む)
        return CSI + b";".join(rnd.choice([b"38", b"48", b"5", b"2", num(rnd)]) for _ in range(rnd.randrange(1, 12))) + b"m"
    if k == 4:   # OSC / DCS / APC (終端あり・なし・ESC 打ち切り)
        head = rnd.choice([b"]0;", b"]8;;", b"]133;A", b"]10;?", b"]11;?", b"P", b"Ptmux;", b"_", b"^", b"X"])
        body = bytes(rnd.randrange(0x20, 0x7f) for _ in range(rnd.choice([0, 5, 100, 3000])))
        tail = rnd.choice([b"\x07", E + b"\\", E + E, b"", E + b"[", b"\x18"])
        return E + head + body + tail
    if k == 5:   # 壊れた UTF-8
        return bytes(rnd.randrange(0x80, 0x100) for _ in range(rnd.randrange(1, 6)))
    if k == 6:
        return rnd.choice(["漢字", "日本語テキスト", "😀", "👍🏽", "é", "​", "⏺", "✻", "️", "𠀋", "ｱ"]).encode()
    if k == 7:
        return rnd.choice([b"\r", b"\n", b"\t", b"\x08", b"\x0e", b"\x0f", b"\x07", b"\x0b", b"\x0c", b"\x00"])
    if k == 8:
        return E + bytes([rnd.randrange(0x20, 0x7f)])
    if k == 9:
        return E + rnd.choice([b"(0", b")0", b"(B", b"#8", b"%G", b" F", b"7", b"8", b"M", b"D", b"E", b"c", b"H"])
    if k == 10:
        return CSI + b"?" + rnd.choice([b"1049", b"47", b"1047", b"1048", b"25", b"7", b"1", b"2026", b"2004", b"1000", b"1006"]) + rnd.choice([b"h", b"l", b"$p"])
    if k == 11:
        return CSI + b"0" * rnd.randrange(1, 2000) + b"5H"
    return bytes(rnd.randrange(0x20, 0x7f) for _ in range(rnd.randrange(1, 120)))


def main():
    vthost, outdir = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 150
    os.makedirs(outdir, exist_ok=True)
    fails = 0
    for seed in range(n):
        rnd = random.Random(seed)
        data = b"".join(piece(rnd) for _ in range(rnd.randrange(50, 400)))
        rec = os.path.join(outdir, "fuzz%03d.rec" % seed)
        open(rec, "wb").write(data)
        size = rnd.choice(["96x32", "80x24", "10x5", "1x1", "96x2"])
        chunk = str(rnd.choice([1, 3, 64, 4096]))
        try:
            r = subprocess.run([vthost, "-c", chunk, "-s", size, "-r", os.devnull, rec, os.devnull],
                               capture_output=True, timeout=5)
            if r.returncode != 0:
                fails += 1
                print("  NG seed %d (%s -c %s): 終了コード %d\n%s" % (seed, size, chunk, r.returncode, r.stderr.decode(errors="replace")[:800]))
                continue
        except subprocess.TimeoutExpired:
            fails += 1
            print("  NG seed %d (%s -c %s): 5 秒で終わらない" % (seed, size, chunk))
            continue
        os.remove(rec)
    print("fuzz: %d/%d OK" % (n - fails, n))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
