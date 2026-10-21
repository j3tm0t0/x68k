/* ztr.x: 常駐中の slipd.x のトレース ("SLPDTRAC") を古い順に表示する */
#include <stdio.h>
#include <stdint.h>
#include <x68k/dos.h>
struct ent { int32_t t; uint16_t tx, rx, pk, pad; };
int main(void)
{
  uint32_t *p;
  _dos_super(0);
  for (p = (uint32_t *)0x6800; p < (uint32_t *)0x200000; p++)
    if (p[0] == 0x534c5044 && p[1] == 0x54524143) {   /* "SLPD" "TRAC" */
      uint32_t pos = p[2], i, n = pos < 256 ? pos : 256;
      struct ent *e = (struct ent *)(p + 3);
      int t0 = -1;
      if ((unsigned char *)p > (unsigned char *)main && (unsigned char *)p < (unsigned char *)main + 0x10000)
        continue;                                       /* 自分の中の文字列 */
      printf("at %06x pos %lu\n", (unsigned)p, pos);
      for (i = pos - n; i < pos; i++) {
        struct ent *x = &e[i % 256];
        if (t0 < 0) t0 = x->t;
        printf("%5d tx%5u rx%5u pk%u\n", (x->t - t0) * 10, x->tx, x->rx, x->pk);
      }
    }
  return 0;
}
