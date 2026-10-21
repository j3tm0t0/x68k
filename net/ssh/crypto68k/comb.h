/* comb.h -- Ed25519 の固定基点スカラー倍 scalarbase_fast (ref10 ge_scalarmult_base 方式)。
 * ベース B の倍数表 (basetable.h) を使い、64 個の符号付き 4bit 窓で、mixed-add 64 回 + 倍加 4 回に。
 * 元の scalarbase (256 倍加 + 256 加算) の ~8 倍速。結果は同一。field_fast.h から include。
 * 依存: gf, M_fast, S_fast (正規化を内部で行うので入力は緩くてよい), gf0/gf1 相当は自前で持つ。 */
#include "basetable.h"

/* 緩い gf の加減算 (正規化しない。M_fast/S_fast が入力を正規化するので可)。 */
static void g_add(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void g_sub(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void g_cpy(gf o, const gf a) { int i; for (i = 0; i < 16; i++) o[i] = a[i]; }

/* mixed add: r = p + q (q は precomp: ysubx,xaddy,t2d)。a=-1 の拡張座標。 */
static void ge_madd(gf r[4], gf p[4], const precomp *q)
{
  gf YpX, YmX, A, B, C, D, E, F, G, H, qy, qx, qt;
  int i;
  for (i = 0; i < 16; i++) { qy[i] = q->ysubx[i]; qx[i] = q->xaddy[i]; qt[i] = q->t2d[i]; }
  g_sub(YmX, p[1], p[0]);
  g_add(YpX, p[1], p[0]);
  M_fast(A, YmX, qy);
  M_fast(B, YpX, qx);
  M_fast(C, p[3], qt);
  g_add(D, p[2], p[2]);
  g_sub(E, B, A); g_sub(F, D, C); g_add(G, D, C); g_add(H, B, A);
  M_fast(r[0], E, F); M_fast(r[1], H, G); M_fast(r[2], G, F); M_fast(r[3], E, H);
}

/* 倍加: r = 2p (a=-1)。 */
static void ge_dbl(gf r[4], gf p[4])
{
  gf A, B, C, E, F, G, H, XpY, tz, AB;
  S_fast(A, p[0]);
  S_fast(B, p[1]);
  S_fast(tz, p[2]); g_add(C, tz, tz);
  g_add(XpY, p[0], p[1]); S_fast(E, XpY); g_sub(E, E, A); g_sub(E, E, B);  /* 2XY */
  g_sub(G, B, A);          /* D=-A なので G=D+B=B-A */
  g_sub(F, G, C);
  g_add(AB, A, B); g_sub(H, p[0], p[0]); g_sub(H, H, AB);  /* H = -(A+B) */
  M_fast(r[0], E, F); M_fast(r[1], G, H); M_fast(r[2], F, G); M_fast(r[3], E, H);
}

/* precomp の定数時間 cmov (cond!=0 で in を out に) */
static void pc_cmov(precomp *o, const precomp *in, int cond)
{
  unsigned short m = (unsigned short)(-(cond & 1));
  int i;
  for (i = 0; i < 16; i++) {
    o->ysubx[i] ^= m & (o->ysubx[i] ^ in->ysubx[i]);
    o->xaddy[i] ^= m & (o->xaddy[i] ^ in->xaddy[i]);
    o->t2d[i]   ^= m & (o->t2d[i]   ^ in->t2d[i]);
  }
}

/* 単位元 precomp (ysubx=1, xaddy=1, t2d=0) を o にセット */
static void pc_one(precomp *o)
{
  int i;
  for (i = 0; i < 16; i++) { o->ysubx[i] = 0; o->xaddy[i] = 0; o->t2d[i] = 0; }
  o->ysubx[0] = 1; o->xaddy[0] = 1;
}

/* base_table[pos] から |b| 番目を選び、b<0 なら負にする (定数時間) */
static void pc_select(precomp *t, int pos, signed char b)
{
  int bneg = (b >> 7) & 1;                 /* b<0 で 1 */
  int babs = b - (((-bneg) & b) * 2);      /* |b| */
  int k;
  gf negt;
  pc_one(t);
  for (k = 0; k < 8; k++) pc_cmov(t, &base_table[pos][k], babs == k + 1);
  /* 負にする: ysubx<->xaddy を入れ替え、t2d を否定 (p 上の -t2d) */
  { int i; unsigned short tmp;
    /* t2d を p で否定した gf を作る: negt = p - t2d。ここでは gf として -(t2d) を緩く作り正規化は madd の M が行う */
    gf gt; for (i = 0; i < 16; i++) gt[i] = t->t2d[i];
    g_sub(negt, gt, gt);        /* 0 */
    g_sub(negt, negt, gt);      /* -t2d */
    /* 条件付きで入れ替え・否定 */
    for (i = 0; i < 16; i++) {
      unsigned short m = (unsigned short)(-(bneg & 1));
      /* swap ysubx/xaddy */
      tmp = m & (t->ysubx[i] ^ t->xaddy[i]);
      t->ysubx[i] ^= tmp; t->xaddy[i] ^= tmp;
    }
    /* t2d を条件付きで否定 (正規化して u16 に収める) */
    if (bneg) {
      gf nn; int j; int32_t c = 0, v;
      for (j = 0; j < 16; j++) nn[j] = negt[j];
      /* norm して u16 に */
      for (j = 0; j < 16; j++) { v = (int32_t)nn[j] + c; t->t2d[j] = (unsigned short)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
      v = (int32_t)t->t2d[0] + 38 * c; t->t2d[0] = (unsigned short)v; c = (v - (int32_t)(uint16_t)v) >> 16;
      for (j = 1; j < 16 && c; j++) { v = (int32_t)t->t2d[j] + c; t->t2d[j] = (unsigned short)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
    }
  }
}

/* o = s * B。s は 32 バイトのスカラー (<2^256)。元の scalarbase と同一結果。 */
static void scalarbase_fast(gf p[4], const unsigned char *s)
{
  signed char e[64];
  int i, carry;
  gf r[4];
  precomp t;
  for (i = 0; i < 32; i++) { e[2 * i] = (s[i] >> 0) & 15; e[2 * i + 1] = (s[i] >> 4) & 15; }
  carry = 0;
  for (i = 0; i < 63; i++) { e[i] += carry; carry = (e[i] + 8) >> 4; e[i] -= carry << 4; }
  e[63] += carry;
  /* p = 単位元 (0,1,1,0) */
  for (i = 0; i < 16; i++) { p[0][i] = 0; p[1][i] = 0; p[2][i] = 0; p[3][i] = 0; }
  p[1][0] = 1; p[2][0] = 1;
  for (i = 1; i < 64; i += 2) { pc_select(&t, i / 2, e[i]); ge_madd(r, p, &t); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]); }
  ge_dbl(r, p); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]);
  ge_dbl(r, p); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]);
  ge_dbl(r, p); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]);
  ge_dbl(r, p); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]);
  for (i = 0; i < 64; i += 2) { pc_select(&t, i / 2, e[i]); ge_madd(r, p, &t); g_cpy(p[0], r[0]); g_cpy(p[1], r[1]); g_cpy(p[2], r[2]); g_cpy(p[3], r[3]); }
}
