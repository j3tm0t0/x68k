/* Musashi 上で zterm のフレーム列を ztd_frame に流す素の 68000 プログラム (term/vt100/tests/m68ksim/benchmain.c の zterm 版)。
 * ARG[0] = フレーム列の番地、ARG[1] = バイト数 (ARG[2] chunk・ARG[3] 寸法は使わない: 寸法は HELLO で決まる) */
#include "vt100.h"
#include "ztclient.h"
typedef unsigned long u32;
#define ARG ((volatile u32 *)0x7ffff0)
#define DONE (*(volatile u32 *)0x7fffe0)
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
void *memmove(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
void bench(void)
{
  const unsigned char *p = (const unsigned char *)ARG[0];
  long n = (long)ARG[1], o = 0;
  int first = 1;
  while (o + 3 <= n) {
    int len = (p[o + 1] << 8) | p[o + 2];
    ztd_frame(p + o + 3, len);
    if (first) { DONE = 1; first = 0; }   /* HELLO (vt_init) の後から測る */
    o += 3 + len;
  }
  DONE = 2;
  for (;;) ;
}
void _iocs_txrascpy(int d1, int d2, int d3)
{
  volatile u32 *t = (volatile u32 *)0x7fffd0;
  t[1] = (u32)d1; t[2] = (u32)d2; t[3] = (u32)d3; t[0] = 1;
}
