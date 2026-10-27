/* reset.c -- X68000 をソフトリセットする (IPL ROM のリセットの番地に飛ぶ)。boot/serial の道具 */
#include <x68k/dos.h>

int main(void)
{
  _dos_super(0);
  __asm__ volatile (
    "ori.w #0x0700,%sr\n\t"
    "movea.l 0xff0000,%sp\n\t"
    "movea.l 0xff0004,%a0\n\t"
    "jmp (%a0)");
  return 0;
}
