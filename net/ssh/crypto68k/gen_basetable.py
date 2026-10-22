#!/usr/bin/env python3
"""Ed25519 の固定基点 comb 用の事前計算表を生成する (ref10 ge_scalarmult_base と同じ構造)。
base[pos][k] = [(k+1) * 16^(2*pos)] * B  (pos=0..31, k=0..7) を
precomp 形式 (ysubx=y-x, xaddy=y+x, t2d=2d*x*y) の gf (u16[16]) で出す。
出力: gen_basetable.py > basetable.h
"""
import sys

p = 2**255 - 19
d = (-121665 * pow(121666, p - 2, p)) % p
# 標準 Ed25519 ベース点 (TweetNaCl の X,Y 定数と同じ)
By = (4 * pow(5, p - 2, p)) % p
Bx = 15112221349535400772501151409588531511454012693041857206046113283949847762202


def inv(a): return pow(a, p - 2, p)


# 拡張座標 (X:Y:Z:T), x=X/Z, y=Y/Z, T=XY/Z
def to_ext(x, y): return (x % p, y % p, 1, (x * y) % p)


def ext_add(P, Q):
    X1, Y1, Z1, T1 = P
    X2, Y2, Z2, T2 = Q
    A = ((Y1 - X1) * (Y2 - X2)) % p
    B = ((Y1 + X1) * (Y2 + X2)) % p
    C = (T1 * 2 * d * T2) % p
    D = (2 * Z1 * Z2) % p
    E = (B - A) % p; F = (D - C) % p; G = (D + C) % p; H = (B + A) % p
    return ((E * F) % p, (G * H) % p, (F * G) % p, (E * H) % p)


def ext_dbl(P): return ext_add(P, P)


def scalar_mul(k, P):
    R = (0, 1, 1, 0)   # 単位元
    Q = P
    while k:
        if k & 1: R = ext_add(R, Q)
        Q = ext_dbl(Q)
        k >>= 1
    return R


def affine(P):
    X, Y, Z, T = P
    zi = inv(Z)
    return (X * zi) % p, (Y * zi) % p


B = to_ext(Bx, By)
# sanity: pack(B) の y が By、x の符号
ax, ay = affine(B)
assert ax == Bx and ay == By

# gf: radix 2^16, 16 リム (little-endian)
def to_gf(v):
    v %= p
    return [(v >> (16 * i)) & 0xffff for i in range(16)]


out = sys.stdout
out.write("/* 自動生成 (gen_basetable.py)。Ed25519 固定基点 comb 表 (ref10 構造)。\n")
out.write(" * base[pos][k] = [(k+1)*16^(2*pos)]*B を (ysubx,xaddy,t2d) の gf(u16[16]) で。 */\n")
out.write("#ifndef BASETABLE_H\n#define BASETABLE_H\n")
out.write("typedef struct { unsigned short ysubx[16], xaddy[16], t2d[16]; } precomp;\n")
out.write("static const precomp base_table[32][8] = {\n")
for pos in range(32):
    out.write("{\n")
    mul = pow(16, 2 * pos, p)
    for k in range(8):
        P = scalar_mul((k + 1) * mul % (2**256), B)
        x, y = affine(P)
        ysubx = to_gf((y - x) % p)
        xaddy = to_gf((y + x) % p)
        t2d = to_gf((2 * d * x * y) % p)
        def arr(a): return "{" + ",".join(str(z) for z in a) + "}"
        out.write("  {%s,%s,%s},\n" % (arr(ysubx), arr(xaddy), arr(t2d)))
    out.write("},\n")
out.write("};\n#endif\n")
sys.stderr.write("basetable: 32x8 precomp points\n")
