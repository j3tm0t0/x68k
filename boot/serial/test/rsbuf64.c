/* rsbuf64.x -- 試験用: RSDRV.SYS の内蔵 RS-232C の受信バッファを 64 バイトに戻す。
 * XEiJ は RSDRV.SYS 2.02 を見つけると受信バッファを 64 から 1024 バイトに広げるパッチをあてるので、
 * 実機と同じ大きさで試すために、試験用のフロッピーの AUTOEXEC.BAT から起動する (test/xeij-fd.sh が入れる)。
 * 受信割り込み ($170) の入口 "movem.l d0-d1/a0/a2,-(sp); lea 制御ブロック,a2" から制御ブロックを見つける
 * (+8 先頭 +12 末尾 +16 書き込み位置 +20 読み出し位置 +24 個数.w +26 大きさ.w)。 */
#include <x68k/dos.h>
#include <x68k/iocs.h>
int main(void)
{
  int ssp = _dos_super(0);
  unsigned short *h = *(unsigned short **)0x170, sr, old = 0;
  unsigned char *cb;
  int ok = 0;
  if (h[0] == 0x48e7 && h[1] == 0xc0a0 && h[2] == 0x45f9) {
    cb = *(unsigned char **)(h + 3);
    __asm__ volatile ("move.w %%sr,%0; ori.w #0x0700,%%sr" : "=d"(sr) :: "memory");
    old = *(unsigned short *)(cb + 26);
    *(unsigned char **)(cb + 12) = *(unsigned char **)(cb + 8) + 64;
    *(unsigned char **)(cb + 16) = *(unsigned char **)(cb + 8);
    *(unsigned char **)(cb + 20) = *(unsigned char **)(cb + 8);
    *(unsigned short *)(cb + 24) = 0;
    *(unsigned short *)(cb + 26) = 64;
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
    ok = 1;
  }
  if (ssp >= 0) _dos_super(ssp);
  _dos_print(ok ? (old == 64 ? "rsbuf64: already 64\r\n" : "rsbuf64: RS-232C receive buffer -> 64 bytes\r\n")
                : "rsbuf64: RSDRV not found\r\n");
  _dos_exit();
}
