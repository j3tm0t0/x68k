/*
 * tcset.x -- SCC ch.B の時定数 (WR12/WR13) を読み書きする試験用 (zmux.sys の 78125bps の見張りの試験)
 *   tcset r     WR12 と WR13 を表示   tcset N   WR12 = N、WR13 = 0 に書く
 */
#define __DOS_INLINE__
#include <x68k/dos.h>
#include <stdint.h>
extern char *mem_ptr;
extern unsigned char *cmd_line;

static void putnum(unsigned long v)
{
  char b[12], *p = b + 11;
  *p = 0;
  do { *--p = '0' + v % 10; v /= 10; } while (v);
  _dos_print(p);
}

int zmux_main(void)
{
  unsigned char *a = cmd_line + 1;
  volatile uint8_t *c = (volatile uint8_t *)0xe98005;
  uint16_t sr;
  int ssp, x = 0;
  uint8_t r12, r13;
  while (*a == ' ') a++;
  ssp = _dos_super(0);
  __asm__ __volatile__("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
  if (*a >= '0' && *a <= '9') {
    while (*a >= '0' && *a <= '9') x = x * 10 + *a++ - '0';
    *c = 12; *c = x;
    *c = 13; *c = 0;
  }
  *c = 12; r12 = *c;
  *c = 13; r13 = *c;
  __asm__ __volatile__("move.w %0,%%sr" : : "d"(sr) : "cc");
  if (ssp > 0) _dos_super(ssp);
  _dos_print("TCSET wr12="); putnum(r12); _dos_print(" wr13="); putnum(r13); _dos_print("\r\n");
  return 0;
}
