/* ホストで cp68k (C 版) の chacha20-poly1305@openssh.com を計算する (ref.py / csim と比べる)。
 * usage: hosttool INFILE > OUT   (INFILE = 鍵 64 + seq 4 (BE) + 平文、bench.c と同じ) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cp68k.h"
static void setup(uint32_t x[16], const unsigned char *k, uint32_t seq, uint32_t ctr)
{
  static const unsigned char sigma[16] = "expand 32-byte k";
  int i;
  for (i = 0; i < 4; i++) x[i] = sigma[4*i] | sigma[4*i+1] << 8 | sigma[4*i+2] << 16 | (uint32_t)sigma[4*i+3] << 24;
  for (i = 0; i < 8; i++) x[4+i] = k[4*i] | k[4*i+1] << 8 | k[4*i+2] << 16 | (uint32_t)k[4*i+3] << 24;
  x[12] = ctr; x[13] = 0;
  x[14] = 0; x[15] = (seq >> 24) | ((seq >> 8) & 0xff00) | ((seq << 8) & 0xff0000) | (seq << 24);
}
int main(int argc, char **argv)
{
  static unsigned char buf[1 << 20], out[1 << 20];
  FILE *f = fopen(argv[1], "rb");
  long n = fread(buf, 1, sizeof buf, f), len = n - 68;
  uint32_t x[16], seq = (uint32_t)buf[64] << 24 | buf[65] << 16 | buf[66] << 8 | buf[67];
  unsigned char pk[64];
  memset(pk, 0, sizeof pk);
  setup(x, buf, seq, 0); chacha68k_xor(x, pk, pk, 64);
  setup(x, buf + 32, seq, 0); chacha68k_xor(x, buf + 68, out, 4);
  setup(x, buf, seq, 1); chacha68k_xor(x, buf + 72, out + 4, len - 4);
  poly68k_mac(out + len, out, len, pk);
  fwrite(out, 1, len + 16, stdout);
  return 0;
}
