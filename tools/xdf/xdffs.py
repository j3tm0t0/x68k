#!/usr/bin/env python3
"""xdffs.py -- Human68k の 2HD フロッピーイメージ (XDF、1232KB、FAT12) の中身を見る・取り出す・作り直す。

usage:
  xdffs.py ls IMG                      全ファイルを一覧にする (属性、大きさ、日時、先頭クラスタ)
  xdffs.py extract IMG DIR             全ファイルをホストの DIR に取り出す (名前は大文字のまま)
  xdffs.py build OUT BASE [操作...]    BASE の起動セクタと中身を元に、FAT とディレクトリを新しく書いた OUT を作る
      --merge IMG     IMG の全ファイルを属性と日時ごと重ねる (同じ名前は置き換え)
      --add HOSTDIR   HOSTDIR の中身をルートに重ねる (サブディレクトリも。名前は大文字にする)
      --rm PATH       ファイルかディレクトリを消す (例: /SYS/OLD.SYS)
    操作は書いた順に行う。HUMAN.SYS を最初のクラスタから置き (IPL が読む)、どのファイルも連続したクラスタに置く。
    空き容量 (KB) を出す。

形は起動セクタの BPB (0x0B から、リトルエンディアン) から読む。X68000 の 2HD は 1024 バイト/セクタ、1 セクタ/クラスタ、
予約 1 (IPL)、FAT 2 セクタ x 2、ルート 192 項目、メディアバイト FE。
ディレクトリ項目は Human68k の形: 名前 8 + 拡張子 3 + 属性 1 + 名前の続き 10 (12-21 バイト目、0 埋め) + 時刻 + 日付 + クラスタ + 大きさ。
"""
import os, struct, sys, time


class Ent:
    def __init__(self, n8, e3, e10, attr, tm, dm, data=b"", kids=None):
        self.n8, self.e3, self.e10, self.attr, self.tm, self.dm = n8, e3, e10, attr, tm, dm
        self.data, self.kids = data, kids          # kids はディレクトリのとき項目の一覧

    @property
    def name(self):
        base = (self.n8.rstrip(b" ") + self.e10.rstrip(b"\0")).decode("cp932")
        ext = self.e3.rstrip(b" ").decode("cp932")
        return base + ("." + ext if ext else "")

    def isdir(self):
        return self.attr & 0x10

    def isvol(self):
        return self.attr & 0x08


class Img:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.raw = d
        self.sec, self.spc, self.res = struct.unpack_from("<HBH", d, 0x0B)
        self.nfat, self.nroot, self.total, self.media, self.fatsec = struct.unpack_from("<BHHBH", d, 0x10)
        if self.sec * self.total != len(d):
            sys.exit("xdffs: %s: BPB says %d x %d bytes, file is %d" % (path, self.total, self.sec, len(d)))
        self.fat = d[self.res * self.sec:(self.res + self.fatsec) * self.sec]
        self.root_off = (self.res + self.nfat * self.fatsec) * self.sec
        self.data0 = self.root_off + self.nroot * 32
        self.csize = self.sec * self.spc
        self.ncl = (len(d) - self.data0) // self.csize + 2
        self.root = self.parse(d[self.root_off:self.data0], set())

    def nxt(self, c):
        o = c * 3 // 2
        v = self.fat[o] | self.fat[o + 1] << 8
        return (v >> 4) if c & 1 else (v & 0xFFF)

    def chain(self, c, seen):
        out = []
        while 2 <= c < 0xFF8:
            if c >= self.ncl or c in seen:
                sys.exit("xdffs: broken FAT chain at cluster %d" % c)
            seen.add(c)
            out.append(c)
            c = self.nxt(c)
        return out

    def read(self, c, seen):
        return b"".join(self.raw[self.data0 + (x - 2) * self.csize:self.data0 + (x - 1) * self.csize]
                        for x in self.chain(c, seen))

    def parse(self, body, seen):
        ents = []
        for i in range(0, len(body), 32):
            e = body[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[0:2] in (b". ", b".."):
                continue
            tm, dm, c, size = struct.unpack_from("<HHHI", e, 22)
            x = Ent(e[0:8], e[8:11], e[12:22], e[11], tm, dm)
            if x.isvol():
                pass
            elif x.isdir():
                x.kids = self.parse(self.read(c, seen), seen)
            else:
                x.data = self.read(c, seen)[:size] if c else b""
                if len(x.data) != size:
                    sys.exit("xdffs: %s: %d bytes in the chain, %d in the entry" % (x.name, len(x.data), size))
            ents.append(x)
        return ents


def walk(ents, prefix=""):
    for x in ents:
        yield prefix + "/" + x.name, x
        if x.isdir():
            yield from walk(x.kids, prefix + "/" + x.name)


def find(ents, name):
    for i, x in enumerate(ents):
        if x.name.upper() == name.upper():
            return i
    return -1


def dosname(name):
    base, _, ext = name.partition(".")
    if "." in ext or len(ext) > 3 or len(base.encode("cp932")) > 18 or not base:
        sys.exit("xdffs: name %r does not fit Human68k (18 + 3)" % name)
    b = base.encode("cp932")
    return b[:8].ljust(8), ext.encode("cp932").ljust(3), b[8:].ljust(10, b"\0")


def dostime(t):
    t = time.localtime(t)
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2),
            (((t.tm_year - 1980) & 0x7f) << 9) | (t.tm_mon << 5) | t.tm_mday)


def put(ents, x):
    """x を ents に重ねる。同じ名前があれば置き換え (ディレクトリ同士なら中身を重ねる)"""
    i = find(ents, x.name)
    if i < 0:
        ents.append(x)
    elif ents[i].isdir() and x.isdir():
        for k in x.kids:
            put(ents[i].kids, k)
    else:
        ents[i] = x


def from_host(path):
    name = os.path.basename(path).upper()
    tm, dm = dostime(os.path.getmtime(path))
    n8, e3, e10 = dosname(name)
    if os.path.isdir(path):
        kids = [from_host(os.path.join(path, n)) for n in sorted(os.listdir(path)) if not n.startswith(".")]
        return Ent(n8, e3, e10, 0x10, tm, dm, kids=kids)
    return Ent(n8, e3, e10, 0x20, tm, dm, data=open(path, "rb").read())


def remove(ents, path):
    parts = [p for p in path.split("/") if p]
    for p in parts[:-1]:
        i = find(ents, p)
        if i < 0 or not ents[i].isdir():
            sys.exit("xdffs: %s not found" % path)
        ents = ents[i].kids
    i = find(ents, parts[-1])
    if i < 0:
        sys.exit("xdffs: %s not found" % path)
    del ents[i]


def build(out, base, root):
    sec, ncl, csize = base.sec, base.ncl, base.csize
    img = bytearray(len(base.raw))
    img[0:base.res * sec] = base.raw[0:base.res * sec]       # 起動セクタ (IPL と BPB)
    fat = [0] * ncl
    fat[0], fat[1] = 0xF00 | base.media, 0xFFF
    state = {"next": 2}

    def alloc(nbytes):
        n = (nbytes + csize - 1) // csize
        if n == 0:
            return 0
        s = state["next"]
        if s + n > ncl:
            sys.exit("xdffs: disk full (%d KB too much)" % ((s + n - ncl) * csize // 1024))
        for i in range(n - 1):
            fat[s + i] = s + i + 1
        fat[s + n - 1] = 0xFFF
        state["next"] += n
        return s

    def put_data(c, data):
        o = base.data0 + (c - 2) * csize
        img[o:o + len(data)] = data

    def entry(x, c, size):
        return x.n8 + x.e3 + bytes([x.attr]) + x.e10 + struct.pack("<HHHI", x.tm, x.dm, c, size)

    def write_dir(ents, my, parent, is_root):
        body = bytearray()
        # 子のクラスタを先に決める (ファイルは中身を置き、ディレクトリは後で中を書く)。
        # HUMAN.SYS のクラスタを最初に取る (IPL はルートの HUMAN.SYS をクラスタ 2 から連続して読む)。項目の並びは変えない
        recs = {}
        for x in sorted(ents, key=lambda x: x.name.upper() != "HUMAN.SYS"):
            if x.isvol():
                recs[id(x)] = (x, 0, 0)
            elif x.isdir():
                recs[id(x)] = (x, alloc((len(x.kids) + 2) * 32), 0)
            else:
                c = alloc(len(x.data))
                if c:
                    put_data(c, x.data)
                recs[id(x)] = (x, c, len(x.data))
        recs = [recs[id(x)] for x in ents]
        if not is_root:
            dot = Ent(b".       ", b"   ", b"\0" * 10, 0x10, cur.tm, cur.dm)
            dotdot = Ent(b"..      ", b"   ", b"\0" * 10, 0x10, cur.tm, cur.dm)
            body += entry(dot, my, 0) + entry(dotdot, parent, 0)
        for x, c, size in recs:
            body += entry(x, c, size)
        if is_root:
            if len(recs) > base.nroot:
                sys.exit("xdffs: %d entries in the root, at most %d" % (len(recs), base.nroot))
            img[base.root_off:base.root_off + len(body)] = body
        else:
            put_data(my, body)
        for x, c, size in recs:
            if x.isdir():
                write_dir_of(x, c, my)

    def write_dir_of(x, c, parent):
        nonlocal cur
        save, cur = cur, x
        write_dir(x.kids, c, parent, False)
        cur = save

    cur = None
    write_dir(root, 0, 0, True)

    f = bytearray(base.fatsec * sec)
    for i in range(ncl):
        o = i * 3 // 2
        if i & 1:
            f[o] = (f[o] & 0x0F) | ((fat[i] & 0x0F) << 4)
            f[o + 1] = fat[i] >> 4
        else:
            f[o] = fat[i] & 0xFF
            f[o + 1] = (f[o + 1] & 0xF0) | (fat[i] >> 8)
    for k in range(base.nfat):
        o = (base.res + k * base.fatsec) * sec
        img[o:o + len(f)] = f
    open(out, "wb").write(img)
    return (ncl - state["next"]) * csize // 1024


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "ls":
        im = Img(a[1])
        for p, x in walk(im.root):
            t = "%04d-%02d-%02d %02d:%02d" % ((x.dm >> 9) + 1980, (x.dm >> 5) & 15, x.dm & 31, x.tm >> 11, (x.tm >> 5) & 63)
            kind = "<VOL>" if x.isvol() else "<DIR>" if x.isdir() else "%8d" % len(x.data)
            print("%02x %8s %s %s" % (x.attr, kind, t, p))
        return
    if len(a) == 3 and a[0] == "extract":
        im = Img(a[1])
        for p, x in walk(im.root):
            if x.isvol():
                continue
            hp = os.path.join(a[2], *p.split("/"))
            if x.isdir():
                os.makedirs(hp, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(hp), exist_ok=True)
                open(hp, "wb").write(x.data)
        return
    if len(a) >= 3 and a[0] == "build":
        out, base = a[1], Img(a[2])
        root = base.root
        ops = a[3:]
        while ops:
            op, arg = ops[0], ops[1]
            ops = ops[2:]
            if op == "--merge":
                for x in Img(arg).root:
                    if not x.isvol():
                        put(root, x)
            elif op == "--add":
                for n in sorted(os.listdir(arg)):
                    if not n.startswith("."):
                        put(root, from_host(os.path.join(arg, n)))
            elif op == "--rm":
                remove(root, arg)
            else:
                sys.exit("xdffs: unknown option %s" % op)
        print("%d KB free" % build(out, base, root))
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
