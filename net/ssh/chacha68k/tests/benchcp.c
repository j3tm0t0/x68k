/* csim 用: bench.c と同じ手順を cp68k で (区間 1 = chacha、区間 2 = poly1305)。入出力も bench.c と同じ。 */
#include <string.h>
#include "cp68k.h"
#define SEC_ON(n)  (*(volatile unsigned long *)0x7fffd0 = (n))
#define SEC_OFF(n) (*(volatile unsigned long *)0x7fffd4 = (n))
static void setup(uint32_t x[16], const unsigned char *k, const unsigned char *seq, uint32_t ctr)
{
  static const unsigned char sigma[16] = "expand 32-byte k";
  int i;
  for (i = 0; i < 4; i++) x[i] = sigma[4*i] | sigma[4*i+1] << 8 | (uint32_t)sigma[4*i+2] << 16 | (uint32_t)sigma[4*i+3] << 24;
  for (i = 0; i < 8; i++) x[4+i] = k[4*i] | k[4*i+1] << 8 | (uint32_t)k[4*i+2] << 16 | (uint32_t)k[4*i+3] << 24;
  x[12] = ctr; x[13] = 0; x[14] = 0;
  x[15] = seq[0] | seq[1] << 8 | (uint32_t)seq[2] << 16 | (uint32_t)seq[3] << 24;
}
void bench(void)
{
  unsigned char *in = (unsigned char *)0x800000, *out = (unsigned char *)0x900000;
  unsigned long n = *(unsigned long *)0x7ffff4, len = n - 68;
  static unsigned char pk[64];
  uint32_t x[16];
  const unsigned char *pt = in + 68;
  *(volatile unsigned long *)0x7fffe0 = 1;
  SEC_ON(1);
  memset(pk, 0, 64);
  setup(x, in, in + 64, 0); chacha68k_xor(x, pk, pk, 64);
  setup(x, in + 32, in + 64, 0); chacha68k_xor(x, pt, out, 4);
  setup(x, in, in + 64, 1); chacha68k_xor(x, pt + 4, out + 4, len - 4);
  SEC_OFF(1);
  SEC_ON(2);
  poly68k_mac(out + len, out, len, pk);
  SEC_OFF(2);
  *(volatile unsigned long *)0x7fffe4 = (unsigned long)out;
  *(volatile unsigned long *)0x7fffe8 = len + 16;
}
