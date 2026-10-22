#!/usr/bin/env python3
"""68000 の ChaCha20 chacha68k_xor_asm を生成する (GNU as、%レジスタ構文)。
  void chacha68k_xor_asm(uint32_t x[16], const unsigned char *in, unsigned char *out, unsigned long nblocks);
64 バイトのブロックを nblocks 個: out = in ^ 鍵ストリーム、x[12..13] (64 ビットのカウンタ) をブロックごとに進める。
in / out は偶数番地 (C 側の chacha68k_xor が確かめる)。

20 ラウンドは完全展開 (80 クォーターラウンド)。16 語に対してレジスタは %d0-%d7 と %a0-%a6 の 15 本なので、
生成器がレジスタ割り当てを追跡する:
  - a += b / c += d: 足される側はレジスタ (An でも Dn でも。adda.l は Dn と同じ 8 クロック)
  - d = rotl(d ^ a, n): EOR のソースは Dn に限るので、a が An のときは空き Dn に写して eor.l Dd,Df とし、
    結果は Df に置く (旧 Dd が空く = 名前の付け替え)。回転は Dn で: 16 = swap、12 = swap + ror.l #4、
    8 = rol.l #8、7 = rol.l #7
  - 足りないときは「次に使うのがいちばん先の語」を追い出す (空き An へ move、無ければスタックの語の置き場へ)
ラウンドの後は全レジスタをスタックに退避し、語ごとに x を足してバイト順を入れ替え (rol.w #8 / swap / rol.w #8)、
入力と XOR して書く (鍵ストリームはリトルエンディアン)。
"""
import sys

ROUNDS = 20
COLS = [(0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15)]
DIAG = [(0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)]

# 演算列: ("add", dst, src) / ("xr", dst, src, n)
OPS = []
for _ in range(ROUNDS // 2):
    for a, b, c, d in COLS + DIAG:
        OPS += [("add", a, b), ("xr", d, a, 16), ("add", c, d), ("xr", b, c, 12),
                ("add", a, b), ("xr", d, a, 8), ("add", c, d), ("xr", b, c, 7)]

DREGS = ["%%d%d" % i for i in range(8)]
AREGS = ["%%a%d" % i for i in range(7)]
# スタック: 語の置き場 SLOT(w) = 4*w (64 バイト)、退避 TMP 60 バイト、引数の写し
SLOT0 = 0
TMP = 64
LOCALS = TMP + 60
SAVED = 11 * 4
ARG = LOCALS + SAVED + 4

code = []
cyc = [0]
def o(s, c=0):
    code.append("\t" + s + "\n"); cyc[0] += c

def slot(w): return "%d(%%sp)" % (SLOT0 + 4 * w)

class Alloc:
    def __init__(self, layout):
        self.loc = dict(layout)          # 語 -> レジスタ名 / "m"
        self.pos = 0                     # OPS の現在位置
    def holder(self, r):
        for w, l in self.loc.items():
            if l == r: return w
        return None
    def free(self, cls):
        regs = DREGS if cls == "d" else AREGS
        return [r for r in regs if self.holder(r) is None]
    def nextuse(self, w):
        for i in range(self.pos, len(OPS)):
            if w in OPS[i][1:3]: return i
        return 10 ** 9
    def victim(self, cls, keep):
        regs = DREGS if cls == "d" else AREGS
        cands = [(self.nextuse(self.holder(r)), r) for r in regs if self.holder(r) is not None and self.holder(r) not in keep]
        return max(cands)[1]
    def evict(self, r, keep):
        # r を空ける: 中身を空き An / 空き Dn / スタックへ (先の用途で選ぶ)
        w = self.holder(r)
        fa = [x for x in self.free("a")]
        fd = [x for x in self.free("d") if x != r]
        if r.startswith("%d") and fa:
            o("move.l\t%s,%s" % (r, fa[0]), 4); self.loc[w] = fa[0]
        elif r.startswith("%a") and fd and len(fd) > 1:
            o("move.l\t%s,%s" % (r, fd[0]), 4); self.loc[w] = fd[0]
        else:
            o("move.l\t%s,%s" % (r, slot(w)), 16); self.loc[w] = "m"
    def get_free(self, cls, keep):
        f = self.free(cls)
        if f: return f[0]
        r = self.victim(cls, keep)
        self.evict(r, keep)
        return r
    def to_reg(self, w, keep, prefer="a"):
        # w をレジスタに (メモリにあれば)。An が空いていれば An、無ければ Dn
        if self.loc[w] != "m": return self.loc[w]
        fa, fd = self.free("a"), self.free("d")
        if prefer == "a" and fa: r = fa[0]
        elif fd and (prefer == "d" or len(fd) > 1): r = fd[0]
        elif fa: r = fa[0]
        else: r = self.get_free("a", keep | {w})
        o(("movea.l" if r.startswith("%a") else "move.l") + "\t%s,%s" % (slot(w), r), 16)
        self.loc[w] = r
        return r
    def to_dreg(self, w, keep):
        l = self.loc[w]
        if l.startswith("%d"): return l
        r = self.get_free("d", keep | {w})
        l = self.loc[w]
        o("move.l\t%s,%s" % (slot(w) if l == "m" else l, r), 16 if l == "m" else 4)
        self.loc[w] = r
        return r

def rot(r, n):
    if n == 16: o("swap\t%s" % r, 4)
    elif n == 12: o("swap\t%s" % r, 4); o("ror.l\t#4,%s" % r, 16)
    elif n == 8: o("rol.l\t#8,%s" % r, 24)
    elif n == 7: o("rol.l\t#7,%s" % r, 22)

def gen_rounds(al):
    for i, op in enumerate(OPS):
        al.pos = i
        if op[0] == "add":
            _, dst, src = op
            keep = {dst, src}
            rs = al.to_reg(src, keep, prefer="d")
            rd = al.to_reg(dst, keep, prefer="a")
            o(("adda.l" if rd.startswith("%a") else "add.l") + "\t%s,%s" % (rs, rd), 8)
        else:
            _, dst, src, n = op
            keep = {dst, src}
            ls, ld = al.loc[src], al.loc[dst]
            if ls.startswith("%d") and ld.startswith("%d"):
                o("eor.l\t%s,%s" % (ls, ld), 8); r = ld
            elif ld.startswith("%d"):
                # src を空き Dn に写し、そこに dst を eor (結果は空き Dn 側、旧 dst のレジスタが空く)
                f = al.get_free("d", keep)
                ls = al.loc[src]
                o("move.l\t%s,%s" % (slot(src) if ls == "m" else ls, f), 16 if ls == "m" else 4)
                o("eor.l\t%s,%s" % (al.loc[dst], f), 8)
                al.loc[dst] = f
                # src は元の場所のまま (値は変わらない)
                r = f
            elif ls.startswith("%d"):
                # dst を Dn に写して eor
                rd = al.to_dreg(dst, keep)
                o("eor.l\t%s,%s" % (al.loc[src], rd), 8); r = rd
            else:
                rs = al.to_dreg(src, keep)
                rd = al.to_dreg(dst, keep)
                o("eor.l\t%s,%s" % (rs, rd), 8); r = rd
            rot(r, n)

def main():
    # 初期配置: movem.l (x),%d0-%d7/%a0-%a6 で x0..x14、x15 はスタックの置き場
    layout = {}
    for w in range(15):
        layout[w] = DREGS[w] if w < 8 else AREGS[w - 8]
    layout[15] = "m"
    al = Alloc(layout)
    o(".text"); o(".globl\tchacha68k_xor_asm")
    code.append("chacha68k_xor_asm:\n")
    o("movem.l\t%d2-%d7/%a2-%a6,-(%sp)")
    o("lea\t-%d(%%sp),%%sp" % LOCALS)
    o("tst.l\t%d(%%sp)" % (ARG + 12))
    o("beq\t.Lc_ret")
    code.append(".Lc_loop:\n")
    c0 = cyc[0]
    o("move.l\t%d(%%sp),%%a6" % ARG, 16)
    o("move.l\t60(%%a6),%s" % slot(15), 24)
    o("movem.l\t(%a6),%d0-%d7/%a0-%a6", 12 + 8 * 15)
    gen_rounds(al)
    rc = cyc[0] - c0
    # 退避: レジスタにある語を TMP へ (movem の順 = %d0..%d7, %a0..%a6)
    o("movem.l\t%%d0-%%d7/%%a0-%%a6,%d(%%sp)" % TMP, 12 + 8 * 15)
    where = {}
    for w in range(16):
        l = al.loc[w]
        if l == "m": where[w] = SLOT0 + 4 * w
        else:
            k = DREGS.index(l) if l.startswith("%d") else 8 + AREGS.index(l)
            where[w] = TMP + 4 * k
    o("move.l\t%d(%%sp),%%a0" % ARG, 16)          # x
    o("move.l\t%d(%%sp),%%a1" % (ARG + 4), 16)    # in
    o("move.l\t%d(%%sp),%%a2" % (ARG + 8), 16)    # out
    for w in range(16):
        o("move.l\t%d(%%sp),%%d0" % where[w], 16)
        o("add.l\t(%a0)+,%d0", 14)
        o("rol.w\t#8,%d0", 22); o("swap\t%d0", 4); o("rol.w\t#8,%d0", 22)
        o("move.l\t(%a1)+,%d1", 12)
        o("eor.l\t%d0,%d1", 8)
        o("move.l\t%d1,(%a2)+", 12)
    o("move.l\t%%a1,%d(%%sp)" % (ARG + 4), 16)
    o("move.l\t%%a2,%d(%%sp)" % (ARG + 8), 16)
    # カウンタ x[12..13] (%a0 は x+64)
    o("addq.l\t#1,-16(%a0)", 20)
    o("bcc\t1f", 10)
    o("addq.l\t#1,-12(%a0)")
    code.append("1:\n")
    o("subq.l\t#1,%d(%%sp)" % (ARG + 12), 24)
    o("bne\t.Lc_loop", 10)
    code.append(".Lc_ret:\n")
    o("lea\t%d(%%sp),%%sp" % LOCALS)
    o("movem.l\t(%sp)+,%d2-%d7/%a2-%a6")
    o("rts")
    # 1 ブロックの見積もり (ラウンド部 + 後処理)
    sys.stdout.write("| chacha68k.s -- gen_chacha.py で生成 (手で直さない)。見積もり: ラウンド %d + 全体 %d クロック/ブロック\n"
                     % (rc, cyc[0] - c0))
    sys.stdout.write("".join(code))

main()
