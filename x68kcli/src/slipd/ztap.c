/* ztap.x: 常駐中の slipd.x が残したパケットの先頭 ("SLPDTAP_") を古い順に表示する */
#include <stdio.h>
#include <stdint.h>
#include <x68k/dos.h>
struct ent { int32_t t; uint8_t dir, pad; uint16_t len; uint8_t b[28]; };
int main(void)
{
  uint32_t *p;
  _dos_super(0);
  for (p = (uint32_t *)0x6800; p < (uint32_t *)0x200000; p++)
    if (p[0] == 0x534c5044 && p[1] == 0x5441505f) {   /* "SLPD" "TAP_" */
      uint32_t pos = p[2], i, n = pos < 64 ? pos : 64;
      struct ent *e = (struct ent *)(p + 3);
      if ((unsigned char *)p > (unsigned char *)main && (unsigned char *)p < (unsigned char *)main + 0x10000)
        continue;
      printf("at %06x pos %lu\n", (unsigned)p, pos);
      for (i = pos - n; i < pos; i++) {
        struct ent *x = &e[i % 64];
        uint8_t *b = x->b;
        int hl = (b[0] & 15) * 4;
        printf("%7ld %c %4u ttl%3u %u.%u.%u.%u>%u.%u.%u.%u ", (long)x->t, x->dir, x->len, b[8],
               b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19]);
        if (b[9] == 1)
          printf("icmp %u/%u", b[hl], b[hl + 1]);
        else if (b[9] == 6 && hl + 4 <= 28)
          printf("tcp %u>%u", (b[hl] << 8) | b[hl + 1], (b[hl + 2] << 8) | b[hl + 3]);
        else if (b[9] == 17 && hl + 4 <= 28)
          printf("udp %u>%u", (b[hl] << 8) | b[hl + 1], (b[hl + 2] << 8) | b[hl + 3]);
        else
          printf("proto %u", b[9]);
        printf("\n");
      }
    }
  return 0;
}
