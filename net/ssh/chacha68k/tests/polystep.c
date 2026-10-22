/* csim 用: poly68k_blocks の C 版と asm 版を 1 ブロックずつ同じ入力で回し、状態 (h0..h3, h4) を並べて出す。
 * 入力: 鍵 32 + メッセージ (16 の倍数)。出力: ブロックごとに C 20 バイト + asm 20 バイト。 */
#include <string.h>
#include "cp68k.h"
extern void poly68k_blocks_asm(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit);
void bench(void)
{
  unsigned char *in = (unsigned char *)0x800000, *out = (unsigned char *)0x900000;
  unsigned long n = (*(unsigned long *)0x7ffff4 - 32) / 16, i;
  poly68k_state a, b;
  poly68k_init(&a, in); poly68k_init(&b, in);
  *(volatile unsigned long *)0x7fffe0 = 1;
  for (i = 0; i < n; i++) {
    poly68k_blocks(&a, in + 32 + 16 * i, 1, 1);
    poly68k_blocks_asm(&b, in + 32 + 16 * i, 1, 1);
    memcpy(out, &a, 20); memcpy(out + 20, &b, 20); out += 40;
  }
  *(volatile unsigned long *)0x7fffe4 = 0x900000;
  *(volatile unsigned long *)0x7fffe8 = 40 * n;
}
