/* txrascpy の仕様を実機で確かめる probe。
 * cell(0,row5) に 'A' を描き、txrascpy で上へ寄せて、結果を /HOME/TXT.OUT に書く。
 * ラスタ単位 = 4 走査線 (1024 線 = 256 ラスタ) と仮定。cell row r の先頭ラスタ = r*4。 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdio.h>

#define TVRAM ((volatile unsigned char *)0xE00000UL)
#define CGROM16 ((const unsigned char *)0xF3A800UL)
#define PLANE 0x20000UL

static void blitA(int col, int row)
{
  const unsigned char *g = CGROM16 + 'A' * 16;
  int s;
  for (s = 0; s < 16; s++) {
    volatile unsigned char *v = TVRAM + (unsigned long)(row * 16 + s) * 128 + col;
    v[0] = g[s]; v[PLANE] = g[s];   /* plane0,1 に */
  }
}
static int cell_has_ink(int col, int row)
{
  int s, n = 0;
  for (s = 0; s < 16; s++) {
    volatile unsigned char *v = TVRAM + (unsigned long)(row * 16 + s) * 128 + col;
    if (v[0]) n++;
  }
  return n;
}

int main(void)
{
  int sup = _dos_super(0);
  int r, c;
  FILE *f;
  /* clear planes 0,1 */
  for (r = 0; r < 1024; r++)
    for (c = 0; c < 96; c++) { TVRAM[(unsigned long)r * 128 + c] = 0; TVRAM[PLANE + (unsigned long)r * 128 + c] = 0; }
  blitA(0, 5);   /* cell row 5 = rasters 20..23 */
  /* 仮説: d1=(src<<8)|dst, d2=count(ラスタ), d3=plane mask. row5 の 4 ラスタ(20..23) を row3(12..15) へ */
  _iocs_txrascpy((20 << 8) | 12, 4, 0x000f);
  _dos_super(sup);
  f = fopen("/HOME/TXT.OUT", "wb");
  if (f) {
    fprintf(f, "row5 ink=%d  row3 ink=%d  row4 ink=%d\n", cell_has_ink(0, 5), cell_has_ink(0, 3), cell_has_ink(0, 4));
    fclose(f);
  }
  return 0;
}
