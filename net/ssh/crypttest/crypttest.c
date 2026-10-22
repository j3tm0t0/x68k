/*
 * crypttest.c -- dropbear の chacha20-poly1305 (libtomcrypt) を大きなパケットで回し、結果の要約を出す。
 * Z (68000) と Mac で同じ値になるかを比べる (ssh.x -Z のヘルパ転送で sshd が "message authentication code incorrect"
 * を出した。Z はそれまで小さなパケットしか暗号化していなかった)。dropbear の chachapoly.c の encrypt と同じ手順:
 *   header 鍵で長さ 4 バイト、本体鍵 (ctr=1) で残り、本体鍵 ctr=0 の 32 バイトで poly1305 の鍵、MAC は暗号文全体。
 * usage: crypttest [OUTFILE]   各長さの FNV-1a (暗号文 + タグ) を 1 行ずつ
 */
#include <stdio.h>
#include <string.h>
#include "tomcrypt.h"

static unsigned long fnv(const unsigned char *p, unsigned long n, unsigned long h)
{
  while (n--) { h ^= *p++; h *= 16777619UL; h &= 0xffffffffUL; }
  return h;
}
static unsigned char in[40000], out[40000 + 16];

int main(int argc, char **argv)
{
  static const unsigned long lens[] = { 16, 52, 64, 100, 1000, 4096, 4100, 8192, 16384, 16420, 32768, 32780, 0 };
  unsigned char key[64], seqbuf[8], pk[64];
  chacha_state ch, hd;
  poly1305_state poly;
  unsigned long taglen, i, seq;
  int k;
  FILE *f = argc > 1 ? fopen(argv[1], "w") : stdout;
  for (i = 0; i < 64; i++) key[i] = (unsigned char)(i * 7 + 3);
  for (i = 0; i < sizeof in; i++) in[i] = (unsigned char)((i * 2654435761UL) >> 24);
  chacha_setup(&ch, key, 32, 20);
  chacha_setup(&hd, key + 32, 32, 20);
  for (k = 0; lens[k]; k++) {
    unsigned long len = lens[k];
    for (seq = 3; seq < 5; seq++) {
      memset(seqbuf, 0, 8);
      seqbuf[4] = (unsigned char)(seq >> 24); seqbuf[5] = (unsigned char)(seq >> 16); seqbuf[6] = (unsigned char)(seq >> 8); seqbuf[7] = (unsigned char)seq;
      chacha_ivctr64(&ch, seqbuf, 8, 0);
      chacha_keystream(&ch, pk, 64);
      poly1305_init(&poly, pk, 32);
      chacha_ivctr64(&hd, seqbuf, 8, 0);
      chacha_crypt(&hd, in, 4, out);
      chacha_ivctr64(&ch, seqbuf, 8, 1);
      chacha_crypt(&ch, in + 4, len - 4, out + 4);
      poly1305_process(&poly, out, len);
      taglen = 16;
      poly1305_done(&poly, out + len, &taglen);
      fprintf(f, "len %5lu seq %lu ct %08lx tag %08lx\n", len, seq, fnv(out, len, 2166136261UL), fnv(out + len, 16, 2166136261UL));
    }
  }
  if (f != stdout) fclose(f);
  return 0;
}
