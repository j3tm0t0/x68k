/* keyprobe.x: _iocs_b_keyinp の戻り値を調べ、スキャンコードが bit8-15 にあることを確かめる。
 * キーを押すたび raw/scan/ascii を /HOME/KEY.OUT に追記し、'q' (ascii 0x71) で終了する。 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdio.h>

int main(void)
{
  FILE *f = fopen("/HOME/KEY.OUT", "wb");
  int n = 0;
  if (!f) return 1;
  fprintf(f, "press keys (q to quit): up down left right home del rollup rolldown f1 q\n");
  fflush(f);
  for (;;) {
    int raw, scan, asc;
    while (_iocs_b_keysns() == 0) { volatile long d; for (d = 0; d < 5000; d++) ; }
    raw = _iocs_b_keyinp();
    scan = (raw >> 8) & 0xff;
    asc = raw & 0xff;
    fprintf(f, "%2d: raw=%08x scan=%02x ascii=%02x\n", n++, raw, scan, asc);
    fflush(f);
    if (asc == 'q') break;
    if (n > 40) break;
  }
  fclose(f);
  return 0;
}
