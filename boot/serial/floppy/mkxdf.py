#!/usr/bin/env python3
"""mkxdf.py -- Human68k の 2HD フロッピーイメージ (XDF、1232KB) を作る。
起動セクタ (IPL) は元のシステムディスクの XDF から写し、FAT とディレクトリはここで新しく書く。

usage: mkxdf.py OUT.XDF IPL.XDF TREE
  TREE はホストのディレクトリ。その中身をそのままフロッピーのルートに置く (サブディレクトリも)。
  HUMAN.SYS は最初のクラスタから連続して置く (IPL が読む)。ほかのファイルも連続して置く。
  ファイル名は 8 文字 + 拡張子 3 文字まで。Human68k の拡張名の欄 (12-21 バイト目) を使い、名前の本体は 18 文字まで書ける。
  空き容量 (KB) を標準出力に出す。

2HD (Human68k) の形: 1024 バイト/セクタ、1232 セクタ、1 セクタ/クラスタ、予約 1 (IPL)、FAT 2 セクタ x 2、ルート 192 項目 (6 セクタ)。
"""
import os, struct, sys, time

SEC = 1024
TOTAL = 1232
FATSEC = 2
ROOTENT = 192
ROOTSEC = ROOTENT * 32 // SEC
DATA0 = 1 + 2 * FATSEC + ROOTSEC          # クラスタ 2 のセクタ
NCLUST = TOTAL - DATA0 + 2

out, ipl_src, tree = sys.argv[1:4]
img = bytearray(TOTAL * SEC)
img[0:SEC] = open(ipl_src, "rb").read(SEC)
fat = [0] * NCLUST
fat[0], fat[1] = 0xFFE, 0xFFF               # メディアバイト FE
next_free = 2


def alloc(n):
    """n クラスタを連続して取り、先頭を返す (0 バイトなら 0)"""
    global next_free
    if n == 0:
        return 0
    s = next_free
    if s + n > NCLUST:
        sys.exit("mkxdf: disk full")
    for i in range(n - 1):
        fat[s + i] = s + i + 1
    fat[s + n - 1] = 0xFFF
    next_free += n
    return s


def put_data(c, data):
    off = (DATA0 + c - 2) * SEC
    img[off:off + len(data)] = data


def dosname(name):
    base, _, ext = name.partition(".")
    if "." in ext or len(ext) > 3 or len(base) > 18 or not base:
        sys.exit("mkxdf: bad name %r" % name)
    b = base.encode("cp932")
    return b[:8].ljust(8), ext.encode("cp932").ljust(3), b[8:].ljust(10, b"\0")


def dt(path):
    t = time.localtime(os.path.getmtime(path))
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2),
            (((t.tm_year - 1980) & 0x7f) << 9) | (t.tm_mon << 5) | t.tm_mday)


def entry(name8, ext3, ext10, attr, tm, dm, clus, size):
    return name8 + ext3 + bytes([attr]) + ext10 + struct.pack("<HHHI", tm, dm, clus, size)


def write_dir(hostdir, parent_clus, is_root):
    """hostdir をディレクトリとして書き、(先頭クラスタ) を返す。ルートなら 0"""
    names = sorted(os.listdir(hostdir), key=lambda n: (n.upper() != "HUMAN.SYS", n.upper()))
    names = [n for n in names if not n.startswith(".")]
    ents = []
    if not is_root:
        ents.append(None)   # "." と ".." は後で
        ents.append(None)
    files = []
    for n in names:
        p = os.path.join(hostdir, n)
        files.append((n, p))
    nent = len(files) + (0 if is_root else 2)
    if is_root:
        if nent > ROOTENT:
            sys.exit("mkxdf: too many files in the root")
        my = 0
    else:
        my = alloc(max(1, (nent * 32 + SEC - 1) // SEC))
    body = bytearray()
    if not is_root:
        tm, dm = dt(hostdir)
        body += entry(b".       ", b"   ", b"\0" * 10, 0x10, tm, dm, my, 0)
        body += entry(b"..      ", b"   ", b"\0" * 10, 0x10, tm, dm, parent_clus, 0)
    for n, p in files:
        n8, e3, e10 = dosname(n.upper())
        tm, dm = dt(p)
        if os.path.isdir(p):
            c = write_dir(p, my, False)
            body += entry(n8, e3, e10, 0x10, tm, dm, c, 0)
        else:
            data = open(p, "rb").read()
            c = alloc((len(data) + SEC - 1) // SEC)
            if c:
                put_data(c, data)
            attr = 0x24 if n.upper() in ("HUMAN.SYS",) else 0x20
            body += entry(n8, e3, e10, attr, tm, dm, c, len(data))
    if is_root:
        img[(1 + 2 * FATSEC) * SEC:(1 + 2 * FATSEC) * SEC + len(body)] = body
    else:
        put_data(my, body)
    return my


write_dir(tree, 0, True)
# FAT12 を詰める
f = bytearray(FATSEC * SEC)
for i in range(0, NCLUST, 2):
    a = fat[i]
    b = fat[i + 1] if i + 1 < NCLUST else 0
    o = i * 3 // 2
    f[o] = a & 0xFF
    f[o + 1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4)
    f[o + 2] = (b >> 4) & 0xFF
f[0] = 0xFE
for k in range(2):
    img[(1 + k * FATSEC) * SEC:(1 + (k + 1) * FATSEC) * SEC] = f
open(out, "wb").write(img)
print((NCLUST - next_free) * SEC // 1024)
