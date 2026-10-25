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
static struct minfo info[MAXMODE + 1];

static void build_table(void)
{
  int i, t;
  memset(info, 0, sizeof info);
  for (i = 0; i < NGROUPS; i++)
    for (t = 0; t < 6; t++) {
      int m = groups[i].mode[t];
      if (m >= 0) { info[m].g = &groups[i]; info[m].t = t; }
    }
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
  strcpy(lines[nlines++], "SPACE:next BS:back ESC:quit");
}

/* ラベルを出して背景 (コード 1 = 黒) を敷く。文字はコード 3 (白) */
static void draw_label(void)
{
  int i, width = 0, p, y, x, s;
  char buf[64];
  for (i = 0; i < nlines; i++) { int l = (int)strlen(lines[i]); if (l > width) width = l; }
  _iocs_b_curoff();
  _iocs_b_color(3);
  for (i = 0; i < nlines; i++) {
    _iocs_b_locate(0, i);
    sprintf(buf, "%-*.40s", width, lines[i]);
    _iocs_b_print(buf);
  }
  _iocs_b_curoff();
  /* 文字のない画素をコード 1 にする (プレーン 0 を全部 1 にする。文字の画素はもとからコード 3) */
  s = sup_enter();
  for (y = 0; y < nlines * 16; y++)
    for (x = 0; x < width; x++) TVRAM[(long)y * 128 + x] = 0xFF;
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
  draw_label();
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
  draw_bars(K, real * 2, w, h);
  sup_leave(s);
  set_text_palette();
  _iocs_b_clr_al();
  make_label(mode, lo, hi);
  draw_label();
}

/* ---- 元の画面を取っておく ---- */
#define SAVE_BYTES 96            /* 768 ドット */
#define SAVE_LINES 512
static u8 *saved;
static int saved_ok;
static u16 saved_pal[4];
static int saved_cur;

static void save_text(void)
{
  int p, y, s;
  saved = malloc((size_t)4 * SAVE_LINES * SAVE_BYTES);
  saved_cur = _iocs_b_locate(-1, -1);
  for (p = 0; p < 4; p++) saved_pal[p] = (u16)_iocs_tpalet(p, -1);
  if (!saved) return;
  s = sup_enter();
  for (p = 0; p < 4; p++)
    for (y = 0; y < SAVE_LINES; y++)
      memcpy(saved + ((long)p * SAVE_LINES + y) * SAVE_BYTES, (const void *)(TVRAM + (long)p * 0x20000 + (long)y * 128), SAVE_BYTES);
  sup_leave(s);
  saved_ok = 1;
}

static void restore_text(void)
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
  _iocs_b_locate((saved_cur >> 16) & 0xFFFF, saved_cur & 0xFFFF);
  _iocs_b_curon();
}

int main(int argc, char **argv)
{
  int orig, lo = 0, hi = 19, mode = 0, i, ext = 0, key = 0, orig_color;
  build_table();
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-l")) { list_modes(); return 0; }
    else if (!strcmp(argv[i], "-x")) ext = 1;
    else if (argv[i][0] >= '0' && argv[i][0] <= '9') mode = atoi(argv[i]);
    else { fprintf(stderr, "usage: modetest [-x] [-l] [mode]\n"); return 1; }
  }
  if (ext) {
    int r = crtmod(0x76FF);
    if (((unsigned)r >> 24) == 0x16 && (r & 0xFFFFFF) >= 19 && (r & 0xFFFFFF) <= MAXMODE) hi = r & 0xFFFFFF;
    else fprintf(stderr, "modetest: IOCS has no mode-count query (returned %08x), using 0-19\n", r);
  }
  if (mode < lo || mode > hi) mode = lo;
  orig = crtmod(-1);
  orig_color = _iocs_b_color(-1);
  save_text();
  while (_iocs_b_keysns()) _iocs_b_keyinp();
  for (;;) {
    while (!info[mode].g) mode = mode < hi ? mode + 1 : lo;   /* 抜けのある番号は飛ばす */
    show_mode(mode, lo, hi);
    key = _iocs_b_keyinp();
    {
      int ch = key & 0xFF, sc = (key >> 8) & 0xFF;
      if (ch == 0x1B || ch == 'q' || ch == 'Q') break;
      if (ch == 0x08 || ch == 'p' || ch == 'P' || sc == 0x3C) {    /* BS ← ↑ */
        do mode = mode > lo ? mode - 1 : hi; while (!info[mode].g);
      } else mode = mode < hi ? mode + 1 : lo;
    }
  }
  crtmod(orig < 0 ? 16 : orig);
  restore_text();
  if (orig_color >= 0) _iocs_b_color(orig_color);
  return 0;
}
