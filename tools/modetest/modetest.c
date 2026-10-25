/*
 * modetest.x -- X68000 の画面モード (IOCS _CRTMOD) を順に表示してカラーバーを描く。
 *
 *   modetest [-x] [-l] [モード番号]
 *     -x  _CRTMOD の拡張モード (20 以降) も回る。IPLROM 1.6 (XEiJ の Hybrid など) か crtmod16.x が要る。
 *         最大の番号は _CRTMOD に D1.W=$76FF を渡して調べる (結果の上位 8 ビットが $16 のときだけ使う)。
 *     -l  モードの一覧を標準出力に出して終わる (画面は触らない)
 *     モード番号  そのモードから始める
 *
 *   キー: SPACE / Enter / → / ↓ / その他  次のモード
 *         BS / ← / ↑ / P                  前のモード
 *         ESC / Q                          終了 (元のモード、元のテキスト画面に戻す)
 *
 * 表示する周波数・実画面の大きさは XEiJ 付属の iplrom16.htm の「画面モードの一覧」による。
 * GVRAM は 1 画素 1 ワード (stride = 実画面の幅 * 2 バイト) で書く。16 色・256 色ではパレット番号、
 * 65536 色では GRBI (G=15-11, R=10-6, B=5-1, I=0) をそのまま書く。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __IOCS_INLINE__
#include <x68k/iocs.h>
#define __DOS_INLINE__
#include <x68k/dos.h>

typedef unsigned short u16;
typedef unsigned char u8;

#define GVRAM ((volatile u8 *)0xC00000UL)
#define TVRAM ((volatile u8 *)0xE00000UL)

/* 画面モードの一覧 (1 グループ = 同じ大きさ・同じ周波数の 6 つのモード) */
struct group {
  const char *name;
  short w, h;            /* 見える大きさ */
  int hf;                /* 水平同期 Hz */
  int vf;                /* 垂直同期 mHz */
  int interlace;
  signed char mode[6];   /* 16 色 4 面 / 256 色 2 面 / 65536 色 / 16 色 1024 / 256 色 1024 / 65536 色 1024 */
};

static const struct group groups[] = {
  { "256x256",       256, 256, 31500, 55458, 0, {  6, 10, 14,  2, 50, 63 } },
  { "256x256 sq",    256, 256, 31963, 56273, 0, { 37, 38, 39, 36, 58, 71 } },
  { "384x256",       384, 256, 31963, 56273, 0, { 29, 30, 31, 28, 56, 69 } },
  { "512x256",       512, 256, 31500, 55458, 0, { 41, 42, 43, 40, 59, 72 } },
  { "512x256 SP",    512, 256, 31500, 55458, 0, { 45, 46, 47, 44, 60, 73 } },
  { "512x512",       512, 512, 31500, 55458, 0, {  4,  8, 12,  0, 48, 61 } },
  { "512x512 sq",    512, 512, 31500, 55458, 0, { 33, 34, 35, 32, 57, 70 } },
  { "768x512",       768, 512, 31500, 55458, 0, { -1, 20, 24, 16, 52, 65 } },
  { "640x480",       640, 480, 31500, 55458, 0, { -1, 23, 27, 19, 55, 68 } },
  { "256x240",       256, 240, 15980, 61463, 0, {  7, 11, 15,  3, 51, 64 } },
  { "512x480",       512, 480, 15980, 61463, 1, {  5,  9, 13,  1, 49, 62 } },
  { "1024x424",     1024, 424, 24699, 53116, 0, { -1, 21, 25, 17, 53, 66 } },
  { "1024x848",     1024, 848, 24699, 53116, 1, { -1, 22, 26, 18, 54, 67 } },
};
#define NGROUPS (int)(sizeof groups / sizeof groups[0])
#define MAXMODE 73

/* t: 0=16色4面 1=256色2面 2=65536色 3=16色(1024) 4=256色(1024) 5=65536色(1024) */
static const u16 t_colors[6] = { 16, 256, 0, 16, 256, 0 };   /* 0 は 65536 */
static const u8  t_pages[6]  = {  4,   2, 1,  1,   1, 1 };
static const u16 t_real[6]   = { 512, 512, 512, 1024, 1024, 1024 };

struct minfo { const struct group *g; int t; };
#define VMODE0 100          /* 100-103: モード 16 の縦の表示期間を変えた試験用 (R04-R07 を直接書く) */
#define VMODE1 103
static struct minfo info[VMODE1 + 1];

static void build_table(void)
{
  int i, t;
  memset(info, 0, sizeof info);
  for (i = 0; i < NGROUPS; i++)
    for (t = 0; t < 6; t++) {
      int m = groups[i].mode[t];
      if (m >= 0) { info[m].g = &groups[i]; info[m].t = t; }
    }
  for (t = VMODE0; t <= VMODE1; t++) info[t] = info[16];
}

static void list_modes(void)
{
  int m;
  printf("mode  size       colors pages  real     H kHz    V Hz\n");
  for (m = 0; m <= MAXMODE; m++) {
    const struct group *g = info[m].g;
    int t = info[m].t;
    if (!g) continue;
    printf("%3d%s %-10s %6u %5d  %4dx%-4d %2d.%03d  %2d.%03d%s\n", m, m > 27 ? "*" : " ", g->name,
           t_colors[t] ? t_colors[t] : 65536, t_pages[t], t_real[t], t_real[t],
           g->hf / 1000, g->hf % 1000, g->vf / 1000, g->vf % 1000, g->interlace ? " interlace" : "");
  }
}

/* ---- スーパーバイザ (VRAM は特権空間) ---- */
static int sup_enter(void)
{
  unsigned short sr;
  __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
  return (sr & 0x2000) ? -1 : _dos_super(0);
}
static void sup_leave(int s) { if (s >= 0) _dos_super(s); }

/* ---- 色 ---- */
static u16 grbi(int r, int g, int b, int i) { return (u16)((g << 11) | (r << 6) | (b << 1) | i); }
static u16 gray64(int v) { return grbi(v >> 1, v >> 1, v >> 1, v & 1); }   /* v = 0..63 */

/* SMPTE 風の 7 色: 白 黄 シアン 緑 マゼンタ 赤 青。パレット 1..7。0 は黒 */
static const u8 barrgb[8][3] = {
  { 0, 0, 0 }, { 31, 31, 31 }, { 31, 31, 0 }, { 0, 31, 31 }, { 0, 31, 0 }, { 31, 0, 31 }, { 31, 0, 0 }, { 0, 0, 31 }
};
static u16 barcol(int i) { return grbi(barrgb[i][0], barrgb[i][1], barrgb[i][2], i == 1 ? 1 : 0); }

/* 16 色モードの下段 16 本のパレット (0..7 = バー、8..15 = 灰色 8 段) は gray8 で作る */
static u16 gray8(int i) { return gray64(i * 63 / 7); }

/* 画面の種類 K: 0=16色 1=256色 2=65536色。row builder がバー 1 本分の画素 (ワード) を返す */
#define PAL_GRAY64 64     /* 256 色モードの灰色 64 段 (パレット 64..127) */
#define PAL_R 128         /* R 32 段 (128..159) */
#define PAL_G 160
#define PAL_B 192

/* グラフィックパレット 0xE82000 (1 色 1 ワード、GRBI)。_GPALET は 256 色の拡張モード (52 など) で
   16 番以降を受け付けないことがあるので、スーパーバイザで直接書く。 */
#define GPAL ((volatile u16 *)0xE82000UL)

static void set_palette(int K)
{
  int i, s;
  if (K == 2) return;
  s = sup_enter();
  for (i = 0; i < 8; i++) GPAL[i] = barcol(i);
  if (K == 0) {
    for (i = 8; i < 16; i++) GPAL[i] = gray8(i - 8);
  } else {
    for (i = 0; i < 64; i++) GPAL[PAL_GRAY64 + i] = gray64(i);
    for (i = 0; i < 32; i++) {
      GPAL[PAL_R + i] = grbi(i, 0, 0, 0);
      GPAL[PAL_G + i] = grbi(0, i, 0, 0);
      GPAL[PAL_B + i] = grbi(0, 0, i, 0);
    }
  }
  sup_leave(s);
}

/* バー i (0=黒, 1..7) の画素 */
static u16 pix_bar(int K, int i) { return K == 2 ? barcol(i) : (u16)i; }
/* 灰色 n 段中の s 段目 */
static u16 pix_gray(int K, int s, int n)
{
  if (K == 0) return (u16)(8 + s);
  if (K == 1) return (u16)(PAL_GRAY64 + s);
  (void)n;
  return gray64(s);
}
static u16 pix_chan(int K, int c, int s)   /* c: 0=R 1=G 2=B、s = 0..31 */
{
  if (K == 1) return (u16)((c == 0 ? PAL_R : c == 1 ? PAL_G : PAL_B) + s);
  return grbi(c == 0 ? s : 0, c == 1 ? s : 0, c == 2 ? s : 0, 0);
}

static const u8 cast[7] = { 7, 0, 5, 0, 3, 0, 1 };   /* 下のバー: 青 黒 マゼンタ 黒 シアン 黒 白 */
static const u8 bars[7] = { 1, 2, 3, 4, 5, 6, 7 };

static u16 rowbuf[1024];

/* 1 行ぶん (幅 w) を rowbuf に作る。band: 0=バー 1=下のバー 2=灰色 3=最下段のうち sub 番目 */
static void make_row(int K, int w, int band, int sub)
{
  int x;
  for (x = 0; x < w; x++) {
    u16 v;
    switch (band) {
    case 0: v = pix_bar(K, bars[x * 7 / w]); break;
    case 1: v = pix_bar(K, cast[x * 7 / w]); break;
    case 2: { int n = K == 0 ? 8 : 64; v = pix_gray(K, x * n / w, n); break; }
    default:
      if (K == 0) v = (u16)(x * 16 / w);               /* 16 色全部 */
      else v = pix_chan(K, sub, x * 32 / w);
    }
    rowbuf[x] = v;
  }
}

static void fill_rows(int stride, int y0, int y1, int w)
{
  int y;
  for (y = y0; y < y1; y++) memcpy((void *)(GVRAM + (long)y * stride), rowbuf, (size_t)w * 2);
}

static void draw_bars(int K, int stride, int w, int h)
{
  int y1 = h * 60 / 100, y2 = h * 70 / 100, y3 = h * 85 / 100, y, s;
  int sub;
  make_row(K, w, 0, 0); fill_rows(stride, 0, y1, w);
  make_row(K, w, 1, 0); fill_rows(stride, y1, y2, w);
  make_row(K, w, 2, 0); fill_rows(stride, y2, y3, w);
  if (K == 0) { make_row(K, w, 3, 0); fill_rows(stride, y3, h, w); }
  else
    for (sub = 0; sub < 3; sub++) {
      y = y3 + (h - y3) * sub / 3; s = y3 + (h - y3) * (sub + 1) / 3;
      make_row(K, w, 3, sub); fill_rows(stride, y, s, w);
    }
}

/* ---- 枠・目盛り・数字 (画面の端が切れていないかを見る) ---- */
static int frame_only;   /* F キーで切り替え: 枠だけ (カラーバー無し) */
static int g_stride;

static const u8 font35[15][5] = {   /* 3x5 の字: 0-9, T L R B (各行 3 ビット) */
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
  {7,2,2,2,2}, {4,4,4,4,7}, {6,5,6,5,5}, {6,5,6,5,6}, {0,0,0,0,0}
};

static void gput(int x, int y, int w, int h, u16 v)
{
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  *(volatile u16 *)(GVRAM + (long)y * g_stride + (long)x * 2) = v;
}

static void grect(int x0, int y0, int x1, int y1, int w, int h, u16 v)   /* x1, y1 を含む */
{
  int x, y;
  for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) gput(x, y, w, h, v);
}

static void glyph(int idx, int x, int y, int sc, int w, int h, u16 fg)
{
  int r, c;
  for (r = 0; r < 5; r++)
    for (c = 0; c < 3; c++)
      if (font35[idx][r] & (4 >> c)) grect(x + c * sc, y + r * sc, x + c * sc + sc - 1, y + r * sc + sc - 1, w, h, fg);
}

/* 文字列 (数字と T L R B) を黒い下地つきで描く。(x, y) は左上 */
static void gtext(const char *t, int x, int y, int sc, int w, int h, u16 fg, u16 bg)
{
  int n = (int)strlen(t), i;
  grect(x - 1, y - 1, x + n * 4 * sc - sc, y + 5 * sc, w, h, bg);
  for (i = 0; i < n; i++) {
    char c = t[i];
    int idx = c >= '0' && c <= '9' ? c - '0' : c == 'T' ? 10 : c == 'L' ? 11 : c == 'R' ? 12 : c == 'B' ? 13 : 14;
    glyph(idx, x + i * 4 * sc, y, sc, w, h, fg);
  }
}

static void draw_overlay(int K, int w, int h)
{
  u16 fw = pix_bar(K, 1), bk = pix_bar(K, 0);
  int k, d, len, x, y;
  char num[8];
  /* 外枠 1 画素 */
  grect(0, 0, w - 1, 0, w, h, fw); grect(0, h - 1, w - 1, h - 1, w, h, fw);
  grect(0, 0, 0, h - 1, w, h, fw); grect(w - 1, 0, w - 1, h - 1, w, h, fw);
  /* 目盛り: 端から 8 画素ごと。短い線 6 画素、16 画素ごとは 12 画素。数字は 32 画素ごと */
  for (k = 1; 8 * k < w / 2 + 8; k++) {
    d = 8 * k; len = (k & 1) ? 6 : 12;
    for (y = 0; y < len; y++) {
      gput(d, y, w, h, fw); gput(w - 1 - d, y, w, h, fw);                 /* 上の端 */
      gput(d, h - 1 - y, w, h, fw); gput(w - 1 - d, h - 1 - y, w, h, fw); /* 下の端 */
    }
  }
  for (k = 1; 8 * k < h / 2 + 8; k++) {
    d = 8 * k; len = (k & 1) ? 6 : 12;
    for (x = 0; x < len; x++) {
      gput(x, d, w, h, fw); gput(x, h - 1 - d, w, h, fw);                 /* 左の端 */
      gput(w - 1 - x, d, w, h, fw); gput(w - 1 - x, h - 1 - d, w, h, fw); /* 右の端 */
    }
  }
  for (k = 4; k <= 12; k += 4) {          /* 32, 64, 96 */
    int sc = 2, nw;
    d = 8 * k;
    sprintf(num, "%d", d); nw = (int)strlen(num) * 4 * sc - sc;
    if (d < w / 2 - nw) {
      gtext(num, d - nw / 2, 14, sc, w, h, fw, bk);                       /* 上: 左端からの距離 */
      gtext(num, w - 1 - d - nw / 2, 14, sc, w, h, fw, bk);               /* 上: 右端からの距離 */
      gtext(num, d - nw / 2, h - 25, sc, w, h, fw, bk);
      gtext(num, w - 1 - d - nw / 2, h - 25, sc, w, h, fw, bk);
    }
    if (d < h / 2 - 8) {
      gtext(num, 16, d - 4, sc, w, h, fw, bk);                            /* 左: 上端からの距離 */
      gtext(num, 16, h - 1 - d - 4, sc, w, h, fw, bk);                    /* 左: 下端からの距離 */
      gtext(num, w - 17 - nw, d - 4, sc, w, h, fw, bk);
      gtext(num, w - 17 - nw, h - 1 - d - 4, sc, w, h, fw, bk);
    }
  }
  /* 四隅の印 */
  gtext("TL", 44, 44, 3, w, h, fw, bk);
  gtext("TR", w - 44 - 23, 44, 3, w, h, fw, bk);
  gtext("BL", 44, h - 44 - 15, 3, w, h, fw, bk);
  gtext("BR", w - 44 - 23, h - 44 - 15, 3, w, h, fw, bk);
}

/* ---- テキストのラベル ---- */
#define MAXLINES 8
static char lines[MAXLINES][40];
static int nlines;

static void make_label(int mode, int lo, int hi)
{
  const struct group *g = info[mode].g;
  int t = info[mode].t, real = t_real[t];
  int cols = t_colors[t] ? t_colors[t] : 65536;
  nlines = 0;
  sprintf(lines[nlines++], "MODE %d  (%d-%d)", mode, lo, hi);
  sprintf(lines[nlines++], "%dx%d%s", g->w, g->h, strchr(g->name, ' ') ? strchr(g->name, ' ') : "");
  sprintf(lines[nlines++], "%d colors, %d page%s", cols, t_pages[t], t_pages[t] > 1 ? "s" : "");
  sprintf(lines[nlines++], "H %d.%03d kHz", g->hf / 1000, g->hf % 1000);
  sprintf(lines[nlines++], "V %d.%03d Hz%s", g->vf / 1000, g->vf % 1000, g->interlace ? " (interlace)" : "");
  sprintf(lines[nlines++], "GVRAM %dx%d", real, real);
  strcpy(lines[nlines++], "SPACE:next BS:back");
  strcpy(lines[nlines++], "F:frame ESC:quit");
}

/* ラベルを出して背景 (コード 1 = 黒) を敷く。文字はコード 3 (白) */
static int lab_col, lab_row;

static void draw_label(void)
{
  int i, width = 0, p, y, x, s;
  char buf[64];
  for (i = 0; i < nlines; i++) { int l = (int)strlen(lines[i]); if (l > width) width = l; }
  _iocs_b_curoff();
  _iocs_b_color(3);
  for (i = 0; i < nlines; i++) {
    _iocs_b_locate(lab_col, lab_row + i);
    sprintf(buf, "%-*.40s", width, lines[i]);
    _iocs_b_print(buf);
  }
  _iocs_b_curoff();
  /* 文字のない画素をコード 1 にする (プレーン 0 を全部 1 にする。文字の画素はもとからコード 3) */
  s = sup_enter();
  for (y = 0; y < nlines * 16; y++)
    for (x = 0; x < width; x++) TVRAM[(long)(lab_row * 16 + y) * 128 + lab_col + x] = 0xFF;
  sup_leave(s);
  (void)p;
}

static void set_text_palette(void)
{
  _iocs_tpalet(1, 0x0001);   /* ほぼ黒 (0 は透明に扱われることがある) */
  _iocs_tpalet(2, 0xFFFE);
  _iocs_tpalet(3, 0xFFFE);   /* 白 */
}

/* _CRTMOD (特に XEiJ の拡張 ROM) は d0/d1 以外のレジスタも壊すことがあるので、全部壊れるものとして呼ぶ。
   これをしないと、モード切り替えのあとで変数が化けてバスエラーになる */
static int crtmod(int mode)
{
  register int d1 __asm__("d1") = mode;
  register int d0 __asm__("d0") = 0x10;
  __asm__ __volatile__ ("trap #15"
                        : "+d"(d0), "+d"(d1)
                        :
                        : "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "cc", "memory");
  return d0;
}

/* _CRTMOD で mode にする。使えないモードは負の値 (-2) が返り、画面モードは変わらない
   (XEiJ + IPLROM 1.6 では 48 以降がそうなる)。そのまま描くと、いまのモードの GVRAM に
   別のモードの並びで書くことになるので、描かずに「使えない」と表示する。 */
static int last_ret;
static int set_mode(int mode)
{
  last_ret = crtmod(mode);
  return last_ret >= 0;
}

/* モードにできなかったときは、いまのモードのテキスト画面にそう書く (バーは描かない) */
static void show_failed(int mode, int lo, int hi)
{
  _iocs_g_clr_on();
  set_text_palette();
  _iocs_b_clr_al();
  make_label(mode, lo, hi);
  sprintf(lines[5], "CRTMOD failed (%d)", last_ret);   /* GVRAM の行を置き換える */
  lab_col = lab_row = 0;
  draw_label();
}

static void center_label(int w, int h)
{
  int i, width = 0, cols = w / 8, rows = h / 16;
  for (i = 0; i < nlines; i++) { int l = (int)strlen(lines[i]); if (l > width) width = l; }
  lab_col = cols > width ? (cols - width) / 2 : 0;
  lab_row = rows > nlines ? (rows - nlines) / 2 : 0;
}

/* ---- 縦の表示期間を変える (モード 16 を元にした 100-103) ----
   CRTC R04 = 総ライン数 - 1、R05 = 垂直同期の終わり、R06 = 表示の開始、R07 = 表示の終わり。
   100/101/102 は表示を 480/496/464 行にし、R06 をずらして画面の中央に寄せる (R04, R05 は元のまま)。
   103 は 31kHz の VGA に近づけて総ライン 525、同期 2 行、表示 480 行にする。 */
#define CRTC ((volatile u16 *)0xE80000UL)
static const int var_lines[4] = { 480, 496, 464, 480 };
static int cur_r[4];      /* R04-R07 の現在値 */
static int adjusting, assumed;

static void crtc_read(void)
{
  int i, s = sup_enter();
  for (i = 0; i < 4; i++) cur_r[i] = CRTC[4 + i];
  sup_leave(s);
  /* CRTC のレジスタは読めない機種・エミュレータがある (0 が返る)。そのときはモード 16 の標準値にする */
  if (cur_r[0] == 0 || cur_r[3] <= cur_r[2]) { cur_r[0] = 567; cur_r[1] = 5; cur_r[2] = 40; cur_r[3] = 552; assumed = 1; }
  else assumed = 0;
}

static void crtc_write(void)
{
  int i, s = sup_enter();
  for (i = 0; i < 4; i++) CRTC[4 + i] = (u16)cur_r[i];
  sup_leave(s);
}

static void paint_variant(int mode, int lo, int hi)
{
  int w = 768, h = cur_r[3] - cur_r[2], s;
  if (h < 8) h = 8;
  if (h > 512) h = 512;
  _iocs_g_clr_on();
  set_palette(0);
  s = sup_enter();
  g_stride = 2048;
  if (!frame_only) draw_bars(0, 2048, w, h);
  draw_overlay(0, w, h);
  sup_leave(s);
  set_text_palette();
  _iocs_b_clr_al();
  nlines = 0;
  if (adjusting) sprintf(lines[nlines++], "ADJUST %d  R04-R07", mode);
  else sprintf(lines[nlines++], "MODE 16 V%d  (%d)", var_lines[mode - VMODE0], mode);
  sprintf(lines[nlines++], "768x%d 16 colors", h);
  sprintf(lines[nlines++], "R04=%d R05=%d%s", cur_r[0], cur_r[1], assumed ? " (assumed)" : "");
  sprintf(lines[nlines++], "R06=%d R07=%d", cur_r[2], cur_r[3]);
  sprintf(lines[nlines++], "total %d lines, show %d", cur_r[0] + 1, cur_r[3] - cur_r[2]);
  if (adjusting) {
    strcpy(lines[nlines++], "<- -> move  up/down height");
    strcpy(lines[nlines++], "A:end R:reset F:frame");
  } else {
    strcpy(lines[nlines++], "A:adjust SPACE:next BS:back");
    strcpy(lines[nlines++], "F:frame ESC:quit");
  }
  center_label(w, h);
  draw_label();
}

static void show_variant(int mode, int lo, int hi, int reset)
{
  if (reset) {
    int l = var_lines[mode - VMODE0];
    adjusting = 0;
    if (!set_mode(16)) { show_failed(mode, lo, hi); return; }
    crtc_read();
    if (mode == VMODE1) { cur_r[0] = 524; cur_r[1] = 1; cur_r[2] = 34; cur_r[3] = 34 + l; }
    else { cur_r[2] += (512 - l) / 2; cur_r[3] = cur_r[2] + l; }
    crtc_write();
  }
  paint_variant(mode, lo, hi);
}

static int next_mode(int m, int lo, int hi)
{
  do {
    if (m == VMODE1) m = lo;
    else if (m == hi) m = VMODE0;
    else m++;
  } while (!info[m].g);
  return m;
}

static int prev_mode(int m, int lo, int hi)
{
  do {
    if (m == lo) m = VMODE1;
    else if (m == VMODE0) m = hi;
    else m--;
  } while (!info[m].g);
  return m;
}

static void show_mode(int mode, int lo, int hi)
{
  const struct group *g = info[mode].g;
  int t = info[mode].t, real = t_real[t];
  int K = t_colors[t] == 16 ? 0 : t_colors[t] == 256 ? 1 : 2;
  int w = g->w < real ? g->w : real, h = g->h < real ? g->h : real, s;
  if (!set_mode(mode)) { show_failed(mode, lo, hi); return; }
  _iocs_g_clr_on();
  set_palette(K);
  s = sup_enter();
  g_stride = real * 2;
  if (!frame_only) draw_bars(K, real * 2, w, h);
  draw_overlay(K, w, h);
  sup_leave(s);
  set_text_palette();
  _iocs_b_clr_al();
  make_label(mode, lo, hi);
  center_label(w, h);
  draw_label();
}

/* ---- 元の画面を取っておく ---- */
#define SAVE_BYTES 96            /* 768 ドット */
#define SAVE_LINES 512
static u8 *saved;
static int saved_ok;
static u16 saved_pal[4];
static int saved_fnk;

static int saved_cur;

/* 現在のカーソル位置。IOCS の作業領域 ($974 = 桁、$976 = 行) を読む。_B_LOCATE の問い合わせは
   機種によって返り値が違うことがあったので、これを優先する */
static int cursor_pos(void)
{
  int s = sup_enter(), c, r;
  { volatile u16 *wa = (volatile u16 *)(long)0x974; c = wa[0]; r = wa[1]; }
  sup_leave(s);
  if (c < 200 && r < 100) return (c << 16) | r;
  return _iocs_b_locate(-1, -1);
}

static void save_text(void)
{
  int p, y, s;
  saved = malloc((size_t)4 * SAVE_LINES * SAVE_BYTES);
  saved_cur = cursor_pos();
  saved_fnk = _dos_c_fnkmod(-1);   /* ファンクションキー行の表示モード */
  for (p = 0; p < 4; p++) saved_pal[p] = (u16)_iocs_tpalet(p, -1);
  if (!saved) return;
  s = sup_enter();
  for (p = 0; p < 4; p++)
    for (y = 0; y < SAVE_LINES; y++)
      memcpy(saved + ((long)p * SAVE_LINES + y) * SAVE_BYTES, (const void *)(TVRAM + (long)p * 0x20000 + (long)y * 128), SAVE_BYTES);
  sup_leave(s);
  saved_ok = 1;
}

static void restore_text(int orig)
{
  int p, y, s;
  set_text_palette();
  for (p = 1; p < 4; p++) _iocs_tpalet(p, saved_pal[p]);
  if (saved_ok) {
    s = sup_enter();
    for (p = 0; p < 4; p++)
      for (y = 0; y < SAVE_LINES; y++)
        memcpy((void *)(TVRAM + (long)p * 0x20000 + (long)y * 128), saved + ((long)p * SAVE_LINES + y) * SAVE_BYTES, SAVE_BYTES);
    sup_leave(s);
  }
  /* _CRTMOD はコンソールを全画面に戻す。ファンクションキー行の表示モードを設定し直すと、
     その行の再描画とスクロール範囲 (最下行を除く) の設定もされる */
  {
    int fm = saved_fnk >= 0 && saved_fnk <= 3 ? saved_fnk : 0;
    const struct group *g = orig >= 0 && orig <= MAXMODE ? info[orig].g : 0;
    if (!g) g = info[16].g;
    /* コンソール範囲は、元のモードの桁数と行数 (ファンクションキー行を出すときは最下行を除く) にする。
       起動時に読んだ値は、前に動かしたものが残っていると狂っているので使わない */
    _dos_c_fnkmod(fm);
    _iocs_b_consol(0, 0, g->w / 8 - 1, g->h / 16 - 1 - (fm == 2 ? 0 : 1));   /* 終わりの桁・行 */
  }
  {
    /* 最初に 0,0 だった (前に動かしたものが残っていた) ときは、最下行 (ファンクションキー行の 1 行上) に置く。
       画面を 0 行目から使い直すと、残っている文字に重なる */
    const struct group *g = orig >= 0 && orig <= MAXMODE ? info[orig].g : 0;
    int rows = (g ? g->h : 512) / 16 - (saved_fnk == 2 ? 0 : 1);
    if (saved_cur == 0) _iocs_b_locate(0, rows - 1);
    else _iocs_b_locate((saved_cur >> 16) & 0xFFFF, saved_cur & 0xFFFF);
  }
  _iocs_b_curon();
}

int main(int argc, char **argv)
{
  int orig, lo = 0, hi = 19, mode = 0, i, ext = 0, key = 0, orig_color, redraw = 0;
  build_table();
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-l")) { list_modes(); return 0; }
    else if (!strcmp(argv[i], "-c")) { int p = cursor_pos(), q = _iocs_b_locate(-1, -1); printf("work area col %d row %d, B_LOCATE col %d row %d\n", p >> 16, p & 0xFFFF, (q >> 16) & 0xFFFF, q & 0xFFFF); return 0; }
    else if (!strcmp(argv[i], "-x")) ext = 1;
    else if (argv[i][0] >= '0' && argv[i][0] <= '9') mode = atoi(argv[i]);
    else { fprintf(stderr, "usage: modetest [-x] [-l] [mode]\n"); return 1; }
  }
  if (ext) {
    int r = crtmod(0x76FF);
    if (((unsigned)r >> 24) == 0x16 && (r & 0xFFFFFF) >= 19 && (r & 0xFFFFFF) <= MAXMODE) hi = r & 0xFFFFFF;
    else fprintf(stderr, "modetest: IOCS has no mode-count query (returned %08x), using 0-19\n", r);
  }
  if (mode < lo || (mode > hi && (mode < VMODE0 || mode > VMODE1))) mode = lo;
  orig = crtmod(-1);
  orig_color = _iocs_b_color(-1);
  save_text();
  while (_iocs_b_keysns()) _iocs_b_keyinp();
  for (;;) {
    while (!info[mode].g) mode = next_mode(mode, lo, hi);
    if (redraw == 1) show_variant(mode, lo, hi, 0);
    else if (mode >= VMODE0) show_variant(mode, lo, hi, 1);
    else show_mode(mode, lo, hi);
    redraw = 0;
    key = _iocs_b_keyinp();
    {
      int ch = key & 0xFF, sc = (key >> 8) & 0xFF;
      if (ch == 0x1B || ch == 'q' || ch == 'Q') break;
      if (ch == 'f' || ch == 'F') { frame_only ^= 1; redraw = mode >= VMODE0 ? 1 : 2; continue; }   /* 同じモードを描き直す */
      if (mode >= VMODE0 && (ch == 'a' || ch == 'A')) { adjusting ^= 1; redraw = 1; continue; }
      if (adjusting) {
        if (ch == 'r' || ch == 'R') { adjusting = 0; continue; }   /* 同じモードを最初から */
        if (sc == 0x3B) { cur_r[2]--; cur_r[3]--; }          /* ← 表示を 1 行上へ */
        else if (sc == 0x3D) { cur_r[2]++; cur_r[3]++; }     /* → 1 行下へ */
        else if (sc == 0x3C) cur_r[3]++;                     /* ↑ 縦を 1 行長く */
        else if (sc == 0x3E) cur_r[3]--;                     /* ↓ 1 行短く */
        else continue;
        crtc_write();
        redraw = 1;
        continue;
      }
      if (ch == 0x08 || ch == 'p' || ch == 'P' || sc == 0x3C) mode = prev_mode(mode, lo, hi);   /* BS ← ↑ */
      else mode = next_mode(mode, lo, hi);
    }
  }
  crtmod(orig < 0 ? 16 : orig);
  restore_text(orig);
  if (orig_color >= 0) _iocs_b_color(orig_color);
  return 0;
}
