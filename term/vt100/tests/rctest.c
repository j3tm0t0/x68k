/* rctest: 実機 (X68000 Z) で IOCS _TXRASCPY がどのプレーンをコピーするかを調べる probe。
 * row5 の各プレーンに別々のパターンを入れ、row3 (空) へ 4 ラスタをコピーし、直後と約 2 フレーム後に読む。
 * mask 0xF / 0x3 / 0x4 / 0xC の 4 通り。結果は stdout (rcmd が拾う)。
 * 2026-10-22 Z 実機: 4 通りとも期待どおり (4 プレーン全部コピーされる)。VT100 終了後に見えた
 * プレーン 2 の 1 行ずれは IOCS コンソールのスクロール (プレーン 0,1 のみ) が原因で、TXRASCPY は無関係。 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdio.h>
#define TVRAM ((volatile unsigned char *)0xE00000UL)
#define PLANE 0x20000UL
static const unsigned char pat[4] = { 0xAA, 0x55, 0xF0, 0x0F };
static void fill_row(int row, int p, unsigned char v)
{
  int s, c;
  for (s = 0; s < 16; s++) for (c = 0; c < 96; c++) TVRAM[p * PLANE + (unsigned long)(row * 16 + s) * 128 + c] = v;
}
static unsigned char rd(int row, int p) { return TVRAM[p * PLANE + (unsigned long)(row * 16 + 3) * 128 + 10]; }
static void wait_frames(int n) { struct iocs_time t0 = _iocs_ontime(); while (_iocs_ontime().sec - t0.sec < 4 * n) ; }
int main(void)
{
  static const int masks[4] = { 0xF, 0x3, 0x4, 0xC };
  unsigned char got0[4][4], got1[4][4];
  int m, p, sup;
  sup = _dos_super(0);
  for (m = 0; m < 4; m++) {
    for (p = 0; p < 4; p++) { fill_row(5, p, pat[p]); fill_row(3, p, 0); }
    wait_frames(2);
    _iocs_txrascpy((20 << 8) | 12, 4, masks[m]);
    for (p = 0; p < 4; p++) got0[m][p] = rd(3, p);
    wait_frames(2);
    for (p = 0; p < 4; p++) got1[m][p] = rd(3, p);
  }
  for (p = 0; p < 4; p++) { fill_row(5, p, 0); fill_row(3, p, 0); }
  if (sup >= 0) _dos_super(sup);
  for (m = 0; m < 4; m++)
    printf("mask %x: now %02x %02x %02x %02x  later %02x %02x %02x %02x (expect %02x %02x %02x %02x)\n", masks[m],
      got0[m][0], got0[m][1], got0[m][2], got0[m][3], got1[m][0], got1[m][1], got1[m][2], got1[m][3],
      (masks[m] & 1) ? pat[0] : 0, (masks[m] & 2) ? pat[1] : 0, (masks[m] & 4) ? pat[2] : 0, (masks[m] & 8) ? pat[3] : 0);
  return 0;
}
