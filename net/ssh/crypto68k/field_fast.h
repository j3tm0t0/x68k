/* field_fast.h -- 2^255-19 の体演算を 68000 向けに書いた高速版 M/S。
 * TweetNaCl の M (i64 積 __muldi3 を 256 回) と car25519 (i64 シフト/乗算) を避ける。
 * 結果は元 M/S と mod p で一致 (リムは [0,2^16) の緩い形で、pack25519 が最終整約)。
 *
 *  - 入力正規化は 32bit の符号付きキャリーで [0,2^16) へ (car25519 の i64 を使わない)。
 *  - 積は MULU (16x16->32)、列は u64 で蓄積。S は対称性で積を半分。
 *  - 2^256 ≡ 38。畳み込み・最終整約は u64 のキャリー 1 巡 + 先頭への 38 倍戻し (正の値のみ)。
 *
 * car25519 の定義より後ろで include すること。
 */
#include <stdint.h>

/* 符号付き入力 (|limb| が 2^20 程度まで) を [0,2^16) に正規化。値は mod p 不変。 */
static void norm_in(gf out, const gf in)
{
  int32_t c = 0, v;
  int i;
  for (i = 0; i < 16; i++) { v = (int32_t)in[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
  /* c = リム15 から出た符号付きキャリー (bits>=256)。2^256≡38 で先頭に戻す。 */
  v = (int32_t)out[0] + 38 * c; out[0] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16;
  for (i = 1; i < 16 && c; i++) { v = (int32_t)out[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
  if (c) { v = (int32_t)out[0] + 38 * c; out[0] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16;
           for (i = 1; i < 16 && c; i++) { v = (int32_t)out[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; } }
}

/* 列和 t[0..30] (正, t[i]<2^42) を [0,2^16) の gf に整約。car25519 2 回と同等 (正の値限定)。 */
static void reduce_cols(gf o, u64 *t)
{
  u64 c = 0, v;
  uint16_t w[16];
  int i;
  for (i = 0; i < 15; i++) t[i] += ((u64)t[i + 16] << 5) + ((u64)t[i + 16] << 2) + ((u64)t[i + 16] << 1); /* *38 */
  for (i = 0; i < 16; i++) { v = t[i] + c; w[i] = (uint16_t)v; c = v >> 16; }
  /* c = リム15 の繰り上がり (bits>=256) -> *38 で先頭へ */
  v = (u64)w[0] + ((c << 5) + (c << 2) + (c << 1)); w[0] = (uint16_t)v; c = v >> 16;
  for (i = 1; i < 16 && c; i++) { v = (u64)w[i] + c; w[i] = (uint16_t)v; c = v >> 16; }
  if (c) { v = (u64)w[0] + ((c << 5) + (c << 2) + (c << 1)); w[0] = (uint16_t)v; c = v >> 16;
           for (i = 1; i < 16 && c; i++) { v = (u64)w[i] + c; w[i] = (uint16_t)v; c = v >> 16; } }
  for (i = 0; i < 16; i++) o[i] = w[i];
}

#ifdef USE_ASM
/* 68000 の手書き comba 乗算/二乗 (mul68k.s)。入力は [0,2^16) の u16[16]、出力は 32 リムの積。 */
#ifdef HOST_ASM_STUB
/* ホストで norm_in_u16/reduce_fold を差分テストするための C 版 fe_mul16/fe_sqr16 (asm と同じ契約)。 */
static void fe_mul16(uint16_t r[32], const uint16_t a[16], const uint16_t b[16])
{
  u64 acc = 0; int k, i;
  for (k = 0; k < 31; k++) {
    for (i = (k > 15 ? k - 15 : 0); i <= (k < 15 ? k : 15); i++)
      acc += (u64)((u32)a[i] * (u32)b[k - i]);
    r[k] = (uint16_t)acc; acc >>= 16;
  }
  r[31] = (uint16_t)acc;
}
static void fe_sqr16(uint16_t r[32], const uint16_t a[16]) { fe_mul16(r, a, a); }
#else
extern void fe_mul16(uint16_t r[32], const uint16_t a[16], const uint16_t b[16]);
extern void fe_sqr16(uint16_t r[32], const uint16_t a[16]);
#endif

/* 符号付き gf を [0,2^16) の u16[16] に正規化 (int32、値は mod p 不変)。 */
static void norm_in_u16(uint16_t out[16], const gf in)
{
  int32_t c = 0, v;
  int i;
  for (i = 0; i < 16; i++) { v = (int32_t)in[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
  v = (int32_t)out[0] + 38 * c; out[0] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16;
  for (i = 1; i < 16 && c; i++) { v = (int32_t)out[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; }
  if (c) { v = (int32_t)out[0] + 38 * c; out[0] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16;
           for (i = 1; i < 16 && c; i++) { v = (int32_t)out[i] + c; out[i] = (uint16_t)v; c = (v - (int32_t)(uint16_t)v) >> 16; } }
}

/* 512bit 積 r[32] を 2^256≡38 で畳み込み [0,2^16) の gf に整約。o[i]=r[i]+38*r[16+i] の繰り上げ。 */
static void reduce_fold(gf o, const uint16_t r[32])
{
  uint32_t t[16], c = 0, v;
  int i;
  for (i = 0; i < 16; i++) t[i] = (uint32_t)r[i] + 38u * r[16 + i];   /* < 2^22 */
  for (i = 0; i < 16; i++) { v = t[i] + c; o[i] = (uint16_t)v; c = v >> 16; }
  v = (uint32_t)(uint16_t)o[0] + 38u * c; o[0] = (uint16_t)v; c = v >> 16;
  for (i = 1; i < 16 && c; i++) { v = (uint32_t)(uint16_t)o[i] + c; o[i] = (uint16_t)v; c = v >> 16; }
  if (c) { v = (uint32_t)(uint16_t)o[0] + 38u * c; o[0] = (uint16_t)v; c = v >> 16;   /* 残余キャリーの再畳み込み (reduce_cols と同じ) */
           for (i = 1; i < 16 && c; i++) { v = (uint32_t)(uint16_t)o[i] + c; o[i] = (uint16_t)v; c = v >> 16; } }
}

static void M_fast(gf o, const gf a, const gf b)
{
  uint16_t na[16], nb[16], r[32];
  norm_in_u16(na, a);
  norm_in_u16(nb, b);
  fe_mul16(r, na, nb);
  reduce_fold(o, r);
}

static void S_fast(gf o, const gf a)
{
  uint16_t na[16], r[32];
  norm_in_u16(na, a);
  fe_sqr16(r, na);
  reduce_fold(o, r);
}

#else   /* C 版 (ホストの差分テスト・asm なしビルド用) */

static void M_fast(gf o, const gf a, const gf b)
{
  gf na, nb;
  u64 t[31];
  int i, j;
  norm_in(na, a);
  norm_in(nb, b);
  for (i = 0; i < 31; i++) t[i] = 0;
  for (i = 0; i < 16; i++) {
    unsigned ai = (unsigned)(unsigned short)na[i];
    u64 *ti = t + i;
    for (j = 0; j < 16; j++)
      ti[j] += (u32)(ai * (unsigned)(unsigned short)nb[j]);
  }
  reduce_cols(o, t);
}

static void S_fast(gf o, const gf a)
{
  gf na;
  u64 t[31];
  int i, j;
  norm_in(na, a);
  for (i = 0; i < 31; i++) t[i] = 0;
  for (i = 0; i < 16; i++) {
    unsigned ai = (unsigned)(unsigned short)na[i];
    u64 *ti = t + i;
    ti[i] += (u32)(ai * ai);
    for (j = i + 1; j < 16; j++)
      ti[j] += (u64)((u32)(ai * (unsigned)(unsigned short)na[j])) << 1;
  }
  reduce_cols(o, t);
}
#endif

/* ===== 逆元・pow2523 を加算鎖で (TweetNaCl の素朴な 254S+252M を 254S+11M に) ===== */
/* いずれも M_fast/S_fast を直接使う (field_fast.h 内で定義済み)。結果は元と同一。 */
static void sqn(gf o, const gf a, int n)   /* o = a^(2^n) */
{
  int i;
  S_fast(o, a);
  for (i = 1; i < n; i++) S_fast(o, o);
}

static void inv_fast(gf o, const gf z)     /* o = z^(2^255-21) = z^-1 */
{
  gf t0, t1, t2, t3;
  S_fast(t0, z);                      /* 2 */
  S_fast(t1, t0); S_fast(t1, t1);     /* 8 */
  M_fast(t1, z, t1);                  /* 9 */
  M_fast(t0, t0, t1);                 /* 11 */
  S_fast(t2, t0);                     /* 22 */
  M_fast(t1, t1, t2);                 /* 2^5-1 */
  sqn(t2, t1, 5);   M_fast(t1, t2, t1);   /* 2^10-1 */
  sqn(t2, t1, 10);  M_fast(t2, t2, t1);   /* 2^20-1 */
  sqn(t3, t2, 20);  M_fast(t2, t3, t2);   /* 2^40-1 */
  sqn(t2, t2, 10);  M_fast(t1, t2, t1);   /* 2^50-1 */
  sqn(t2, t1, 50);  M_fast(t2, t2, t1);   /* 2^100-1 */
  sqn(t3, t2, 100); M_fast(t2, t3, t2);   /* 2^200-1 */
  sqn(t2, t2, 50);  M_fast(t1, t2, t1);   /* 2^250-1 */
  sqn(t1, t1, 5);   M_fast(o, t1, t0);    /* 2^255-21 */
}

static void pow2523_fast(gf o, const gf z) /* o = z^(2^252-3) */
{
  gf t0, t1, t2;
  S_fast(t0, z);                      /* 2 */
  S_fast(t1, t0); S_fast(t1, t1);     /* 8 */
  M_fast(t1, z, t1);                  /* 9 */
  M_fast(t0, t0, t1);                 /* 11 */
  S_fast(t0, t0);                     /* 22 */
  M_fast(t0, t1, t0);                 /* 2^5-1 */
  sqn(t1, t0, 5);   M_fast(t0, t1, t0);   /* 2^10-1 */
  sqn(t1, t0, 10);  M_fast(t1, t1, t0);   /* 2^20-1 */
  sqn(t2, t1, 20);  M_fast(t1, t2, t1);   /* 2^40-1 */
  sqn(t1, t1, 10);  M_fast(t0, t1, t0);   /* 2^50-1 */
  sqn(t1, t0, 50);  M_fast(t1, t1, t0);   /* 2^100-1 */
  sqn(t2, t1, 100); M_fast(t1, t2, t1);   /* 2^200-1 */
  sqn(t1, t1, 50);  M_fast(t0, t1, t0);   /* 2^250-1 */
  sqn(t0, t0, 2);   M_fast(o, t0, z);     /* 2^252-3 */
}

/* 固定基点 comb scalarbase (M_fast/S_fast の後で) */
#include "comb.h"
