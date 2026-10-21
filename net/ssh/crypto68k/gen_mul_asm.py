#!/usr/bin/env python3
"""68000 の comba 乗算 fe_mul16 / fe_sqr16 を生成する (完全展開, GNU as %レジスタ構文)。
  void fe_mul16(uint16_t r[32], const uint16_t a[16], const uint16_t b[16]);
  void fe_sqr16(uint16_t r[32], const uint16_t a[16]);
radix 2^16, 16 リム。積は MULU.W (16x16->32)。64bit アキュムレータ (%d4:%d3) に足し、
列ごとに下位 16bit を r[] に出し 16bit 右シフト。値 <2^37 なので %d4<2^5、swap で畳める。
usage: gen_mul_asm.py > mul68k.s
"""
import sys
def o(s): sys.stdout.write(s)

def prologue(name, nargs):
    # クロバーする callee-saved レジスタ (d3,d4,d7,a2) だけ個別に退避する。
    # (movem.l は Unicorn の m68k で実行できないため個別 move。実機でも同じく動く)
    o("\t.globl\t%s\n%s:\n" % (name, name))
    o("\tmove.l\t%d3,-(%sp)\n\tmove.l\t%d4,-(%sp)\n\tmove.l\t%d7,-(%sp)\n\tmove.l\t%a2,-(%sp)\n")
    off = 16 + 4   # 4 regs saved + return addr
    o("\tmove.l\t%d(%%sp),%%a0\n" % off)
    o("\tmove.l\t%d(%%sp),%%a1\n" % (off + 4))
    if nargs == 3:
        o("\tmove.l\t%d(%%sp),%%a2\n" % (off + 8))
    else:
        o("\tmove.l\t%a1,%a2\n")
    o("\tmoveq\t#0,%d7\n\tmoveq\t#0,%d3\n\tmoveq\t#0,%d4\n")

def column(k, square):
    i0, i1 = max(0, k - 15), min(k, 15)
    pairs = [(i, k - i) for i in range(i0, i1 + 1)]
    if square:
        seen = {}
        for (i, j) in pairs:
            key = tuple(sorted((i, j)))
            seen[key] = seen.get(key, 0) + 1
        items = [(key[0], key[1], cnt) for key, cnt in seen.items()]
    else:
        items = [(i, j, 1) for (i, j) in pairs]
    for (i, j, cnt) in items:
        o("\tmove.w\t%d(%%a1),%%d0\n" % (2 * i))
        o("\tmove.w\t%d(%%a2),%%d1\n" % (2 * j))
        o("\tmulu.w\t%d1,%d0\n")
        # 積を 64bit アキュムレータに足す。二乗の 2 倍は「2 回足す」で行う
        # (add.l %d0,%d0 は積が ~2^32 のとき 33bit 目が溢れて失われるため不可)。
        o("\tadd.l\t%d0,%d3\n\taddx.l\t%d7,%d4\n")
        if cnt == 2:
            o("\tadd.l\t%d0,%d3\n\taddx.l\t%d7,%d4\n")
    o("\tmove.w\t%d3,(%a0)+\n")
    o("\tlsr.l\t#8,%d3\n\tlsr.l\t#8,%d3\n\tswap\t%d4\n\tor.l\t%d4,%d3\n\tmoveq\t#0,%d4\n")

def gen(name, nargs, square):
    prologue(name, nargs)
    for k in range(31):
        column(k, square)
    o("\tmove.w\t%d3,(%a0)+\n")
    o("\tmove.l\t(%sp)+,%a2\n\tmove.l\t(%sp)+,%d7\n\tmove.l\t(%sp)+,%d4\n\tmove.l\t(%sp)+,%d3\n\trts\n\n")

o("\t.text\n")
gen("fe_mul16", 3, False)
gen("fe_sqr16", 2, True)
