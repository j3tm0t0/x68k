/* cpbench.c -- X68000 (XEiJ / Z) で chacha20-poly1305@openssh.com を libtomcrypt と cp68k で回し、
 * 結果が同じか確かめて速さを比べる。dropbear の chachapoly.c の encrypt と同じ手順 (net/ssh/crypttest と同じ)。
 * usage: cpbench [-n PACKETS] [-l LEN] [OUTFILE]   既定 16384 バイト x 8 パケットずつ。時間は _iocs_ontime (1/100 秒) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x68k/iocs.h>
#include "tomcrypt.h"
#include "cp68k.h"

static unsigned long fnv(const unsigned char *p, unsigned long n, unsigned long h)
{
  while (n--) { h ^= *p++; h *= 16777619UL; h &= 0xffffffffUL; }
  return h;
}
static unsigned char in[40000], out1[40016], out2[40016];
static chacha_state ch, hd;

static void seq8(unsigned char *b, unsigned long seq) { memset(b, 0, 8); b[4] = seq >> 24; b[5] = seq >> 16; b[6] = seq >> 8; b[7] = seq; }

static void seal_ltc(unsigned long seq, unsigned long len, unsigned char *out)
{
  unsigned char sb[8], pk[64];
  poly1305_state poly;
  unsigned long taglen = 16;
  seq8(sb, seq);
  chacha_ivctr64(&ch, sb, 8, 0); chacha_keystream(&ch, pk, 64);
  poly1305_init(&poly, pk, 32);
  chacha_ivctr64(&hd, sb, 8, 0); chacha_crypt(&hd, in, 4, out);
  chacha_ivctr64(&ch, sb, 8, 1); chacha_crypt(&ch, in + 4, len - 4, out + 4);
  poly1305_process(&poly, out, len); poly1305_done(&poly, out + len, &taglen);
}
static void seal_68k(unsigned long seq, unsigned long len, unsigned char *out)
{
  static const unsigned char zero[32];
  unsigned char sb[8], pk[32];
  seq8(sb, seq);
  chacha_ivctr64(&ch, sb, 8, 0); chacha68k_xor((uint32_t *)ch.input, zero, pk, 32);
  chacha_ivctr64(&hd, sb, 8, 0); chacha68k_xor((uint32_t *)hd.input, in, out, 4);
  chacha_ivctr64(&ch, sb, 8, 1); chacha68k_xor((uint32_t *)ch.input, in + 4, out + 4, len - 4);
  poly68k_mac(out + len, out, len, pk);
}
static long now(void) { struct iocs_time t = _iocs_ontime(); return t.sec; }
static long cs_since(long t0) { long d = now() - t0; if (d < 0) d += 24L * 3600 * 100; return d; }

int main(int argc, char **argv)
{
  static const unsigned long lens[] = { 16, 52, 64, 100, 1000, 4096, 4100, 16384, 32768, 32780, 0 };
  unsigned char key[64];
  unsigned long i, len = 16384, npk = 8;
  int k, bad = 0, a;
  const char *of = "CPBENCH.TXT";
  FILE *f;
  long t;
  for (a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "-n") && a + 1 < argc) npk = strtoul(argv[++a], 0, 10);
    else if (!strcmp(argv[a], "-l") && a + 1 < argc) len = strtoul(argv[++a], 0, 10);
    else of = argv[a];
  }
  f = fopen(of, "w");
  for (i = 0; i < 64; i++) key[i] = (unsigned char)(i * 7 + 3);
  for (i = 0; i < sizeof in; i++) in[i] = (unsigned char)((i * 2654435761UL) >> 24);
  chacha_setup(&ch, key, 32, 20);
  chacha_setup(&hd, key + 32, 32, 20);
  /* 正しさ: 各長さで libtomcrypt と cp68k が同じ暗号文 + タグか (FNV は crypttest と同じ形式) */
  for (k = 0; lens[k]; k++) {
    unsigned long seq;
    for (seq = 3; seq < 5; seq++) {
      seal_ltc(seq, lens[k], out1); seal_68k(seq, lens[k], out2);
      if (memcmp(out1, out2, lens[k] + 16)) bad++;
      fprintf(f, "len %5lu seq %lu ct %08lx tag %08lx %s\n", lens[k], seq, fnv(out1, lens[k], 2166136261UL),
              fnv(out1 + lens[k], 16, 2166136261UL), memcmp(out1, out2, lens[k] + 16) ? "DIFF" : "same");
    }
  }
  fprintf(f, "check: %s\n", bad ? "NG" : "OK");
  /* 速さ: len バイトのパケットを npk 個 */
  t = now();
  for (i = 0; i < npk; i++) seal_ltc(i, len, out1);
  t = cs_since(t);
  fprintf(f, "libtomcrypt: %lu x %lu bytes in %ld.%02ld s = %ld B/s\n", npk, len, t / 100, t % 100, t ? (long)(npk * len * 100 / t) : 0);
  t = now();
  for (i = 0; i < npk; i++) seal_68k(i, len, out2);
  t = cs_since(t);
  fprintf(f, "cp68k:       %lu x %lu bytes in %ld.%02ld s = %ld B/s\n", npk, len, t / 100, t % 100, t ? (long)(npk * len * 100 / t) : 0);
  fprintf(f, "last packet: %s\n", memcmp(out1, out2, len + 16) ? "DIFF" : "same");
  fclose(f);
  return bad;
}
