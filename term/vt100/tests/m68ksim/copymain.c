/* Musashi 上で vram_move_lines (CPU で写す経路) を回す素の 68000 プログラム (tests/copytest.sh)。
 * 桁数 1〜96 (ロング数 1〜24) x ライン数 1〜32 の組ごとに、VRAM の先頭 64 ライン (4 プレーン) を決まった乱数で埋めて
 * 5 ライン上へ写し、その 64 ラインのハッシュを 0x700000 からの表に書く。asm の版と C の版 (VT_NO_ASM) で表を比べる */
#include "vt100.h"
typedef unsigned long u32;
#define RES ((volatile u32 *)0x700000)
#define DONE (*(volatile u32 *)0x7fffe0)
extern unsigned char vt_host_vram[];
void vt_test_move(int cols, int dst_line, int src_line, int nlines);
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
void *memmove(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; } return d; }
void bench(void)
{
  int cols, n, k = 0, p;
  u32 seed = 12345;
  vt_init(96, 32);
  for (cols = 1; cols <= 96; cols++)
    for (n = 1; n <= 32; n++) {
      u32 h = 2166136261u, i;
      for (p = 0; p < 4; p++) {
        u32 *v = (u32 *)(vt_host_vram + p * 0x20000);
        for (i = 0; i < 64 * 128 / 4; i++) { seed = seed * 1103515245u + 12345u; v[i] = seed; }
      }
      vt_test_move(cols, 0, 5, n);
      for (p = 0; p < 4; p++) {
        u32 *v = (u32 *)(vt_host_vram + p * 0x20000);
        for (i = 0; i < 64 * 128 / 4; i++) h = (h ^ v[i]) * 16777619u;
      }
      RES[k++] = h;
    }
  RES[k] = 0xdeadbeef;
  DONE = 2;
  for (;;) ;
}
