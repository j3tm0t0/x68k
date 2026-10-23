/*
 * zdrv.x -- Z のリモートドライブ (x68kzremotedrv の scsiremote.sys、デバイス名 "\x01ZREMOTE") が付いたドライブ名を探し、
 *           決まった名前 (既定 Z:) を仮想ドライブ (DOS _ASSIGN の 0x50、SUBST と同じ。0x60 は -14 で断られた) としてその根に割り当てる
 *
 *   zdrv.x          Z: = リモートドライブ (見つからなければ何もせず 1 を返す)
 *   zdrv.x X:       X: に
 *   zdrv.x -l       ドライブごとにデバイスドライバの名前を表示する
 * データの HDS (SD) が増えてリモートドライブが B: から C: にずれても、AUTOEXEC.BAT で zdrv.x してから z: を使えばよい
 */
#define __DOS_INLINE__
#include <stdio.h>
#include <string.h>
#include <x68k/dos.h>

/* ドライブ d (1 = A:) のデバイスドライバの名前 (見出しの +14、8 バイト) を name に。無ければ 0。
   ドライバはスーパーバイザ領域にあるので (ユーザモードで読むとバスエラー)、_SUPER の中で写す */
static int driver_name(int d, unsigned char *name, unsigned *addr)
{
  struct dos_dpbptr dpb;
  const unsigned char *h;
  int ssp;
  if (_dos_getdpb(d, &dpb) < 0)
    return 0;
  h = (const unsigned char *)dpb.driver;
  *addr = (unsigned)h;
  if (((unsigned)h & 1) || (unsigned)h >= 0xc00000)
    return 0;
  ssp = _dos_super(0);
  memcpy(name, h + 14, 8);
  _dos_super(ssp);
  return 1;
}

int main(int argc, char **argv)
{
  int list = argc > 1 && !strcmp(argv[1], "-l");
  int target = 'Z';
  if (argc > 1 && !list && argv[1][0] && argv[1][1] == ':')
    target = argv[1][0] & ~0x20;
  for (int d = 1; d <= 26; d++) {
    unsigned char nm[8];
    unsigned a = 0;
    if (!driver_name(d, nm, &a))
      continue;
    if (list) {
      printf("%c: driver %08x \"", 'A' + d - 1, a);
      for (int i = 0; i < 8; i++)
        putchar(nm[i] >= 0x20 && nm[i] < 0x7f ? nm[i] : '.');
      printf("\"\n");
      continue;
    }
    if (!memcmp(nm + 1, "ZREMOTE", 7)) {
      char from[4] = { (char)target, ':', 0 }, to[4] = { (char)('A' + d - 1), ':', '\\', 0 };
      int r = _dos_makeassign(from, to, 0x50);
      if (r < 0) printf("zdrv: %s = %s (remote drive) failed (%d)\n", from, to, r); else printf("zdrv: %s = %s (remote drive)\n", from, to);
      return r < 0 ? 2 : 0;
    }
  }
  if (!list)
    printf("zdrv: no remote drive (ZREMOTE)\n");
  return list ? 0 : 1;
}
