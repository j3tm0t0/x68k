/* pcgtest.x: PCG スプライトの可否を調べるプロトタイプ。
 * 768x512 の高解像度テキスト (CRTMOD 16、CLI の既定) ではスプライト/BG が使えず sp_init が -1 を返す。
 * 他の画面モード (256x256 / 512x512) に切り替えると sp_init が通るかを確かめ、結果をファイルに書く。 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdio.h>

int main(void)
{
  static const int modes[] = { 16, 4, 6, 8, 12, 14 };   /* 16=768x512(既定) 他は 256/512 系 */
  int results[6];
  int i;
  for (i = 0; i < 6; i++) {
    _iocs_crtmod(modes[i]);
    results[i] = _iocs_sp_init();   /* 0=可 / -1=不可 */
    if (results[i] == 0) _iocs_sp_off();
  }
  _iocs_crtmod(16);   /* 768x512 テキストに戻す */
  _iocs_b_consol(0, 0, 95, 31);
  { FILE *f = fopen("PCG.OUT", "wb");
    if (f) {
      for (i = 0; i < 6; i++) fprintf(f, "crtmod=%d sp_init=%d\n", modes[i], results[i]);
      fclose(f);
    }
  }
  return 0;
}
