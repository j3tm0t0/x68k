/* cp68k.c -- 68000 向けの ChaCha20 / Poly1305 (cp68k.h)。C 版と asm 版 (CP68K_ASM) の入口。
 *
 * Poly1305: radix 2^16。h (< 2^132) を 16 ビットのリム h0..h8 (h8 < 16)、r を r0..r7 にして、積 h*r を
 * MULU.W (16x16->32) の comba で列ごとに 64 ビットで足し、列の下位 16 ビットを P[k] として出す (P < 2^256)。
 * 畳み込みは 2^130 = 5 (mod p) で:
 *     P = L + 2^128 * Q  (L = P mod 2^128, Q = P >> 128)
 *       = L + 2^128 * (Q & 3) + 2^130 * (Q >> 2)
 *       = L + 2^128 * (Q & 3) + 5 * (Q >> 2)          (mod p)
 * M = Q & ~3 とおくと 5 * (Q >> 2) = M + (M >> 2)。よって h' = L + 2^128 * (Q & 3) + M + (M >> 2)。
 * h < 2^132 なら P < 2^132 * 2^124 = 2^256、M < 2^128 で h' < 2^130 + 2^129、次のブロックを足しても < 2^132。
 * libtomcrypt (radix 2^26、64 ビット積 = __muldi3) と同じ値 (mod p) なので、最後の整約後のタグは一致する。 */
#include <string.h>
#include "cp68k.h"
/* CP68K_ASM で両方、CP68K_POLY_ASM / CP68K_CHACHA_ASM で片方だけ asm (試験用) */
#ifdef CP68K_ASM
#define CP68K_POLY_ASM
#define CP68K_CHACHA_ASM
#endif

static uint32_t le32(const unsigned char *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

void poly68k_init(poly68k_state *st, const unsigned char key[32])
{
  static const uint32_t clamp[4] = { 0x0fffffff, 0x0ffffffc, 0x0ffffffc, 0x0ffffffc };
  int i;
  for (i = 0; i < 4; i++) {
    uint32_t w = le32(key + 4 * i) & clamp[i];
    st->r[2 * i] = (uint16_t)w;
    st->r[2 * i + 1] = (uint16_t)(w >> 16);
    st->h[i] = 0;
    st->pad[i] = le32(key + 16 + 4 * i);
  }
  st->h4 = 0;
}

#ifdef CP68K_POLY_ASM
extern void poly68k_blocks_asm(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit);
#endif

void poly68k_blocks(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit)
{
#ifdef CP68K_POLY_ASM
  poly68k_blocks_asm(st, m, n, hibit);
#else
  uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h4;
  const uint16_t *r = st->r;
  for (; n; n--, m += 16) {
    uint16_t hw[9];
    uint16_t P[16];
    uint32_t lo = 0, hi = 0, c, t, M0, M1, M2, M3, q;
    int k, i;
    /* h += m + hibit * 2^128 (32 ビットの桁上げ) */
    t = h0 + le32(m);      c = t < h0; h0 = t;
    t = h1 + c;            c = t < c;  t += le32(m + 4);  c += t < le32(m + 4);  h1 = t;
    t = h2 + c;            c = t < c;  t += le32(m + 8);  c += t < le32(m + 8);  h2 = t;
    t = h3 + c;            c = t < c;  t += le32(m + 12); c += t < le32(m + 12); h3 = t;
    h4 += c + hibit;
    hw[0] = (uint16_t)h0; hw[1] = (uint16_t)(h0 >> 16); hw[2] = (uint16_t)h1; hw[3] = (uint16_t)(h1 >> 16);
    hw[4] = (uint16_t)h2; hw[5] = (uint16_t)(h2 >> 16); hw[6] = (uint16_t)h3; hw[7] = (uint16_t)(h3 >> 16);
    hw[8] = (uint16_t)h4;
    /* comba: 列 k = sum h_i * r_(k-i)。(hi:lo) は 64 ビットのアキュムレータ (列の和 < 9 * 2^32) */
    for (k = 0; k < 16; k++) {
      int i0 = k > 7 ? k - 7 : 0, i1 = k < 8 ? k : 8;
      for (i = i0; i <= i1; i++) {
        uint32_t p = (uint32_t)hw[i] * r[k - i];
        lo += p; hi += lo < p;
      }
      P[k] = (uint16_t)lo;
      lo = (lo >> 16) | (hi << 16); hi >>= 16;
    }
    /* 畳み込み: h' = L + 2^128 * (P8 & 3) + M + (M >> 2)、M = (P >> 128) & ~3 */
    q = P[8] & 3;
    M0 = ((uint32_t)P[9] << 16) | (P[8] & 0xfffc); M1 = ((uint32_t)P[11] << 16) | P[10];
    M2 = ((uint32_t)P[13] << 16) | P[12];          M3 = ((uint32_t)P[15] << 16) | P[14];
    {
      uint32_t H0 = (M0 >> 2) | (M1 << 30), H1 = (M1 >> 2) | (M2 << 30), H2 = (M2 >> 2) | (M3 << 30), H3 = M3 >> 2;
      uint32_t L0 = ((uint32_t)P[1] << 16) | P[0], L1 = ((uint32_t)P[3] << 16) | P[2];
      uint32_t L2 = ((uint32_t)P[5] << 16) | P[4], L3 = ((uint32_t)P[7] << 16) | P[6];
      uint32_t s;
      /* T = M + H */
      s = M0 + H0; c = s < M0; M0 = s;
      s = M1 + c; c = s < c; s += H1; c += s < H1; M1 = s;
      s = M2 + c; c = s < c; s += H2; c += s < H2; M2 = s;
      s = M3 + c; c = s < c; s += H3; c += s < H3; M3 = s;
      q += c;
      /* h = L + T */
      h0 = L0 + M0; c = h0 < L0;
      s = L1 + c; c = s < c; s += M1; c += s < M1; h1 = s;
      s = L2 + c; c = s < c; s += M2; c += s < M2; h2 = s;
      s = L3 + c; c = s < c; s += M3; c += s < M3; h3 = s;
      h4 = q + c;
    }
  }
  st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h4 = h4;
#endif
}

void poly68k_finish(poly68k_state *st, unsigned char tag[16])
{
  uint32_t h[5], g[5], c, mask, t;
  int i;
  for (i = 0; i < 4; i++) h[i] = st->h[i];
  h[4] = st->h4;
  /* 2^130 以上を 5 倍して下に戻す (h4 < 16 なので 1 回で h < 2^130 + 小) を 2 回 */
  for (i = 0; i < 2; i++) {
    int j;
    c = (h[4] >> 2) * 5; h[4] &= 3;
    for (j = 0; j < 4; j++) { t = h[j] + c; c = t < c; h[j] = t; }
    h[4] += c;
  }
  /* g = h + 5 - 2^130。g >= 0 (h >= p) なら g を使う */
  c = 5;
  for (i = 0; i < 4; i++) { t = h[i] + c; c = t < c; g[i] = t; }
  g[4] = h[4] + c - 4;
  mask = (g[4] >> 31) - 1;   /* g4 が負 (h < p) なら 0 */
  for (i = 0; i < 4; i++) h[i] = (h[i] & ~mask) | (g[i] & mask);
  /* tag = (h + pad) mod 2^128 */
  c = 0;
  for (i = 0; i < 4; i++) {
    t = h[i] + c; c = t < c;
    t += st->pad[i]; c += t < st->pad[i];
    st32(tag + 4 * i, t);
  }
  memset(st, 0, sizeof *st);
}

#ifdef CP68K_POLY_ASM
/* 表引き版 (gen_poly.py の poly68k_blocks_tab)。表は 64KB の領域 1 つ (b ごとに 256 バイトの枠、TA と TB)。
   表を作るのに ~8.6 万クロックかかるので、26 ブロック (416 バイト) 未満は MULU 版 (1 ブロック ~6600 クロック、表引きは ~3000) */
#define POLY68K_TAB_MIN 26
extern void poly68k_table_asm(char *slot0, const uint32_t R[5]);
extern void poly68k_blocks_tab(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit, const char *base);
static uint32_t poly68k_tab[65536 / 4];
#define POLY68K_BASE ((char *)poly68k_tab + 32768)
static void poly68k_tables(const poly68k_state *st)
{
  uint32_t R[5], R8[5];
  int i;
  for (i = 0; i < 4; i++) R[i] = st->r[2 * i] | ((uint32_t)st->r[2 * i + 1] << 16);
  R[4] = 0;
  R8[0] = R[0] << 8;
  for (i = 1; i < 5; i++) R8[i] = (R[i] << 8) | (R[i - 1] >> 24);
  poly68k_table_asm(POLY68K_BASE, R);
  poly68k_table_asm(POLY68K_BASE + 32, R8);
}
#endif

void poly68k_mac(unsigned char tag[16], const unsigned char *m, unsigned long len, const unsigned char key[32])
{
  poly68k_state st;
  unsigned char buf[16] __attribute__((aligned(2)));
  unsigned long n = len >> 4;
  poly68k_init(&st, key);
#ifdef CP68K_POLY_ASM
  if (n >= POLY68K_TAB_MIN && ((uintptr_t)m & 1) == 0) {
    poly68k_tables(&st);
    poly68k_blocks_tab(&st, m, n, 1, POLY68K_BASE);
  } else
#endif
  if (n) {
    if (((uintptr_t)m & 1) == 0) poly68k_blocks(&st, m, n, 1);
    else {   /* 奇数番地は 68000 で語アクセスできないので 1 ブロックずつ写す */
      unsigned long i;
      for (i = 0; i < n; i++) { memcpy(buf, m + 16 * i, 16); poly68k_blocks(&st, buf, 1, 1); }
    }
  }
  len &= 15;
  if (len) {
    memcpy(buf, m + 16 * n, len);
    buf[len] = 1;
    memset(buf + len + 1, 0, 15 - len);
    poly68k_blocks(&st, buf, 1, 0);
  }
  poly68k_finish(&st, tag);
}

/* ChaCha20: C 版 (ホストと CP68K_ASM 無しの 68000 用)。asm 版は chacha68k.s の chacha68k_xor_asm。 */
#ifdef CP68K_CHACHA_ASM
extern void chacha68k_xor_asm(uint32_t x[16], const unsigned char *in, unsigned char *out, unsigned long nblocks);
#endif

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
  a += b; d = ROTL(d ^ a, 16); c += d; b = ROTL(b ^ c, 12); \
  a += b; d = ROTL(d ^ a, 8);  c += d; b = ROTL(b ^ c, 7);
static void chacha_block(const uint32_t in[16], unsigned char ks[64])
{
  uint32_t x[16];
  int i;
  memcpy(x, in, sizeof x);
  for (i = 0; i < 10; i++) {
    QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
    QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
  }
  for (i = 0; i < 16; i++) st32(ks + 4 * i, x[i] + in[i]);
}

void chacha68k_xor(uint32_t x[16], const unsigned char *in, unsigned char *out, unsigned long len)
{
  unsigned char ks[64];
  unsigned long i, n;
#ifdef CP68K_CHACHA_ASM
  /* 64 バイト単位は asm (偶数番地のときだけ。奇数は下の C で) */
  if (len >= 64 && (((uintptr_t)in | (uintptr_t)out) & 1) == 0) {
    n = len >> 6;
    chacha68k_xor_asm(x, in, out, n);
    in += n << 6; out += n << 6; len &= 63;
  }
#endif
  while (len) {
    chacha_block(x, ks);
    if (++x[12] == 0) ++x[13];
    n = len < 64 ? len : 64;
    for (i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
    in += n; out += n; len -= n;
  }
}
