/* zt.x: SCSI ID 6 の読み書き 1 回にかかる時間を測る (netpipe の帯は使わない) */
#include <stdio.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>
static unsigned char buf[8 * 512];
int main(void)
{
  int t0, t1, t2, t3, i, sect = 0x1f0000;
  _dos_super(0);
  t0 = _iocs_ontime().sec;
  for (i = 0; i < 100; i++)
    _iocs_s_readext(sect -= 8, 1, 6, 1, buf);      /* netpipe の応答帯 (中身は使わない) */
  t1 = _iocs_ontime().sec;
  for (i = 0; i < 100; i++)
    _iocs_s_readext(sect -= 8, 8, 6, 1, buf);
  t2 = _iocs_ontime().sec;
  for (i = 0; i < 100; i++)
    _iocs_s_writeext(0x30, 1, 6, 1, buf);          /* どこにも使われない LBA (ファームは -1 を返す) */
  t3 = _iocs_ontime().sec;
  printf("read1 %d0us  read8 %d0us  write1 %d0us (per op)\n", (t1 - t0), (t2 - t1), (t3 - t2));
  return 0;
}
