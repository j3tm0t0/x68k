/* zprof.x: 常駐中の slipd.x (-z) の簡易プロファイラ ("SLPDPROF") を操作する。
 *   zprof on          : 記録を空にして始める (垂直同期ごとに、割り込まれた PC を 1 つ記録。最大 4096)
 *   zprof off         : 止める
 *   zprof dump FILE   : 止めて FILE に書く ("ZPRF", xip.x のテキスト先頭, 個数, PC の並び。すべてビッグエンディアン 4 バイト)
 * xip.x のテキスト先頭は、メモリから "TCP/IP" 01 00 48 E7 80 80 4E B9 を探して決める */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <x68k/dos.h>

int main(int argc, char **argv);
static const unsigned char xipsig[12] = { 'T', 'C', 'P', '/', 'I', 'P', 0x01, 0x00, 0x48, 0xe7, 0x80, 0x80 };

static uint32_t *find_prof(void)
{
  uint32_t *p;
  for (p = (uint32_t *)0x6800; p < (uint32_t *)0x200000; p++)
    if (p[0] == 0x534c5044 && p[1] == 0x50524f46 &&   /* "SLPD" "PROF" */
        !((unsigned char *)p > (unsigned char *)main && (unsigned char *)p < (unsigned char *)main + 0x10000))
      return p;
  return 0;
}

/* 見つかったものをすべて表示し、最初のものを返す (ディスクのバッファに xip.x の中身が残っていることもある) */
static uint32_t find_xip(void)
{
  unsigned char *p;
  uint32_t first = 0;
  for (p = (unsigned char *)0x6800; p < (unsigned char *)0xbffff0; p += 2)
    if (p[0] == 'T' && !memcmp(p, xipsig, sizeof xipsig) &&
        !(p > (unsigned char *)main && p < (unsigned char *)main + 0x10000)) {
      printf("zprof: xip.x text? %06lx\n", (unsigned long)p);
      if (!first)
        first = (uint32_t)p;
    }
  return first;
}

int main(int argc, char **argv)
{
  volatile uint32_t *z;
  _dos_super(0);
  z = find_prof();
  if (!z) {
    printf("zprof: slipd.x (SLPDPROF) が常駐していない\n");
    return 1;
  }
  if (argc >= 2 && !strcmp(argv[1], "on")) {
    z[2] = 0;
    z[3] = 0;
    z[2] = 1;
    printf("zprof: on (%06lx)\n", (unsigned long)z);
  } else if (argc >= 2 && !strcmp(argv[1], "off")) {
    z[2] = 0;
    printf("zprof: off, %lu samples\n", (unsigned long)z[3]);
  } else if (argc >= 3 && !strcmp(argv[1], "dump")) {
    uint32_t n, hdr[3];
    FILE *f;
    z[2] = 0;
    n = z[3] < 4096 ? z[3] : 4096;
    hdr[0] = 0x5a505246;              /* "ZPRF" */
    hdr[1] = find_xip();
    hdr[2] = n;
    f = fopen(argv[2], "wb");
    if (!f) {
      printf("zprof: %s を開けない\n", argv[2]);
      return 1;
    }
    fwrite(hdr, 4, 3, f);
    fwrite((const void *)(z + 4), 4, n, f);
    fclose(f);
    printf("zprof: %lu samples, xip text %06lx -> %s\n", (unsigned long)n, (unsigned long)hdr[1], argv[2]);
  } else {
    printf("usage: zprof on | off | dump FILE\n");
    return 1;
  }
  return 0;
}
