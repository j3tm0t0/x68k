/*
 * crtmode.x -- Human68k の文字の画面を指定した画面モード (_CRTMOD) にする。
 *
 *   crtmode 19     640x480 (31kHz) にする。80 桁 x 29 行 + ファンクションキー行
 *   crtmode -k off 最下行のファンクションキー行を消し、コンソールを最下行まで広げる
 *   crtmode -k on  ファンクションキー行を戻す。MODE と一緒にも使える (crtmode 19 -k off)
 *   crtmode -f N   _CRTMOD の結果を確かめずにコンソールを設定する
 *   crtmode 16     768x512 (31kHz) に戻す。96 桁 x 31 行 + ファンクションキー行
 *
 * _CRTMOD はコンソールの範囲を全画面に戻し、画面を消すので、そのあとで
 *   1. ファンクションキー行を元の表示モードで描き直し (480 行以下のモードでは画面の最下行に写し)、
 *   2. コンソールの範囲 (最下行を除く) を設定し、
 *   3. 画面を消して、カーソルを左上に置く。
 * 画面の大きさは、テキストの桁数 = 幅 / 8、行数 = 高さ / 16 (モード 0-19 の表)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __IOCS_INLINE__
#include <x68k/iocs.h>
#define __DOS_INLINE__
#include <x68k/dos.h>

/* 見える大きさ (幅, 高さ)。0-19 */
static const short size_tab[20][2] = {
  {512,512},{512,480},{256,256},{256,240},{512,512},{512,480},{256,256},{256,240},
  {512,512},{512,480},{256,256},{256,240},{512,512},{512,480},{256,256},{256,240},
  {768,512},{1024,424},{1024,848},{640,480}
};

/* _CRTMOD は d0/d1 以外のレジスタも壊すことがあるので、全部壊れるものとして呼ぶ */
static int crtmod(int mode)
{
  register int d1 __asm__("d1") = mode;
  register int d0 __asm__("d0") = 0x10;
  __asm__ __volatile__ ("trap #15" : "+d"(d0), "+d"(d1) : : "d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","cc","memory");
  return d0;
}

/* スーパーバイザ (テキスト VRAM は特権空間) */
static int sup_enter(void)
{
  unsigned short sr;
  __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
  return (sr & 0x2000) ? -1 : _dos_super(0);
}
static void sup_leave(int s) { if (s >= 0) _dos_super(s); }

/* ファンクションキー行は、Human68k が画面の高さにかかわらず 31 行目 (ラスタ 496-511) に描く。
   480 行以下のモードでは画面の外になるので、テキスト VRAM のその行を画面の最下行に写す。
   写すのは左の cols 桁 (幅が 96 桁より狭いので右の端の字は切れる) */
static void copy_fnk_row(int rows, int cols)
{
  int p, y, s = sup_enter();
  volatile unsigned char *vram = (volatile unsigned char *)0xE00000UL;
  for (p = 0; p < 4; p++)
    for (y = 0; y < 16; y++) {
      volatile unsigned char *src = vram + (long)p * 0x20000 + (long)(496 + y) * 128;
      volatile unsigned char *dst = vram + (long)p * 0x20000 + (long)((rows - 1) * 16 + y) * 128;
      int x;
      for (x = 0; x < cols; x++) dst[x] = src[x];
    }
  sup_leave(s);
}

/* ファンクションキー行 (画面の最下行) の文字を消す */
static void clear_fnk_row(int rows, int cols)
{
  int p, y, x, s = sup_enter();
  volatile unsigned char *vram = (volatile unsigned char *)0xE00000UL;
  for (p = 0; p < 4; p++)
    for (y = 0; y < 16; y++)
      for (x = 0; x < cols; x++) vram[(long)p * 0x20000 + (long)((rows - 1) * 16 + y) * 128 + x] = 0;
  sup_leave(s);
}

static int usage(void)
{
  fprintf(stderr, "usage: crtmode [-f] [MODE] [-k on|off]   MODE 0-19 (19 = 640x480, 16 = 768x512); -k = function key row\n");
  return 1;
}

int main(int argc, char **argv)
{
  int mode = -1, fnk, w, h, rows, i, key = -1, force = 0, cur, given, ccol = 0, crow = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-f")) force = 1;
    else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
      i++;
      if (!strcmp(argv[i], "on")) key = 1; else if (!strcmp(argv[i], "off")) key = 0; else return usage();
    }
    else if (argv[i][0] >= '0' && argv[i][0] <= '9') mode = atoi(argv[i]);
    else return usage();
  }
  if (mode < 0 && key < 0) return usage();
  given = mode >= 0;
  if (mode > 19) { fprintf(stderr, "crtmode: mode must be 0-19\n"); return 1; }
  fnk = _dos_c_fnkmod(-1);                       /* ファンクションキー行の表示モード */
  if (fnk < 0 || fnk > 3) fnk = 0;
  if (key == 0) fnk = 2;                         /* 2 = 表示しない */
  else if (key == 1 && fnk == 2) fnk = 0;
  cur = crtmod(-1);
  if (mode >= 0) {
    if (crtmod(mode) < 0) { fprintf(stderr, "crtmode: _CRTMOD %d failed\n", mode); return 1; }
    if (!force && crtmod(-1) != mode) {   /* 古い ROM は 19 などを黙って無視する。そのままコンソールを変えると崩れる */
      fprintf(stderr, "crtmode: this machine did not switch to mode %d (now %d). use -f to set the console anyway\n", mode, crtmod(-1));
      return 1;
    }
  } else {
    mode = cur >= 0 && cur <= 19 ? cur : 16;     /* -k だけ: 画面モードは変えない */
  }
  w = size_tab[mode][0]; h = size_tab[mode][1];
  rows = h / 16;
  if (!given) {
    /* -k だけ: _B_CONSOL はカーソルを左上に戻すので、いまの位置を覚えておいて戻す。
       ファンクションキー行を出すとき、最下行にカーソルがあれば、範囲を狭める前に 1 行スクロールして最下行を空ける */
    int s = sup_enter();
    { volatile unsigned short *wa = (volatile unsigned short *)(long)0x974; ccol = wa[0]; crow = wa[1]; }
    sup_leave(s);
    if (fnk != 2 && crow >= rows - 1) { _iocs_b_putc(0x0D); _iocs_b_putc(0x0A); ccol = 0; crow = rows - 2; }
  }
  _dos_c_fnkmod(fnk);
  _iocs_b_consol(0, 0, w / 8 - 1, rows - 1 - (fnk == 2 ? 0 : 1));   /* 終わりの桁・行 */
  if (fnk == 2) clear_fnk_row(rows, w / 8);                        /* 480 行以下では写してあった行も消す */
  if (!given) {
    /* -k だけ: 画面は消さない。カーソルを元の位置 (範囲の外なら最下行) に戻す */
    if (fnk != 2 && rows < 32) copy_fnk_row(rows, w / 8);
    if (crow > rows - 1 - (fnk == 2 ? 0 : 1)) crow = rows - 1 - (fnk == 2 ? 0 : 1);
    _iocs_b_locate(ccol, crow);
    return 0;
  }
  _iocs_b_clr_al();
  if (fnk != 2 && rows < 32) copy_fnk_row(rows, w / 8);
  _iocs_b_locate(0, 0);
  _iocs_b_curon();
  return 0;
}
