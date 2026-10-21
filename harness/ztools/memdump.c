/* memdump.x: ADDR から LEN バイトを FILE に書く (スーパーバイザで読むので ROM や I/O の近くも読める)
 * 1 ワードずつ DOS _BUS_ERR で読み、バスエラーになる所は 0xffff で埋めて数える
 * (0xea0000 からそのまま読んだら、バスエラーで止まった)
 *   memdump ea0020 1fe0 b:scsirom.bin */
#include <stdio.h>
#include <stdlib.h>
#include <x68k/dos.h>

/* DOS _BUS_ERR: s から d へ size バイト読む。バスエラーなら 0 以外 */
static int bus_err(const void *s, void *d, int size)
{
  register int r __asm__("d0");
  __asm__ volatile("move.w %3,-(%%sp)\n\tpea (%2)\n\tpea (%1)\n\t.dc.w 0xfff7\n\tlea 10(%%sp),%%sp"
                   : "=d"(r) : "a"(s), "a"(d), "d"(size) : "d1", "d2", "a0", "a1", "a2", "memory", "cc");
  return r;
}
int main(int argc, char **argv)
{
  unsigned long a, n, bad = 0;
  FILE *f;
  if (argc < 4) {
    printf("usage: memdump ADDR LEN FILE (16 進)\n");
    return 1;
  }
  a = strtoul(argv[1], 0, 16);
  n = strtoul(argv[2], 0, 16);
  f = fopen(argv[3], "wb");
  if (!f) {
    printf("memdump: %s を開けない\n", argv[3]);
    return 1;
  }
  while (n) {
    static unsigned short buf[2048];
    unsigned long k = n < sizeof buf ? n : sizeof buf, i;
    for (i = 0; i < k / 2; i++)
      if (bus_err((unsigned short *)a + i, buf + i, 2)) {
        buf[i] = 0xffff;
        bad++;
      }
    fwrite(buf, 1, k, f);
    a += k;
    n -= k;
  }
  fclose(f);
  printf("memdump: -> %s (バスエラー %lu ワード)\n", argv[3], bad);
  return 0;
}
