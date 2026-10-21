/* zmb.x: Z の Pseudo SCSI BIOS のメールボックス (0xEA0060) を直に叩いて読み、IOCS _S_READEXT の結果と比べる
 *   zmb ID LBA N   (16 進。N セクタ、最大 8)
 * 割り込みを止めたまま完了を待つ (ほかのスレッドの SCSI と混ざらないように)。待ちは上限付き */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

#define MB ((volatile unsigned char *)0xea0060)
static unsigned char a[8 * 512], b[8 * 512];

int main(int argc, char **argv)
{
  int id, n, sr, r;
  unsigned long lba, spin;
  if (argc < 4) {
    printf("usage: zmb ID LBA N\n");
    return 1;
  }
  id = strtol(argv[1], 0, 16);
  lba = strtoul(argv[2], 0, 16);
  n = strtol(argv[3], 0, 16);
  if (n < 1 || n > 8)
    return 1;
  memset(a, 0x55, sizeof a);
  memset(b, 0xaa, sizeof b);
  r = _iocs_s_readext(lba, n, id, 1, a);
  printf("iocs: %d\n", r);
  _dos_super(0);
  __asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr));
  MB[2] = id;
  MB[3] = 0x28;
  *(volatile unsigned long *)(MB + 4) = lba;
  *(volatile unsigned long *)(MB + 8) = n * 512;
  *(volatile unsigned long *)(MB + 12) = (unsigned long)b;
  MB[0] = 0x80;
  MB[1] = 4;
  for (spin = 0; (MB[0] & 0x80) && spin < 20000000; spin++)
    ;
  MB[1] = 0;
  r = (*(volatile unsigned short *)(MB + 16) << 16) | *(volatile unsigned short *)(MB + 18);
  __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
  printf("mailbox: status %08x, spin %lu, %s\n", r, spin, memcmp(a, b, n * 512) ? "DIFFERENT" : "same");
  printf("%.16s\n", (char *)b);
  return 0;
}
