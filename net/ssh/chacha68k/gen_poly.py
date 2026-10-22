#!/usr/bin/env python3
"""68000 の Poly1305 ブロック関数 poly68k_blocks_asm を生成する (GNU as、%レジスタ構文)。
  void poly68k_blocks_asm(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit);
アルゴリズムは cp68k.c の C 版 poly68k_blocks と同じ (radix 2^16 の comba + 2^130=5 の畳み込み)。

レジスタ:
  %d4-%d7  h の下位 128 ビット (32 ビットずつ、%d4 が最下位)。各レジスタの下位語と上位語が h のリム 2 つ
           (h_2k, h_2k+1)。MULU.W は下位語しか見ないので、上位語のリムは swap で下ろして使う (状態を追跡)。
  %a0-%a6  r0..r6 (movea.w、下位語が r_j)。r7 はスタック。積は move.w %aj,%d0 ; mulu.w %dh,%d0。
  %d1:%d2  64 ビットのアキュムレータ (下位:上位)、%d3 = 0 (addx 用)、%d0 = 積
  h8 (h の 128 ビット以上、< 16) はスタックの語。MULU の時間は 38+2n (n = ソースの 1 のビット数) なので
  h8 をソースにする積は速い。
"""
import sys
out = []
def o(s): out.append("\t" + s + "\n")
def label(s): out.append(s + ":\n")

# スタックのローカル (movem で 11 本退避した後に lea -LOC)
P_BASE = 0          # P0..P15 (k の対ごとに長語: P_{2m+1} が上位語)
R7 = 32
H8 = 34
MP = 36             # メッセージのポインタ
CNT = 40
HIBIT = 44          # 語
LOC = 48
SAVED = 11 * 4
ARG = LOC + SAVED + 4   # 第 1 引数
def P(k):           # P_k の番地: 対 m = k//2 の長語 (上位語 = 奇数 k)
    return P_BASE + 4 * (k // 2) + (0 if k & 1 else 2)

def bswap(r):       # リトルエンディアンの長語を数値に
    o("rol.w\t#8,%s" % r); o("swap\t%s" % r); o("rol.w\t#8,%s" % r)

o(".text")
o(".globl\tpoly68k_blocks_asm")
label("poly68k_blocks_asm")
o("movem.l\t%d2-%d7/%a2-%a6,-(%sp)")
o("lea\t-%d(%%sp),%%sp" % LOC)
o("move.l\t%d(%%sp),%%a1" % ARG)              # st
o("move.l\t%d(%%sp),%d(%%sp)" % (ARG + 4, MP))
o("move.l\t%d(%%sp),%%d0" % (ARG + 8))
o("beq\t.Lp_ret0")
o("move.l\t%%d0,%d(%%sp)" % CNT)
o("move.w\t%d(%%sp),%d(%%sp)" % (ARG + 12 + 2, HIBIT))
o("movem.l\t(%a1),%d4-%d7")                   # h[0..3]
o("move.w\t18(%%a1),%d(%%sp)" % H8)           # h4 (長語の下位語)
o("move.w\t34(%%a1),%d(%%sp)" % R7)
for j in (0, 2, 3, 4, 5, 6, 1):                # r0..r6 (st を指す %a1 は最後)
    o("movea.w\t%d(%%a1),%%a%d" % (20 + 2 * j, j))
o("moveq\t#0,%d3")

label(".Lp_loop")
# --- h += m + hibit * 2^128 --- (%a6 を一時的にメッセージのポインタに)
o("move.l\t%d(%%sp),%%a6" % MP)
for i, r in enumerate(["%d4", "%d5", "%d6", "%d7"]):
    o("move.l\t(%a6)+,%d0"); bswap("%d0")       # move/rol/swap は X を変えない
    o(("add.l" if i == 0 else "addx.l") + "\t%%d0,%s" % r)
o("move.w\t%d(%%sp),%%d0" % H8)
o("move.w\t%d(%%sp),%%d1" % HIBIT)
o("addx.w\t%d1,%d0")
o("move.w\t%%d0,%d(%%sp)" % H8)
o("move.l\t%%a6,%d(%%sp)" % MP)
o("move.l\t%d(%%sp),%%a6" % ARG)              # st
o("movea.w\t32(%a6),%a6")                       # r6

# --- comba ---
low = {4: 0, 5: 0, 6: 0, 7: 0}                  # レジスタ -> 下位語にあるリム (0 = 偶数)
nswap = 0
first = True
def prod(i, j):
    global first
    src = "%d(%%sp)" % R7 if j == 7 else "%%a%d" % j
    dst = "%d1" if first else "%d0"
    o("move.w\t%s,%s" % (src, dst))
    if i == 8:
        o("mulu.w\t%d(%%sp),%s" % (H8, dst))
    else:
        o("mulu.w\t%%d%d,%s" % (4 + i // 2, dst))
    if first:
        o("moveq\t#0,%d2")
        first = False
    else:
        o("add.l\t%d0,%d1"); o("addx.l\t%d3,%d2")

for k in range(16):
    i0, i1 = max(0, k - 7), min(k, 8)
    need = list(range(i0, i1 + 1))
    regs = sorted(set(4 + i // 2 for i in need if i < 8))
    for rg in regs:
        limbs = [i for i in need if i < 8 and 4 + i // 2 == rg]
        cur = [i for i in limbs if i % 2 == low[rg]]
        oth = [i for i in limbs if i % 2 != low[rg]]
        for i in cur:
            prod(i, k - i)
        for i in oth:
            o("swap\t%%d%d" % rg); nswap += 1; low[rg] ^= 1
            prod(i, k - i)
    if 8 in need:
        prod(8, k - 8)
    o("move.w\t%%d1,%d(%%sp)" % P(k))
    if k < 15:
        o("clr.w\t%d1"); o("swap\t%d1"); o("swap\t%d2"); o("add.l\t%d2,%d1")
        o("moveq\t#0,%d2")

# --- 畳み込み: h' = L + 2^128 (P8 & 3) + M + (M >> 2)、M = (P >> 128) & ~3 ---
o("movem.l\t%d(%%sp),%%d4-%%d7" % (P_BASE + 16))   # P9:P8, P11:P10, P13:P12, P15:P14
o("moveq\t#3,%d0")
o("and.w\t%d4,%d0")                             # q = P8 & 3
o("and.w\t#-4,%d4")                             # M
for _ in range(2):                              # H = M >> 2
    o("lsr.l\t#1,%d7"); o("roxr.l\t#1,%d6"); o("roxr.l\t#1,%d5"); o("roxr.l\t#1,%d4")
o("move.l\t%d(%%sp),%%d1" % (P_BASE + 16))
o("and.w\t#-4,%d1")
o("add.l\t%d1,%d4")                             # T = H + M
for n, r in ((20, "%d5"), (24, "%d6"), (28, "%d7")):
    o("move.l\t%d(%%sp),%%d1" % (P_BASE + n)); o("addx.l\t%%d1,%s" % r)
o("addx.w\t%d3,%d0")
o("move.l\t%d(%%sp),%%d1" % P_BASE)
o("add.l\t%d1,%d4")                             # h = L + T
for n, r in ((4, "%d5"), (8, "%d6"), (12, "%d7")):
    o("move.l\t%d(%%sp),%%d1" % (P_BASE + n)); o("addx.l\t%%d1,%s" % r)
o("addx.w\t%d3,%d0")
o("move.w\t%%d0,%d(%%sp)" % H8)
o("subq.l\t#1,%d(%%sp)" % CNT)
o("bne\t.Lp_loop")

o("move.l\t%d(%%sp),%%a0" % ARG)
o("movem.l\t%d4-%d7,(%a0)")
o("moveq\t#0,%d0")
o("move.w\t%d(%%sp),%%d0" % H8)
o("move.l\t%d0,16(%a0)")
label(".Lp_ret0")
o("lea\t%d(%%sp),%%sp" % LOC)
o("movem.l\t(%sp)+,%d2-%d7/%a2-%a6")
o("rts")

sys.stdout.write("| poly68k.s -- gen_poly.py で生成 (手で直さない)。swap %d 回/ブロック\n" % nswap)
sys.stdout.write("".join(out))
