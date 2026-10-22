/* Musashi 上で vt_write を回す素の 68000 プログラム (VT_HOSTTEST 版の vt100.c をリンク) */
#include "vt100.h"
typedef unsigned long u32;
#define ARG ((volatile u32 *)0x7ffff0)   /* 0: fixture addr 1: size 2: chunk 3: cols<<16|rows */
#define DONE (*(volatile u32 *)0x7fffe0)
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
void *memmove(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
void bench(void)
{
  const unsigned char *buf = (const unsigned char *)ARG[0];
  long fsz = (long)ARG[1], off; int chunk = (int)ARG[2];
  vt_init((int)(ARG[3] >> 16), (int)(ARG[3] & 0xffff));
  DONE = 1;   /* 初期化の終わり (ここからの時間を測る) */
  for (off = 0; off < fsz; off += chunk) {
    int n = (off + chunk <= fsz) ? chunk : (int)(fsz - off);
    vt_write(buf + off, n);
  }
  DONE = 2;
  for (;;) ;
}
/* TXRASCOPY: シミュレータに頼む (0x7fffd0 に書くとシミュレータが VRAM をコピーし、ラスタ数ぶんの時間を足す) */
void _iocs_txrascpy(int d1, int d2, int d3)
{
  volatile u32 *t = (volatile u32 *)0x7fffd0;
  t[1] = (u32)d1; t[2] = (u32)d2; t[3] = (u32)d3; t[0] = 1;
}
