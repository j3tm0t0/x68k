/* e2e_con.py: Human68k のバスエラーの箱を出す (スーパーバイザで、何もつながっていない番地を読む) */
#include <x68k/dos.h>
int main(void)
{
  _dos_super(0);
  return *(volatile unsigned short *)0xeafff0;
}
