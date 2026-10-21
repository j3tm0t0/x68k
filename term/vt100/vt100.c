/*
 * vt100.c -- X68000 テキスト画面に直接描く VT100 (xterm サブセット) エミュレータ本体。
 *
 * 画面の持ち方:
 *   logical  論理画面 (セル配列)。パーサが書き換える。main / alt の 2 面を切り替える。
 *   drawn    いま VRAM に描かれている内容の控え。flush で logical と比べ、違うセルだけ描き直す。
 *            (top のように同じ内容を何度も送ってくる相手でも、変わったセルだけ描けば速い)
 * スクロールは VRAM をラスタ単位でずらして (txrascpy か自前の 32bit 転送) drawn も同じようにずらす。
 * これで flush は新しく空いた行だけ描けばよくなる (全面描き直しを避ける)。
 *
 * X68000 のテキスト VRAM: プレーン p (0..3) が 0xE00000 + p*0x20000、128 バイト/ライン、1024 ライン。
 * セル (c,r) の p プレーン・ラスタ s のバイト = 0xE00000 + p*0x20000 + (r*16+s)*128 + c。
 * 4 プレーンの 4bit がテキストパレット (0xE82200、16 ワード) の番号になり、色が決まる。
 * フォントは CGROM の 8x16 ANK (0xF3A800、16 バイト/字、MSB 左、文字コード*16)。
 *
 * VRAM・パレット・CGROM はスーパーバイザ空間。触る関数の頭で _dos_super(0)、末尾で戻す。
 */
#ifdef VT_HOSTTEST
/* Mac 上でパーサ・スクロール・属性だけを pyte と比べるためのスタブ版。
 * VRAM/パレット/CGROM はホストメモリに逃がし、IOCS/DOS 呼び出しは無害化する。 */
static int _dos_super(int x) { (void)x; return 0; }
static void _dos_breakck(int x) { (void)x; }
static void _iocs_b_curon(void) {}
static void _iocs_b_curoff(void) {}
static int _iocs_b_keysns(void) { return 0; }
static int _iocs_b_keyinp(void) { return 0; }
unsigned char vt_host_vram[4 * 0x20000];
unsigned short vt_host_pal[16];
unsigned char vt_host_cg[0x1000];
#else
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#endif
#include "vt100.h"

/* ---- 画面の寸法 ---- */
#define MAXCOLS 96
#define MAXROWS 32
#define CW      8          /* セル幅 (ドット) */
#define CH      16         /* セル高 (ドット) */
#define VBYTES  128        /* 1 ライン 128 バイト (1 プレーン) */
#define PLANE   0x20000UL  /* プレーン間の距離 */

#ifdef VT_HOSTTEST
#define TVRAM  ((volatile unsigned char *)vt_host_vram)
#define TPAL   ((volatile unsigned short *)vt_host_pal)
#define CGROM16 ((const unsigned char *)vt_host_cg)
#else
#define TVRAM  ((volatile unsigned char *)0xE00000UL)
#define TPAL   ((volatile unsigned short *)0xE82200UL)
#define CGROM16 ((const unsigned char *)0xF3A800UL)   /* 8x16 ANK フォント */
#endif

/* ---- セル ---- */
/* ch: 描く文字。0x20-0x7f は ANK。flags の SPECIAL が立つと DEC 罫線グリフ表を引く。 */
typedef struct {
  unsigned char ch;
  unsigned char fg;     /* 0..15 パレット番号 (bold / reverse 解決済み) */
  unsigned char bg;     /* 0..15 */
  unsigned char flags;  /* bit0: 下線 */
} cell_t;
#define F_UNDERLINE 0x01
#define F_SPECIAL   0x02   /* ch を DEC 特殊グリフ表で描く */

static cell_t scr_main[MAXROWS][MAXCOLS];
static cell_t scr_alt[MAXROWS][MAXCOLS];
static cell_t drawn[MAXROWS][MAXCOLS];
static cell_t (*cur)[MAXCOLS] = scr_main;   /* いま表示している面 */

static int COLS = MAXCOLS, ROWS = MAXROWS;

/* ---- カーソル・属性の状態 ---- */
static int cx, cy;            /* カーソル位置 (0 起算) */
static int pend_wrap;         /* DECAWM の遅延折り返し待ち */
static int top, bot;          /* スクロール領域 (0 起算・両端含む) */
static int c_fg = 7, c_bg = 0;
static int a_bold, a_uline, a_reverse;
static int decckm;            /* アプリケーションカーソルキーモード */
static int decawm = 1;        /* 自動折り返し (既定 ON) */
static int cur_vis = 1;       /* カーソル表示 */
static int alt_active;        /* 代替画面を表示中か */
static int g_special[2];      /* G0/G1 が DEC 特殊グラフィックか */
static int gl;                /* いま GL に割り当てている G 番号 (0/1) */

/* 保存カーソル (DECSC/DECRC) */
static int sv_cx, sv_cy, sv_fg, sv_bg, sv_bold, sv_ul, sv_rev, sv_sp0, sv_sp1, sv_gl;

static unsigned char tabstop[MAXCOLS];

/* カーソルの描画状態 (ソフトカーソル) */
static int csr_shown, csr_x, csr_y;

/* ---- パーサ状態 ---- */
enum { S_GROUND, S_ESC, S_CSI, S_STR, S_ESC_INT, S_CHARSET };
static int state = S_GROUND;
static int csi_priv;                 /* '?' などの private 前置 */
#define MAXPARAM 16
static int param[MAXPARAM], nparam, param_seen;
static int str_kind;                 /* 文字列シーケンスの種類 (']' 'P' など) */
static int str_esc;                  /* 文字列中で ESC を見たか (ST 待ち) */
static int cs_which;                 /* 文字セット指定の対象 ')' か '(' か */

/* UTF-8 デコード */
static unsigned long u_cp;
static int u_need;

/* ホストへの応答キュー (DSR / DA) */
static unsigned char outq[64];
static int outq_len;

/* パレットの退避 */
static unsigned short pal_save[16];
static int pal_saved;

/* ---- スーパーバイザ ---- */
/* 戻り値を _dos_super に渡して戻す。負なら元からスーパーバイザなので触らない。 */
static int sup_enter(void) { return _dos_super(0); }
static void sup_leave(int s) { if (s >= 0) _dos_super(s); }

/* ================= DEC 特殊グラフィック (罫線) の 8x16 グリフ ================= */
/* VT100 の 0x60-0x7e。CGROM の罫線は 16 幅の漢字で使えないので自前で 8x16 を作る。
 * 横線はセル中央 (ラスタ 7,8)、縦線は中央列 (bit3 = 0x08)。使う記号だけ埋める。 */
static unsigned char sg_glyph[32][16];
static int sg_ready;

static void sg_hline(unsigned char *g, int y) { g[y] = 0xff; }
static void sg_vline(unsigned char *g) { int y; for (y = 0; y < 16; y++) g[y] = 0x08; }
static void sg_build(void)
{
  int i, y;
  unsigned char *g;
  for (i = 0; i < 32; i++) for (y = 0; y < 16; y++) sg_glyph[i][y] = 0;
  /* j=0x6a ┘  k=0x6b ┐  l=0x6c ┌  m=0x6d └  n=0x6e ┼  q=0x71 ─  t=0x74 ├  u=0x75 ┤
   * v=0x76 ┴  w=0x77 ┬  x=0x78 │  o=0x6f ⎺  s=0x73 ⎽  a=0x61 (市松) */
  g = sg_glyph[0x71 - 0x60]; sg_hline(g, 7); sg_hline(g, 8);                      /* ─ */
  g = sg_glyph[0x78 - 0x60]; sg_vline(g);                                         /* │ */
  g = sg_glyph[0x6e - 0x60]; sg_vline(g); sg_hline(g, 7); sg_hline(g, 8);         /* ┼ */
  /* 角・T 字: 中央から必要な向きだけ伸ばす */
  g = sg_glyph[0x6c - 0x60]; for (y = 7; y < 16; y++) g[y] = 0x08; g[7] |= 0xf8; g[8] |= 0xf8;   /* ┌ */
  g = sg_glyph[0x6b - 0x60]; for (y = 7; y < 16; y++) g[y] = 0x08; g[7] |= 0x0f; g[8] |= 0x0f;   /* ┐ */
  g = sg_glyph[0x6d - 0x60]; for (y = 0; y <= 8; y++) g[y] = 0x08; g[7] |= 0xf8; g[8] |= 0xf8;   /* └ */
  g = sg_glyph[0x6a - 0x60]; for (y = 0; y <= 8; y++) g[y] = 0x08; g[7] |= 0x0f; g[8] |= 0x0f;   /* ┘ */
  g = sg_glyph[0x74 - 0x60]; sg_vline(g); g[7] |= 0xf8; g[8] |= 0xf8;             /* ├ */
  g = sg_glyph[0x75 - 0x60]; sg_vline(g); g[7] |= 0x0f; g[8] |= 0x0f;             /* ┤ */
  g = sg_glyph[0x77 - 0x60]; for (y = 7; y < 16; y++) g[y] = 0x08; sg_hline(g, 7); sg_hline(g, 8); /* ┬ */
  g = sg_glyph[0x76 - 0x60]; for (y = 0; y <= 8; y++) g[y] = 0x08; sg_hline(g, 7); sg_hline(g, 8); /* ┴ */
  g = sg_glyph[0x6f - 0x60]; sg_hline(g, 0);                                      /* ⎺ 上 */
  g = sg_glyph[0x73 - 0x60]; sg_hline(g, 15);                                     /* ⎽ 下 */
  g = sg_glyph[0x61 - 0x60]; for (y = 0; y < 16; y++) g[y] = (y & 1) ? 0x55 : 0xaa; /* ▒ 市松 */
  g = sg_glyph[0x60 - 0x60]; for (y = 4; y < 12; y++) g[y] = 0x3c;                /* ◆ */
  sg_ready = 1;
}

/* ================= パレット ================= */
static unsigned short rgb(int r, int g, int b) /* r,g,b: 0..31 */
{
  return (unsigned short)((g << 11) | (r << 6) | (b << 1) | 1);
}
static void palette_setup(void)
{
  int i;
  static const unsigned char tbl[16][3] = {
    { 0, 0, 0}, {21, 0, 0}, { 0,21, 0}, {21,21, 0}, { 0, 0,21}, {21, 0,21}, { 0,21,21}, {21,21,21},
    {10,10,10}, {31, 0, 0}, { 0,31, 0}, {31,31, 0}, { 0, 0,31}, {31, 0,31}, { 0,31,31}, {31,31,31}
  };
  for (i = 0; i < 16; i++) pal_save[i] = TPAL[i];
  pal_saved = 1;
  for (i = 0; i < 16; i++) TPAL[i] = rgb(tbl[i][0], tbl[i][1], tbl[i][2]);
}

/* ================= グリフ描画 ================= */
static const unsigned char *glyph_of(const cell_t *c)
{
  if ((c->flags & F_SPECIAL) && c->ch >= 0x60 && c->ch <= 0x7f)
    return sg_glyph[c->ch - 0x60];
  if (c->ch >= 0x20 && c->ch <= 0x7f)
    return CGROM16 + (int)c->ch * 16;
  return CGROM16 + 0x20 * 16;   /* 空白 */
}

/* 1 セルを VRAM に描く (スーパーバイザ前提) */
static void blit(int col, int row, const cell_t *c)
{
  const unsigned char *gp = glyph_of(c);
  int fg = c->fg, bg = c->bg, s, p;
  unsigned char fgm[4], bgm[4];
  for (p = 0; p < 4; p++) {
    fgm[p] = (fg >> p) & 1 ? 0xff : 0x00;
    bgm[p] = (bg >> p) & 1 ? 0xff : 0x00;
  }
  for (s = 0; s < CH; s++) {
    unsigned char g = gp[s];
    if ((c->flags & F_UNDERLINE) && s == CH - 1) g = 0xff;
    {
      volatile unsigned char *v = TVRAM + (unsigned long)(row * CH + s) * VBYTES + col;
      v[0 * PLANE] = (unsigned char)((g & fgm[0]) | (~g & bgm[0]));
      v[1 * PLANE] = (unsigned char)((g & fgm[1]) | (~g & bgm[1]));
      v[2 * PLANE] = (unsigned char)((g & fgm[2]) | (~g & bgm[2]));
      v[3 * PLANE] = (unsigned char)((g & fgm[3]) | (~g & bgm[3]));
    }
  }
}

/* カーソルセルを反転する (XOR なので同じ呼び出しで消える) */
static void cursor_xor(int col, int row)
{
  int s;
  for (s = 0; s < CH; s++) {
    volatile unsigned char *v = TVRAM + (unsigned long)(row * CH + s) * VBYTES + col;
    v[0 * PLANE] ^= 0xff; v[1 * PLANE] ^= 0xff; v[2 * PLANE] ^= 0xff; v[3 * PLANE] ^= 0xff;
  }
}

/* ================= スクロール (VRAM をラスタ単位でずらす) ================= */
/* 32bit 単位でコピーする (src<dst のときは後ろから)。VRAM は 4 プレーン別々に。 */
static void vram_move_lines(int dst_line, int src_line, int nlines)
{
  int p;
  long bytes = (long)nlines * VBYTES;
  for (p = 0; p < 4; p++) {
    volatile unsigned char *base = TVRAM + (unsigned long)p * PLANE;
    volatile unsigned long *d = (volatile unsigned long *)(base + (long)dst_line * VBYTES);
    volatile unsigned long *s = (volatile unsigned long *)(base + (long)src_line * VBYTES);
    long n = bytes / 4, i;
    if ((unsigned long)d <= (unsigned long)s) {
      for (i = 0; i < n; i++) d[i] = s[i];
    } else {
      for (i = n - 1; i >= 0; i--) d[i] = s[i];
    }
  }
}

static void clear_cell(cell_t *c) { c->ch = 0x20; c->fg = c_fg; c->bg = c_bg; c->flags = 0; }

/* スクロール領域 [top,bot] を n 行上へ (正: 上スクロール / 負: 下スクロール) */
static void scroll(int n)
{
  int r, col, keep;
  if (n == 0) return;
  if (n > bot - top + 1) n = bot - top + 1;
  if (-n > bot - top + 1) n = -(bot - top + 1);
  keep = (bot - top + 1) - (n > 0 ? n : -n);
  if (n > 0) {
    /* 論理バッファと drawn を上へ */
    for (r = top; r <= bot - n; r++)
      for (col = 0; col < COLS; col++) { cur[r][col] = cur[r + n][col]; drawn[r][col] = drawn[r + n][col]; }
    /* VRAM を上へ */
    if (keep > 0) vram_move_lines(top * CH, (top + n) * CH, keep * CH);
    for (r = bot - n + 1; r <= bot; r++)
      for (col = 0; col < COLS; col++) { clear_cell(&cur[r][col]); drawn[r][col] = cur[r][col]; blit(col, r, &cur[r][col]); }
  } else {
    n = -n;
    for (r = bot; r >= top + n; r--)
      for (col = 0; col < COLS; col++) { cur[r][col] = cur[r - n][col]; drawn[r][col] = drawn[r - n][col]; }
    if (keep > 0) vram_move_lines((top + n) * CH, top * CH, keep * CH);
    for (r = top; r < top + n; r++)
      for (col = 0; col < COLS; col++) { clear_cell(&cur[r][col]); drawn[r][col] = cur[r][col]; blit(col, r, &cur[r][col]); }
  }
}

/* ================= flush: logical と drawn の差分を描く ================= */
static void flush(void)
{
  int r, c, s;
  s = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }   /* 先にカーソルを消す */
  for (r = 0; r < ROWS; r++)
    for (c = 0; c < COLS; c++) {
      cell_t *lc = &cur[r][c], *dc = &drawn[r][c];
      if (lc->ch != dc->ch || lc->fg != dc->fg || lc->bg != dc->bg || lc->flags != dc->flags) {
        blit(c, r, lc);
        *dc = *lc;
      }
    }
  if (cur_vis) { cursor_xor(cx, cy); csr_shown = 1; csr_x = cx; csr_y = cy; }
  sup_leave(s);
}

/* ================= 文字の書き込み ================= */
static void resolve_attr(cell_t *c)
{
  int fg = c_fg, bg = c_bg, t;
  if (a_bold && fg < 8) fg += 8;
  if (a_reverse) { t = fg; fg = bg; bg = t; }
  c->fg = fg; c->bg = bg;
  c->flags = a_uline ? F_UNDERLINE : 0;
}

static void put_glyph(unsigned char ch, int special)
{
  cell_t *c;
  if (pend_wrap) {        /* 前回末尾に置いた後の遅延折り返し */
    cx = 0;
    if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++;
    pend_wrap = 0;
  }
  c = &cur[cy][cx];
  c->ch = ch;
  resolve_attr(c);
  if (special) c->flags |= F_SPECIAL;
  if (cx == COLS - 1) {
    if (decawm) pend_wrap = 1;   /* 折り返しは次の文字まで遅延 */
  } else {
    cx++;
  }
}

/* ================= カーソル移動・消去 ================= */
static void clamp_cursor(void) { if (cx < 0) cx = 0; if (cx >= COLS) cx = COLS - 1; if (cy < 0) cy = 0; if (cy >= ROWS) cy = ROWS - 1; }

static void erase_cells(int r, int c0, int c1)
{
  int c;
  for (c = c0; c <= c1 && c < COLS; c++) clear_cell(&cur[r][c]);
}

/* ================= パラメータ ================= */
static int p_def(int i, int def) {
  if (i >= nparam) return def;
  return param[i] ? param[i] : def;
}

/* ================= SGR ================= */
static int map256(int n)
{
  if (n < 16) return n;
  if (n >= 232) { int g = (n - 232) / 3; return g < 4 ? (g == 0 ? 0 : 8) : 15; }
  { /* 6x6x6 キューブ → 8 色 + bright を近似 */
    int c = n - 16, r = c / 36, g = (c / 6) % 6, b = c % 6, idx = 0, bright;
    if (r > 2) idx |= 1;
    if (g > 2) idx |= 2;
    if (b > 2) idx |= 4;
    bright = (r > 3 || g > 3 || b > 3);
    return bright ? idx + 8 : idx;
  }
}
static void do_sgr(void)
{
  int i = 0;
  if (nparam == 0) { a_bold = a_uline = a_reverse = 0; c_fg = 7; c_bg = 0; return; }
  for (i = 0; i < nparam; i++) {
    int v = param[i];
    switch (v) {
      case 0: a_bold = a_uline = a_reverse = 0; c_fg = 7; c_bg = 0; break;
      case 1: a_bold = 1; break;
      case 4: a_uline = 1; break;
      case 7: a_reverse = 1; break;
      case 22: a_bold = 0; break;
      case 24: a_uline = 0; break;
      case 27: a_reverse = 0; break;
      case 39: c_fg = 7; break;
      case 49: c_bg = 0; break;
      case 38: case 48: {
        int to_fg = (v == 38), col = -1;
        if (i + 1 < nparam && param[i + 1] == 5) { col = map256(param[i + 2]); i += 2; }
        else if (i + 1 < nparam && param[i + 1] == 2) {
          int r = param[i + 2], g = param[i + 3], b = param[i + 4], idx = 0;
          if (r > 127) idx |= 1;
          if (g > 127) idx |= 2;
          if (b > 127) idx |= 4;
          col = (r > 200 || g > 200 || b > 200) ? idx + 8 : idx; i += 4;
        }
        if (col >= 0) { if (to_fg) c_fg = col; else c_bg = col; }
        break;
      }
      default:
        if (v >= 30 && v <= 37) c_fg = v - 30;
        else if (v >= 40 && v <= 47) c_bg = v - 40;
        else if (v >= 90 && v <= 97) c_fg = v - 90 + 8;
        else if (v >= 100 && v <= 107) c_bg = v - 100 + 8;
        break;
    }
  }
}

/* ================= モード設定 ================= */
static void switch_screen(int to_alt, int clear_on_enter)
{
  if (to_alt && !alt_active) {
    cur = scr_alt; alt_active = 1;
    if (clear_on_enter) { int r, c; for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) clear_cell(&cur[r][c]); }
  } else if (!to_alt && alt_active) {
    cur = scr_main; alt_active = 0;
  }
}
static void set_mode(int set)
{
  int i;
  for (i = 0; i < nparam; i++) {
    int v = param[i];
    if (csi_priv == '?') {
      switch (v) {
        case 1: decckm = set; break;
        case 7: decawm = set; break;
        case 25: cur_vis = set; break;
        case 47: switch_screen(set, 0); break;
        case 1047: switch_screen(set, set); break;
        case 1048: if (set) { sv_cx = cx; sv_cy = cy; } else { cx = sv_cx; cy = sv_cy; } break;
        case 1049:
          if (set) { sv_cx = cx; sv_cy = cy; sv_fg = c_fg; sv_bg = c_bg; switch_screen(1, 1); }
          else { switch_screen(0, 0); cx = sv_cx; cy = sv_cy; c_fg = sv_fg; c_bg = sv_bg; }
          break;
        default: break;
      }
    }
  }
}

/* ================= CSI 終端処理 ================= */
static void queue(const char *s) { while (*s && outq_len < (int)sizeof outq) outq[outq_len++] = (unsigned char)*s++; }

static void do_csi(unsigned char f)
{
  int n, a, b;
  switch (f) {
    case 'A': cy -= p_def(0, 1); if (cy < top) cy = top; pend_wrap = 0; break;
    case 'B': cy += p_def(0, 1); if (cy > bot) cy = bot; pend_wrap = 0; break;
    case 'C': cx += p_def(0, 1); if (cx >= COLS) cx = COLS - 1; pend_wrap = 0; break;
    case 'D': cx -= p_def(0, 1); if (cx < 0) cx = 0; pend_wrap = 0; break;
    case 'E': cy += p_def(0, 1); cx = 0; if (cy > bot) cy = bot; pend_wrap = 0; break;
    case 'F': cy -= p_def(0, 1); cx = 0; if (cy < top) cy = top; pend_wrap = 0; break;
    case 'G': case '`': cx = p_def(0, 1) - 1; clamp_cursor(); pend_wrap = 0; break;
    case 'd': cy = p_def(0, 1) - 1; clamp_cursor(); pend_wrap = 0; break;
    case 'H': case 'f':
      cy = p_def(0, 1) - 1; cx = p_def(1, 1) - 1; clamp_cursor(); pend_wrap = 0; break;
    case 'J':
      a = p_def(0, 0);
      if (a == 0) { erase_cells(cy, cx, COLS - 1); for (n = cy + 1; n < ROWS; n++) erase_cells(n, 0, COLS - 1); }
      else if (a == 1) { for (n = 0; n < cy; n++) erase_cells(n, 0, COLS - 1); erase_cells(cy, 0, cx); }
      else { for (n = 0; n < ROWS; n++) erase_cells(n, 0, COLS - 1); }
      break;
    case 'K':
      a = p_def(0, 0);
      if (a == 0) erase_cells(cy, cx, COLS - 1);
      else if (a == 1) erase_cells(cy, 0, cx);
      else erase_cells(cy, 0, COLS - 1);
      break;
    case 'L': { int cnt = p_def(0, 1), r;    /* IL */
      if (cy < top || cy > bot) break;
      for (r = bot; r >= cy + cnt; r--) for (n = 0; n < COLS; n++) cur[r][n] = cur[r - cnt][n];
      for (r = cy; r < cy + cnt && r <= bot; r++) erase_cells(r, 0, COLS - 1);
      break; }
    case 'M': { int cnt = p_def(0, 1), r;    /* DL */
      if (cy < top || cy > bot) break;
      for (r = cy; r <= bot - cnt; r++) for (n = 0; n < COLS; n++) cur[r][n] = cur[r + cnt][n];
      for (r = bot - cnt + 1; r <= bot && r >= top; r++) erase_cells(r, 0, COLS - 1);
      break; }
    case '@': { int cnt = p_def(0, 1), c; for (c = COLS - 1; c >= cx + cnt; c--) cur[cy][c] = cur[cy][c - cnt]; /* ICH */
      erase_cells(cy, cx, cx + cnt - 1); break; }
    case 'P': { int cnt = p_def(0, 1), c; for (c = cx; c + cnt < COLS; c++) cur[cy][c] = cur[cy][c + cnt]; /* DCH */
      erase_cells(cy, COLS - cnt, COLS - 1); break; }
    case 'X': { int cnt = p_def(0, 1); erase_cells(cy, cx, cx + cnt - 1); break; }  /* ECH */
    case 'S': scroll(p_def(0, 1)); break;
    case 'T': scroll(-p_def(0, 1)); break;
    case 'r':
      a = p_def(0, 1) - 1; b = (nparam >= 2 && param[1]) ? param[1] - 1 : ROWS - 1;
      if (a < 0) a = 0;
      if (b >= ROWS) b = ROWS - 1;
      if (a < b) { top = a; bot = b; cx = 0; cy = top; pend_wrap = 0; }
      break;
    case 'm': do_sgr(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 's': sv_cx = cx; sv_cy = cy; break;
    case 'u': cx = sv_cx; cy = sv_cy; clamp_cursor(); break;
    case 'n':
      if (csi_priv == 0 && p_def(0, 0) == 6) {   /* DSR: カーソル位置を報告 */
        char buf[16]; int r = cy + 1, c = cx + 1, k = 0;
        buf[k++] = 0x1b; buf[k++] = '[';
        if (r >= 10) buf[k++] = '0' + r / 10;
        buf[k++] = '0' + r % 10;
        buf[k++] = ';';
        if (c >= 10) buf[k++] = '0' + c / 10;
        buf[k++] = '0' + c % 10;
        buf[k++] = 'R'; buf[k] = 0; queue(buf);
      }
      break;
    case 'c': if (csi_priv == 0) queue("\x1b[?1;2c"); break;   /* DA: VT100 を名乗る */
    case 'g':
      if (p_def(0, 0) == 3) { int c; for (c = 0; c < COLS; c++) tabstop[c] = 0; }
      else tabstop[cx] = 0;
      break;
    default: break;
  }
}

/* ================= 文字列シーケンス (OSC/DCS/APC/PM) ================= */
/* 中身は捨てる。ST (ESC \) か BEL で終わる。 */
static void str_byte(unsigned char b)
{
  if (str_esc) { state = S_GROUND; str_esc = 0; return; }  /* ESC の次 = 終端 (\ 想定) */
  if (b == 0x1b) { str_esc = 1; return; }
  if (b == 0x07) { state = S_GROUND; return; }             /* BEL */
}

/* ================= ESC 直後 ================= */
static void do_esc(unsigned char b)
{
  switch (b) {
    case '[': csi_priv = 0; nparam = 0; param_seen = 0; param[0] = 0; state = S_CSI; return;
    case ']': case 'P': case '^': case '_': case 'X':
      str_kind = b; str_esc = 0; state = S_STR; return;
    case '(': cs_which = 0; state = S_CHARSET; return;
    case ')': cs_which = 1; state = S_CHARSET; return;
    case '7': sv_cx = cx; sv_cy = cy; sv_fg = c_fg; sv_bg = c_bg; sv_bold = a_bold; sv_ul = a_uline;
              sv_rev = a_reverse; sv_sp0 = g_special[0]; sv_sp1 = g_special[1]; sv_gl = gl; break;    /* DECSC */
    case '8': cx = sv_cx; cy = sv_cy; c_fg = sv_fg; c_bg = sv_bg; a_bold = sv_bold; a_uline = sv_ul;
              a_reverse = sv_rev; g_special[0] = sv_sp0; g_special[1] = sv_sp1; gl = sv_gl; clamp_cursor(); break; /* DECRC */
    case '=': break;   /* DECKPAM (キーパッドはキー変換側で扱わないので無視) */
    case '>': break;   /* DECKPNM */
    case 'D': if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++; pend_wrap = 0; break;   /* IND */
    case 'M': if (cy == top) scroll(-1); else if (cy > 0) cy--; pend_wrap = 0; break;         /* RI */
    case 'E': cx = 0; if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++; pend_wrap = 0; break; /* NEL */
    case 'H': tabstop[cx] = 1; break;   /* HTS */
    case 'c': /* RIS: 全リセット */
      { int r, c; cx = cy = 0; top = 0; bot = ROWS - 1; c_fg = 7; c_bg = 0;
        a_bold = a_uline = a_reverse = 0; decawm = 1; decckm = 0; cur_vis = 1; pend_wrap = 0;
        g_special[0] = g_special[1] = 0; gl = 0; switch_screen(0, 0);
        for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) clear_cell(&cur[r][c]); }
      break;
    case '#': state = S_ESC_INT; return;   /* DECALN 等 (# の後の 1 文字) */
    default: break;
  }
  state = S_GROUND;
}

/* ================= 1 バイト処理 ================= */
static void ground_ctrl(unsigned char b)
{
  switch (b) {
    case 0x07: break;                                 /* BEL (無視) */
    case 0x08: if (cx > 0) cx--; pend_wrap = 0; break; /* BS */
    case 0x09: { int c = cx + 1; while (c < COLS - 1 && !tabstop[c]) c++; cx = c; if (cx >= COLS) cx = COLS - 1; pend_wrap = 0; } break; /* HT */
    case 0x0a: case 0x0b: case 0x0c:                  /* LF/VT/FF */
      if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++; pend_wrap = 0; break;
    case 0x0d: cx = 0; pend_wrap = 0; break;          /* CR */
    case 0x0e: gl = 1; break;                         /* SO -> G1 */
    case 0x0f: gl = 0; break;                         /* SI -> G0 */
    default: break;
  }
}

/* UTF-8 を 1 バイトずつ食わせ、確定したコードポイントを返す。-1 は未確定。 */
static long utf8_feed(unsigned char b)
{
  if (u_need) {
    if ((b & 0xc0) != 0x80) { u_need = 0; return (b < 0x80) ? b : 0xfffd; }  /* 壊れている */
    u_cp = (u_cp << 6) | (b & 0x3f);
    if (--u_need == 0) return (long)u_cp;
    return -1;
  }
  if (b < 0x80) return b;
  if ((b & 0xe0) == 0xc0) { u_cp = b & 0x1f; u_need = 1; return -1; }
  if ((b & 0xf0) == 0xe0) { u_cp = b & 0x0f; u_need = 2; return -1; }
  if ((b & 0xf8) == 0xf0) { u_cp = b & 0x07; u_need = 3; return -1; }
  return 0xfffd;
}

static int is_wide(long cp)
{
  return (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
         (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
         (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
         (cp >= 0x20000 && cp <= 0x3fffd);
}

/* 確定コードポイントを画面に置く */
static void put_cp(long cp)
{
  int special = g_special[gl];
  if (cp >= 0x20 && cp < 0x7f) { put_glyph((unsigned char)cp, special); return; }
  if (cp == 0x7f) return;
  if (cp >= 0x2500 && cp <= 0x257f) {   /* Unicode 罫線 → DEC 特殊グリフに寄せる */
    unsigned char m = 0;
    switch (cp) {
      case 0x2500: case 0x2501: m = 0x71; break;  /* ─ */
      case 0x2502: case 0x2503: m = 0x78; break;  /* │ */
      case 0x250c: case 0x250f: m = 0x6c; break;  /* ┌ */
      case 0x2510: case 0x2513: m = 0x6b; break;  /* ┐ */
      case 0x2514: case 0x2517: m = 0x6d; break;  /* └ */
      case 0x2518: case 0x251b: m = 0x6a; break;  /* ┘ */
      case 0x251c: case 0x2523: m = 0x74; break;  /* ├ */
      case 0x2524: case 0x252b: m = 0x75; break;  /* ┤ */
      case 0x252c: case 0x2533: m = 0x77; break;  /* ┬ */
      case 0x2534: case 0x253b: m = 0x76; break;  /* ┴ */
      case 0x253c: case 0x254b: m = 0x6e; break;  /* ┼ */
      default: m = 0; break;
    }
    if (m) { put_glyph(m, 1); return; }
    put_glyph('+', 0); return;
  }
  if (cp == 0x2592) { put_glyph(0x61, 1); return; }   /* ▒ */
  /* その他は '?' (全角相当は 2 セル使って桁をずらさない) */
  put_glyph('?', 0);
  if (is_wide(cp)) put_glyph('?', 0);
}

void vt_write(const unsigned char *p, int n)
{
  int i, sup;
  /* 解析中に scroll()/blit() が VRAM・CGROM を直接触るので、vt_write 全体をスーパーバイザで動かす。
   * flush()/vt_init() 内の sup_enter はこの間は -1 を返す (入れ子は無害)。 */
  sup = sup_enter();
  for (i = 0; i < n; i++) {
    unsigned char b = p[i];
    switch (state) {
      case S_GROUND:
        if (b == 0x1b) { state = S_ESC; u_need = 0; }
        else if (b < 0x20) { ground_ctrl(b); u_need = 0; }
        else { long cp = utf8_feed(b); if (cp >= 0) put_cp(cp); }
        break;
      case S_ESC: do_esc(b); break;
      case S_ESC_INT: /* '#' の次 */
        if (b == '8') { int r, c; for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) { cur[r][c].ch = 'E'; resolve_attr(&cur[r][c]); } }
        state = S_GROUND; break;
      case S_CHARSET:
        g_special[cs_which] = (b == '0');   /* '0'=DEC 特殊, 'B'/'A' などは ASCII */
        state = S_GROUND; break;
      case S_CSI:
        if (b >= '0' && b <= '9') { if (nparam == 0) nparam = 1; param[nparam - 1] = param[nparam - 1] * 10 + (b - '0'); param_seen = 1; }
        else if (b == ';') { if (nparam < MAXPARAM) { param[nparam] = 0; nparam++; } }
        else if (b == '?' || b == '>' || b == '<' || b == '=') { csi_priv = b; }
        else if (b >= 0x20 && b <= 0x2f) { /* 中間バイトは無視 */ }
        else if (b >= 0x40 && b <= 0x7e) { do_csi(b); state = S_GROUND; }
        else if (b == 0x1b) { state = S_ESC; }
        else { state = S_GROUND; }
        break;
      case S_STR: str_byte(b); break;
      default: state = S_GROUND; break;
    }
  }
  flush();
  sup_leave(sup);
}

/* ================= 初期化・終了 ================= */
void vt_default_size(int *cols, int *rows) { if (cols) *cols = MAXCOLS; if (rows) *rows = MAXROWS; }
void vt_get_size(int *cols, int *rows) { if (cols) *cols = COLS; if (rows) *rows = ROWS; }

void vt_init(int cols, int rows)
{
  int r, c, s;
  if (cols <= 0) cols = MAXCOLS;
  if (rows <= 0) rows = MAXROWS;
  if (cols > MAXCOLS) cols = MAXCOLS;
  if (rows > MAXROWS) rows = MAXROWS;
  COLS = cols; ROWS = rows;
  if (!sg_ready) sg_build();
  cx = cy = 0; top = 0; bot = ROWS - 1; pend_wrap = 0;
  c_fg = 7; c_bg = 0; a_bold = a_uline = a_reverse = 0;
  decckm = 0; decawm = 1; cur_vis = 1; alt_active = 0; cur = scr_main;
  g_special[0] = g_special[1] = 0; gl = 0; outq_len = 0; state = S_GROUND; u_need = 0;
  csr_shown = 0;
  for (c = 0; c < MAXCOLS; c++) tabstop[c] = (c % 8 == 0) ? 1 : 0;
  s = sup_enter();
  palette_setup();
  _iocs_b_curoff();           /* BIOS のカーソルは使わない (自前で描く) */
  for (r = 0; r < ROWS; r++)
    for (c = 0; c < COLS; c++) { clear_cell(&cur[r][c]); drawn[r][c] = cur[r][c]; blit(c, r, &cur[r][c]); }
  sup_leave(s);
  _dos_breakck(0);            /* Ctrl-C ブレークを止める (キーはこちらで拾う) */
}

void vt_fini(void)
{
  int s, i;
  s = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }
  if (pal_saved) for (i = 0; i < 16; i++) TPAL[i] = pal_save[i];
  _iocs_b_curon();
  sup_leave(s);
  _dos_breakck(1);
}

/* ================= キーボード ================= */
/* _iocs_b_keysns: 空なら 0。_iocs_b_keyinp: 1 キー取り出し。戻り値の bit8-15 がキースキャン、bit0-7 が ASCII。 */
static int emit(unsigned char *o, int max, int *k, const char *s)
{
  while (*s && *k < max) o[(*k)++] = (unsigned char)*s++;
  return 0;
}
int vt_key(unsigned char *out, int max)
{
  int k = 0;
  /* 先にホストへの応答 (DSR/DA) を吐く */
  while (outq_len > 0 && k < max) { out[k++] = outq[0]; { int j; outq_len--; for (j = 0; j < outq_len; j++) outq[j] = outq[j + 1]; } }
  while (k < max && _iocs_b_keysns() != 0) {
    int code = _iocs_b_keyinp();
    int scan = (code >> 8) & 0xff;
    int asc = code & 0xff;
    const char *ck = decckm ? "O" : "[";
    char seq[8];
    switch (scan) {
      case 0x3c: seq[0] = 0x1b; seq[1] = ck[0]; seq[2] = 'A'; seq[3] = 0; emit(out, max, &k, seq); continue; /* ↑ */
      case 0x3e: seq[0] = 0x1b; seq[1] = ck[0]; seq[2] = 'B'; seq[3] = 0; emit(out, max, &k, seq); continue; /* ↓ */
      case 0x3d: seq[0] = 0x1b; seq[1] = ck[0]; seq[2] = 'C'; seq[3] = 0; emit(out, max, &k, seq); continue; /* → */
      case 0x3b: seq[0] = 0x1b; seq[1] = ck[0]; seq[2] = 'D'; seq[3] = 0; emit(out, max, &k, seq); continue; /* ← */
      case 0x36: emit(out, max, &k, "\x1b[H"); continue;    /* HOME */
      case 0x37: emit(out, max, &k, "\x1b[3~"); continue;   /* DEL */
      case 0x5e: emit(out, max, &k, "\x1b[2~"); continue;   /* INS */
      case 0x38: emit(out, max, &k, "\x1b[5~"); continue;   /* ROLL UP -> PgUp */
      case 0x39: emit(out, max, &k, "\x1b[6~"); continue;   /* ROLL DOWN -> PgDn */
      case 0x63: emit(out, max, &k, "\x1bOP"); continue;    /* F1 */
      case 0x64: emit(out, max, &k, "\x1bOQ"); continue;    /* F2 */
      case 0x65: emit(out, max, &k, "\x1bOR"); continue;    /* F3 */
      case 0x66: emit(out, max, &k, "\x1bOS"); continue;    /* F4 */
      case 0x67: emit(out, max, &k, "\x1b[15~"); continue;  /* F5 */
      case 0x68: emit(out, max, &k, "\x1b[17~"); continue;  /* F6 */
      case 0x69: emit(out, max, &k, "\x1b[18~"); continue;  /* F7 */
      case 0x6a: emit(out, max, &k, "\x1b[19~"); continue;  /* F8 */
      case 0x6b: emit(out, max, &k, "\x1b[20~"); continue;  /* F9 */
      case 0x6c: emit(out, max, &k, "\x1b[21~"); continue;  /* F10 */
      default: break;
    }
    if (asc == 0x08) { out[k++] = 0x7f; continue; }   /* BS -> DEL */
    if (asc != 0) out[k++] = (unsigned char)asc;       /* 印字可能・Ctrl 系・CR・TAB・ESC */
  }
  return k;
}

/* ================= デバッグ用 (vtcat がスナップショットを取るため) ================= */
/* 論理画面の 1 セルを取り出す。out[0]=ch, out[1]=fg, out[2]=bg, out[3]=flags。 */
void vt_get_cell(int r, int c, unsigned char out[4])
{
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) { out[0] = ' '; out[1] = 7; out[2] = 0; out[3] = 0; return; }
  out[0] = cur[r][c].ch; out[1] = cur[r][c].fg; out[2] = cur[r][c].bg; out[3] = cur[r][c].flags;
}
void vt_get_cursor(int *x, int *y) { if (x) *x = cx; if (y) *y = cy; }
