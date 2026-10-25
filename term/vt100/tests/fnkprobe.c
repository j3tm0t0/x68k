/* fnkprobe -- vt_init の間、ファンクションキー行が消えて最下行が端末の行になり、vt_fini で戻るかを XEiJ で見る試験。
   usage: fnkprobe [秒 [p]]  (p: vt_init の後に DOS の出力も出す)   vt_init → 最下行に印を書いて N 秒待つ → vt_fini → 戻ったモードを表示してまた N 秒待つ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include "../vt100.h"

static void wait_s(int s)
{
  int t0 = _iocs_ontime().sec;
  while (_iocs_ontime().sec - t0 < s * 100) ;
}

int main(int argc, char **argv)
{
  int s = argc > 1 ? atoi(argv[1]) : 15, before, after;
  char m[64];
  before = _dos_c_fnkmod(-1);
  vt_init(0, 0);
  if (argc > 2) {   /* ssh.x は端末の間も DOS の出力 (ログ) を出す。IOCS のカーソルが戻ってしまわないか見る */
    _dos_print("log line from DOS\r\n");
  }
  strcpy(m, "\x1b[32;1H\x1b[7mBOTTOM ROW OF THE TERMINAL\x1b[0m");
  vt_write((const unsigned char *)m, strlen(m));
  wait_s(s);
  vt_fini();
  after = _dos_c_fnkmod(-1);
  printf("\r\nfnkmod before %d after %d\r\n", before, after);
  fflush(stdout);
  wait_s(s);
  return before == after ? 0 : 1;
}
