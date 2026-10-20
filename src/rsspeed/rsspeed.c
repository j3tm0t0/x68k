/*
 * rsspeed.x -- IOCS _SET232C を直接呼んで RS-232C (AUX) の通信モードを設定する
 *
 * X68000 Z では tmsio.x / bsio.x が SCC に書き込むボーレートが UART に反映されず、
 * IOCS (SRAM の既定 = 9600bps) のままになる。tmsio を常駐させる前にこれで IOCS に
 * モードを設定しておくと、その後の tmsio / ppp.x の設定にかかわらず UART はこの速度で動く。
 *
 *   usage: rsspeed.x [mode(hex)]      既定 4C09 = stop 1 / parity なし / 8bit / 速度コード 9 (38400)
 *          速度コード (下位 4bit): 7=9600 8=19200 9=38400 (TMSIO 系の並び)
 *   build: gcc rsspeed.c -o rsspeed.x -liocs
 */
#include <stdio.h>
#include <stdlib.h>
#ifdef __ELF__
#include <x68k/iocs.h>      /* elf2x68k (Mac でクロスビルド: dev イメージの AUTOEXEC.BAT 用) */
#else
#include <iocslib.h>
#endif

int main(int argc, char **argv)
{
  int mode = 0x4C09, old;

  if (argc > 1)
    mode = (int)strtol(argv[1], (char **)0, 16);
  old = _iocs_set232c(mode);
  printf("rsspeed: SET232C 0x%04X -> 0x%04X\n", old & 0xffff, mode & 0xffff);
  return 0;
}
