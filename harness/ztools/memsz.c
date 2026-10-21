/* memsz.x: SRAM の主記憶容量と、いま確保できる最大のメモリブロックを表示する */
#include <stdio.h>
#include <x68k/dos.h>
int main(void)
{
  int ssp = _dos_super(0);
  unsigned long ram = *(volatile unsigned long *)0xed0008;
  _dos_super(ssp);
  printf("ram=%lu KB  free(max block)=%ld KB\n", ram / 1024, ((long)_dos_malloc(0x7fffffff) & 0xffffff) / 1024);
  return 0;
}
