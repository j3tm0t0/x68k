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


# ======================================================================================
# 表引き版 (長いメッセージ用)。r はパケットごとに決まるので、パケットの最初に
#   TA[b] = b * r、TB[b] = b * (r << 8)   (b = 0..255、1 項 32 バイト)
# を作り、h のバイト k ごとに T[h_k] を 8k ビットずらして足す (MULU を使わない)。項の並び (ビッグエンディアン):
#   +4: V4  +8: V3  +12: V2  +16: V1  +20: V0  +24: 0 (語)
# +24 から -(An) で 5 長語読むと V そのもの、+26 から読むと V << 16 になる (語境界なので 68000 でも読める)。
# よってバイト k (位置 8k = 32q + 8t) は t=0: TA+24、t=1: TB+24、t=2: TA+26、t=3: TB+26 を長語 q から足す。
# 途中の和は (h mod 2^8(k+1)) * r < 2^(8k+132) なので、長語 q..q+4 の外へは桁上げしない。
#   void poly68k_table_asm(uint32_t *tab, const uint32_t R[5]);   tab[b*8 ..] = b * R (R[0] が最下位)
#   void poly68k_blocks_tab(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit,
#                           const uint32_t *tab);   tab = TA (2048 長語) の後に TB
# ======================================================================================
o("")
o(".globl\tpoly68k_table_asm")
label("poly68k_table_asm")
# %d0=V3 %d1=V2 %d2=V1 %d3=V0 (movem の順 = 番地の昇順)、%d4-%d7 = R0..R3、%a2 = V4、%a3 = R4、%a4 = 0
o("movem.l\t%d2-%d7/%a2-%a4,-(%sp)")
o("move.l\t40(%sp),%a0")
o("move.l\t44(%sp),%a1")
o("movem.l\t(%a1),%d4-%d7")
o("move.l\t16(%a1),%a3")
o("moveq\t#0,%d0"); o("moveq\t#0,%d1"); o("moveq\t#0,%d2"); o("moveq\t#0,%d3")
o("suba.l\t%a2,%a2"); o("suba.l\t%a4,%a4")
o("lea\t8192(%a0),%a1")                         # 終わり
label(".Lt_loop")
o("move.l\t%a2,4(%a0)")
o("movem.l\t%d0-%d3,8(%a0)")
o("move.l\t%a4,24(%a0)")
o("lea\t32(%a0),%a0")
o("add.l\t%d4,%d3"); o("addx.l\t%d5,%d2"); o("addx.l\t%d6,%d1"); o("addx.l\t%d7,%d0")
o("bcc.s\t1f"); o("addq.l\t#1,%a2")
label("1")
o("adda.l\t%a3,%a2")
o("cmpa.l\t%a1,%a0")
o("bne.s\t.Lt_loop")
o("movem.l\t(%sp)+,%d2-%d7/%a2-%a4")
o("rts")

# --- ブロック関数 ---
# レジスタ: h の長語 h0..h3 = %d4,%d0,%d1,%d2 (積の窓の最後の割り当てと同じにして、ループの頭で移さない)。
# 積の窓 A[q..q+4] は %d0-%d4 を回して使う (A[q] が終わったらスタックへ出し、そのレジスタを A[q+5] に)。
# %d5 = 添字、%d6 = 読み出し、%a0 = TA、%a1 = TB、%a5 = 項のポインタ、%a6 = メッセージ。
TL = 0           # L = A[0..3] (長語、A[q] は TL+4q)
THB = 16         # h の長語 (movem.l %d0-%d2/%d4 の順: h1,h2,h3,h0)
TH8 = 32         # h8 (語)
TQ = 34          # 畳み込みの q (語)
TCNT = 36
THIBIT = 40      # 語
TLOC = 44
TARG = TLOC + SAVED + 4
HREG = ["%d4", "%d0", "%d1", "%d2"]          # h0..h3
HADDR = {0: THB + 12, 1: THB, 2: THB + 4, 3: THB + 8}   # 長語 j の番地
def hbyte(k):    # h のバイト k の番地 (ビッグエンディアンの長語 j の 3-t 番目)
    if k == 16: return TH8 + 1
    return HADDR[k // 4] + 3 - (k % 4)

o("")
o(".globl\tpoly68k_blocks_tab")
label("poly68k_blocks_tab")
o("movem.l\t%d2-%d7/%a2-%a6,-(%sp)")
o("lea\t-%d(%%sp),%%sp" % TLOC)
o("move.l\t%d(%%sp),%%a1" % TARG)              # st
o("move.l\t%d(%%sp),%%a6" % (TARG + 4))
o("move.l\t%d(%%sp),%%d0" % (TARG + 8))
o("beq\t.Lb_ret0")
o("move.l\t%%d0,%d(%%sp)" % TCNT)
o("move.w\t%d(%%sp),%d(%%sp)" % (TARG + 14, THIBIT))
o("move.w\t18(%%a1),%d(%%sp)" % TH8)
for j in range(4):
    o("move.l\t%d(%%a1),%s" % (4 * j, HREG[j]))
o("move.l\t%d(%%sp),%%a0" % (TARG + 16))       # TA
o("lea\t8192(%a0),%a1")                         # TB

label(".Lb_loop")
for j in range(4):                               # h += m (X の連鎖。move/rol/swap は X を変えない)
    o("move.l\t(%a6)+,%d3"); bswap("%d3")
    o(("add.l" if j == 0 else "addx.l") + "\t%%d3,%s" % HREG[j])
o("move.w\t%d(%%sp),%%d5" % TH8)
o("move.w\t%d(%%sp),%%d6" % THIBIT)
o("addx.w\t%d6,%d5")
o("move.w\t%%d5,%d(%%sp)" % TH8)
o("movem.l\t%%d0-%%d2/%%d4,%d(%%sp)" % THB)
for r in ("%d0", "%d1", "%d2", "%d3", "%d4"):
    o("moveq\t#0,%s" % r)
win = {q: "%%d%d" % q for q in range(5)}        # A[q] -> レジスタ
for k in range(17):
    q, t = k // 4, k % 4
    o("moveq\t#0,%d5")
    o("move.b\t%d(%%sp),%%d5" % hbyte(k))
    o("lsl.w\t#5,%d5")
    o("lea\t%d(%s,%%d5.w),%%a5" % (24 if t < 2 else 26, "%a0" if t % 2 == 0 else "%a1"))
    nl = 5 if q < 4 else 4                       # h8 (< 16) * r < 2^128 なので最後は 4 長語
    o("add.l\t-(%%a5),%s" % win[q])
    for l in range(1, nl):
        o("move.l\t-(%a5),%d6"); o("addx.l\t%%d6,%s" % win[q + l])
    if t == 3 and q < 4:                         # A[q] は確定: スタックへ出して A[q+5] に回す
        o("move.l\t%s,%d(%%sp)" % (win[q], TL + 4 * q))
        if q + 5 <= 7:
            win[q + 5] = win[q]
            o("moveq\t#0,%s" % win[q])
        del win[q]
assert [win[q] for q in range(4, 8)] == HREG, win
# 畳み込み: Q = A[4..7] (h0..h3 のレジスタ)、空きは %d3,%d5,%d6,%d7
Q = HREG
o("moveq\t#3,%d3")
o("and.w\t%s,%%d3" % Q[0])
o("move.w\t%%d3,%d(%%sp)" % TQ)                 # q = Q & 3
o("and.w\t#-4,%s" % Q[0])                        # M
H = ["%d3", "%d5", "%d6", "%d7"]
for a, b in zip(Q, H): o("move.l\t%s,%s" % (a, b))
for _ in range(2):
    o("lsr.l\t#1,%s" % H[3]); o("roxr.l\t#1,%s" % H[2]); o("roxr.l\t#1,%s" % H[1]); o("roxr.l\t#1,%s" % H[0])
o("add.l\t%s,%s" % (H[0], Q[0]))                 # T = M + (M >> 2)
for j in range(1, 4): o("addx.l\t%s,%s" % (H[j], Q[j]))
o("moveq\t#0,%d7"); o("addx.l\t%d7,%d7")       # 桁上げ
o("move.l\t%d(%%sp),%%d3" % TL); o("add.l\t%%d3,%s" % Q[0])   # h = L + T
for j in range(1, 4): o("move.l\t%d(%%sp),%%d3" % (TL + 4 * j)); o("addx.l\t%%d3,%s" % Q[j])
o("moveq\t#0,%d3"); o("addx.l\t%d3,%d7")
o("add.w\t%d(%%sp),%%d7" % TQ)
o("move.w\t%%d7,%d(%%sp)" % TH8)
o("subq.l\t#1,%d(%%sp)" % TCNT)
o("bne\t.Lb_loop")

o("move.l\t%d(%%sp),%%a1" % TARG)
for j in range(4):
    o("move.l\t%s,%d(%%a1)" % (HREG[j], 4 * j))
o("moveq\t#0,%d0")
o("move.w\t%d(%%sp),%%d0" % TH8)
o("move.l\t%d0,16(%a1)")
label(".Lb_ret0")
o("lea\t%d(%%sp),%%sp" % TLOC)
o("movem.l\t(%sp)+,%d2-%d7/%a2-%a6")
o("rts")

sys.stdout.write("| poly68k.s -- gen_poly.py で生成 (手で直さない)。swap %d 回/ブロック\n" % nswap)
sys.stdout.write("".join(out))
