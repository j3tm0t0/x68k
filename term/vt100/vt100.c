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
/* Mac 上で試すスタブ版。VRAM/パレット/CGROM はホストメモリに逃がし、IOCS/DOS 呼び出しは無害化する。
 * CGROM (0xF00000〜、768KB) は vthost が CGROM イメージファイルを vt_host_cgrom に読み込む。
 * 描画は実機と同じ blit を通るので、vt_host_vram を PNG にすれば実機の画面と同じ絵になる。 */
static int _dos_super(int x) { (void)x; return 0; }
static void _dos_breakck(int x) { (void)x; }
static void _iocs_b_curon(void) {}
static void _iocs_b_curoff(void) {}
static int _iocs_b_keysns(void) { return 0; }
static int _iocs_b_keyinp(void) { return 0; }
unsigned char vt_host_vram[4 * 0x20000];
unsigned short vt_host_pal[16];
unsigned char vt_host_cgrom[0xC0000];   /* CGROM 0xF00000-0xFBFFFF の写し */
#else
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#endif
#include "vt100.h"
#include "kanjitab.h"   /* UTF-8(Unicode) -> JIS X0208 (genkanji.py 生成) */
#include "emojitab.h"   /* 絵文字 16x16・16色 (genemoji.py 生成、Twemoji 由来) */
#include "widthtab.h"   /* 全角・幅 0 の範囲表 (genwidth.py 生成、wcwidth 由来) */

/* ---- 画面の寸法 ---- */
#define MAXCOLS 96
#define MAXROWS 32
#define CW      8          /* セル幅 (ドット) */
#define CH      16         /* セル高 (ドット) */
#define VBYTES  128        /* 1 ライン 128 バイト (1 プレーン) */
#define PLANE   0x20000UL  /* プレーン間の距離 */
typedef unsigned int u32;  /* 32bit 転送用 (ホストの long は 8 バイトなので int) */

#ifdef VT_HOSTTEST
#define TVRAM  ((volatile unsigned char *)vt_host_vram)
#define TPAL   ((volatile unsigned short *)vt_host_pal)
#define CGBASE ((const unsigned char *)vt_host_cgrom)
#else
#define TVRAM  ((volatile unsigned char *)0xE00000UL)
#define TPAL   ((volatile unsigned short *)0xE82200UL)
#define CGBASE ((const unsigned char *)0xF00000UL)    /* CGROM 先頭 (16x16 漢字) */
#endif
#define CGROM16 (CGBASE + 0x3A800)   /* 8x16 ANK フォント (0xF3A800) */

/* ---- セル ---- */
/* cp: 描くコードポイント。0x20-0x7e は ANK、それ以外は Unicode (漢字など)。UTF-8 デコーダ
 * (utf8_feed) と描画層 (blit) を分けてあり、cp はコードポイントのまま持つ。
 * 全角 (East Asian Width = W/F) は 2 セルを占める: 左に F_WIDE、右に F_WIDECONT (継続・描かない)。 */
typedef struct {
  unsigned short cp;
  unsigned char fg;     /* 0..15 パレット番号 (bold / reverse 解決済み) */
  unsigned char bg;     /* 0..15 */
  unsigned char flags;  /* F_UNDERLINE | F_SPECIAL | F_WIDE | F_WIDECONT */
} cell_t;
#define F_UNDERLINE 0x01
#define F_SPECIAL   0x02   /* cp を DEC 罫線グリフ表で描く */
#define F_WIDE      0x04   /* 全角の左セル (16 ドット幅で描く) */
#define F_WIDECONT  0x08   /* 全角の右セル (描かない。左が両方を描く) */
#define F_EMOJI     0x10   /* cp は絵文字表のインデックス。16x16・16色で描く (F_WIDE と併用) */

static cell_t scr_main[MAXROWS][MAXCOLS];
static cell_t scr_alt[MAXROWS][MAXCOLS];
static cell_t drawn[MAXROWS][MAXCOLS];
static cell_t (*cur)[MAXCOLS] = scr_main;   /* いま表示している面 */

static int COLS = MAXCOLS, ROWS = MAXROWS;

/* 行ごとの「論理画面を書き換えた」印。flush は印の付いた行だけ drawn と比べる (全 3072 セルの走査を避ける) */
static unsigned char rdirty[MAXROWS];
#define TOUCH(r) (rdirty[r] = 1)
static void touch_rows(int r0, int r1) { int r; for (r = r0; r <= r1; r++) rdirty[r] = 1; }

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
enum { S_GROUND, S_ESC, S_CSI, S_STR, S_ESC_INT, S_CHARSET, S_ESC_SKIP };
static int state = S_GROUND;
static int csi_priv;                 /* '?' などの private 前置 */
static int csi_inter;                /* 中間バイト (0x20-0x2f、'$' や ' ' など。最後の 1 つ) */
#define MAXPARAM 32
#define MAXPVAL  9999                /* パラメータ値の上限 (桁あふれで負になり配列の外を書かないように) */
static int param[MAXPARAM], nparam, param_seen;
static int str_kind;                 /* 文字列シーケンスの種類 (']' 'P' など) */
static int str_esc;                  /* 文字列中で ESC を見たか (ST 待ち) */
#define STRBUF 8
static unsigned char str_buf[STRBUF];   /* 文字列の先頭 (OSC 10;? / 11;? の問い合わせを見分ける分だけ) */
static int str_len;
static int cs_which;                 /* 文字セット指定の対象 ')' か '(' か */

/* UTF-8 デコード */
static unsigned long u_cp;
static int u_need;

/* ホストへの応答キュー (DSR / DA など)。vt_key が先に吐く */
static unsigned char outq[256];
static int outq_len;

/* パレットの退避 */
static unsigned short pal_save[16];
static int pal_saved;

/* ---- 計測 (-DVT_PROF で数える。vtcat -b が表示する) ---- */
#ifdef VT_PROF
long vt_prof[8];   /* 0=blit したセル 1=scroll 呼び出し 2=TXRASCOPY 3=32bit 転送の行 4=flush 5=vt_write 6=put_cell 7=行塗り */
#define PROF(i, n) (vt_prof[i] += (n))
#else
#define PROF(i, n)
#endif

/* ---- スーパーバイザ ---- */
/* 戻り値を _dos_super に渡して戻す。負なら元からスーパーバイザなので触らない。 */
static int sup_enter(void) { return _dos_super(0); }
static void sup_leave(int s) { if (s >= 0) _dos_super(s); }

/* ================= DEC 特殊グラフィック (罫線) の 8x16 グリフ ================= */
/* VT100 の 0x60-0x7e。CGROM の罫線は 16 幅の漢字で使えないので自前で 8x16 を作る。
 * 横線はセル中央 (ラスタ 7,8)、縦線は中央列 (bit3 = 0x08)。使う記号だけ埋める。 */
static unsigned char sg_glyph[32][16];
static unsigned char blk_glyph[32][16];   /* U+2580-259F ブロック要素 */
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
  g = sg_glyph[0x7e - 0x60]; g[7] = g[8] = 0x18;                                  /* · 中点 */
  g = sg_glyph[0x66 - 0x60]; g[1] = g[4] = 0x30; g[2] = g[3] = 0x48;              /* ° */
  /* ブロック要素 U+2580-259F (Claude Code のロゴ ▐▛███▜▌ など)。四分割は上 8 ラスタ・下 8 ラスタ x 左 4・右 4 ドット */
  for (i = 0; i < 32; i++) {
    static const unsigned char quad[32] = {   /* bit3=左上 bit2=右上 bit1=左下 bit0=右下 (0xff は個別に作る) */
      0x0c, 0xff, 0xff, 0xff, 0x03, 0xff, 0xff, 0xff, 0x0f, 0xff, 0xff, 0xff, 0x0a, 0xff, 0xff, 0xff,
      0x05, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x01, 0x08, 0x0b, 0x09, 0x0e, 0x0d, 0x04, 0x06, 0x07 };
    unsigned char q = quad[i];
    g = blk_glyph[i];
    for (y = 0; y < 16; y++) g[y] = 0;
    if (q != 0xff) {
      for (y = 0; y < 16; y++) {
        int top = y < 8;
        g[y] = (unsigned char)(((q & (top ? 8 : 2)) ? 0xf0 : 0) | ((q & (top ? 4 : 1)) ? 0x0f : 0));
      }
    }
  }
  for (y = 0; y < 16; y++) {
    int k;
    for (k = 1; k <= 7; k++) if (y >= 16 - k * 2) blk_glyph[k][y] = 0xff;          /* ▁▂▃▄▅▆▇ 下から k/8 */
    for (k = 9; k <= 15; k++) blk_glyph[k][y] = (unsigned char)(0xff << (8 - (16 - k)));   /* ▉▊▋▌▍▎▏ 左から */
    blk_glyph[0x11][y] = (y & 1) ? 0x22 : 0x88;                                     /* ░ */
    blk_glyph[0x12][y] = (y & 1) ? 0x55 : 0xaa;                                     /* ▒ */
    blk_glyph[0x13][y] = (y & 1) ? 0xdd : 0x77;                                     /* ▓ */
    blk_glyph[0x14][y] = (y < 2) ? 0xff : 0;                                        /* ▔ */
    blk_glyph[0x15][y] = 0x01;                                                      /* ▕ */
  }
  blk_glyph[0x10][0] = 0; { int k; for (k = 0; k < 16; k++) blk_glyph[0x10][k] = 0x0f; }   /* ▐ 右半分 */
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
/* CGROM の ANK は JIS X 0201: 0x5c は ¥、0x7e はオーバーライン (‾)、0xa1-0xdf は半角カナ。
 * ASCII の \ は CGROM 0x80 (X68000 の文字セットのバックスラッシュ) で、~ は自前のグリフで描く
 * (実機で "moto@host:~$" の ~ が点に見えた)。逆に U+00A5 ¥・U+203E ‾ は 0x5c・0x7e で描く。 */
static const unsigned char tilde_glyph[16] = { 0,0,0,0,0,0, 0x32,0x4c, 0,0,0,0,0,0,0,0 };
static const unsigned char *glyph_of(const cell_t *c)   /* 8x16 ANK / DEC 罫線 (16 バイト) */
{
  if ((c->flags & F_SPECIAL) && c->cp >= 0x60 && c->cp <= 0x7f)
    return sg_glyph[c->cp - 0x60];
  if (c->cp == 0x7e)
    return tilde_glyph;
  if (c->cp == 0x5c)
    return CGROM16 + 0x80 * 16;
  if (c->cp >= 0x20 && c->cp <= 0x7e)
    return CGROM16 + (int)c->cp * 16;
  if (c->cp >= 0xff61 && c->cp <= 0xff9f)   /* 半角カナ */
    return CGROM16 + (int)(c->cp - 0xff61 + 0xa1) * 16;
  if (c->cp >= 0x2580 && c->cp <= 0x259f) return blk_glyph[c->cp - 0x2580];
  if (c->cp == 0x00a5) return CGROM16 + 0x5c * 16;
  if (c->cp == 0x203e) return CGROM16 + 0x7e * 16;
  return CGROM16 + 0x20 * 16;   /* 空白 */
}

/* 16x16 漢字グリフ (32 バイト = 2 バイト/行) の先頭。JIS 区点から CGROM アドレスを計算する。 */
static const unsigned char *kanji_glyph(unsigned short jis)
{
  int ku = (jis >> 8) - 0x20, ten = (jis & 0xff) - 0x20;
  if (ku < 1 || ku > 94 || ten < 1 || ten > 94 || (ku >= 9 && ku <= 15)) return CGBASE;
  /* CGROM には 9〜15 区が無い。1〜8 区 (非漢字) の直後に 16 区 (第 1 水準、0xF05E00) が続く */
  if (ku >= 16) ku -= 7;
  return CGBASE + (unsigned long)((ku - 1) * 94 + (ten - 1)) * 32;
}

/* 絵文字 16x16・16色 (4bpp) を 4 プレーンに直接描く (セルの fg/bg は使わず各ピクセル自身の色)。 */
static void blit_emoji(int col, int row, int idx)
{
  const unsigned char *bm = emojitab[idx].bm;   /* 16 行 x 8 バイト, 2px/byte, 上位ニブル = 左 */
  int s, p, px;
  for (s = 0; s < 16; s++) {
    unsigned char pl[4][2];
    volatile unsigned char *v;
    for (p = 0; p < 4; p++) { pl[p][0] = 0; pl[p][1] = 0; }
    for (px = 0; px < 16; px++) {
      unsigned char n = (px & 1) ? (bm[s * 8 + px / 2] & 0x0f) : (bm[s * 8 + px / 2] >> 4);
      int half = px >> 3, bit = 7 - (px & 7);
      for (p = 0; p < 4; p++) if ((n >> p) & 1) pl[p][half] |= (unsigned char)(1 << bit);
    }
    v = TVRAM + (unsigned long)(row * CH + s) * VBYTES + col;
    for (p = 0; p < 4; p++) { v[p * PLANE] = pl[p][0]; v[1 + p * PLANE] = pl[p][1]; }
  }
}

/* 1 プレーン x 16 ラスタを書く小関数。ラスタ間は VBYTES (128) なので v[i*128] は 68000 の d16(An) で届き、
 * 展開すると 1 バイト 1 命令 (move.b (a1)+,d16(a0) 等) になる。プレーンごとに「前景/背景のビット」の組で
 * 4 通り: 0/0 は 0、1/1 は 0xff、1/0 はグリフ、0/1 は反転グリフ (旧版は 1 ラスタごとに 4 プレーン分の
 * AND/OR を計算しており 1 セル数千クロックかかっていた)。st はグリフの 1 ラスタのバイト数 (半角 1 / 全角 2)。 */
#define R16(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
static void pl_fill(volatile unsigned char *v, unsigned char b)
{
#define PF(i) v[(i) * VBYTES] = b;
  R16(PF)
#undef PF
}
static void pl_copy1(volatile unsigned char *v, const unsigned char *g)
{
#define PC(i) v[(i) * VBYTES] = g[i];
  R16(PC)
#undef PC
}
static void pl_inv1(volatile unsigned char *v, const unsigned char *g)
{
#define PN(i) v[(i) * VBYTES] = (unsigned char)~g[i];
  R16(PN)
#undef PN
}
static void pl_copy2(volatile unsigned char *v, const unsigned char *g)
{
#define PC(i) v[(i) * VBYTES] = g[(i) * 2];
  R16(PC)
#undef PC
}
static void pl_inv2(volatile unsigned char *v, const unsigned char *g)
{
#define PN(i) v[(i) * VBYTES] = (unsigned char)~g[(i) * 2];
  R16(PN)
#undef PN
}

/* 1 セルを VRAM に描く (スーパーバイザ前提)。全角は左セルで 16 ドット幅 (2 バイト) を描く。 */
static void blit(int col, int row, const cell_t *c)
{
#ifdef VT_PROF_NOBLIT
  (void)col; (void)row; (void)c; PROF(0, 1); return;   /* 計測: 描画を抜いた版 (解析・差分走査だけの時間) */
#endif
  int fg = c->fg, bg = c->bg, p, wide = 0;
  const unsigned char *gp;
  unsigned char ul[32];
  volatile unsigned char *v;
  if (c->flags & F_WIDECONT) return;   /* 右半分は左の F_WIDE セルが描く */
  PROF(0, 1);
  if (c->flags & F_EMOJI) { blit_emoji(col, row, c->cp); return; }   /* cp = 絵文字インデックス */
  if (c->flags & F_WIDE) {
    unsigned short jis = ucs_to_jis(c->cp);
    if (jis) { gp = kanji_glyph(jis); wide = 1; }
    else gp = glyph_of(c);   /* 変換できない全角は空白相当 (put_cp 側で '?' に落とすので通常来ない) */
  } else {
    gp = glyph_of(c);
  }
  if (c->flags & F_UNDERLINE) {   /* 下線: 最下ラスタを埋めた写しを使う */
    int i, n = wide ? 32 : 16;
    for (i = 0; i < n; i++) ul[i] = gp[i];
    ul[n - 1] = 0xff; if (wide) ul[n - 2] = 0xff;
    gp = ul;
  }
  v = TVRAM + (unsigned long)row * CH * VBYTES + col;
  for (p = 0; p < 4; p++, v += PLANE) {
    int f = (fg >> p) & 1, b = (bg >> p) & 1;
    if (f == b) { pl_fill(v, f ? 0xff : 0); if (wide) pl_fill(v + 1, f ? 0xff : 0); }
    else if (wide) { if (f) { pl_copy2(v, gp); pl_copy2(v + 1, gp + 1); } else { pl_inv2(v, gp); pl_inv2(v + 1, gp + 1); } }
    else if (f) pl_copy1(v, gp);
    else pl_inv1(v, gp);
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
/* 32bit 単位でコピーする (src<dst のときは後ろから)。VRAM は 4 プレーン別々に。
 * 実機では上方向 (dst<src) のとき IOCS TXRASCOPY を使う。1 ラスタ = 4 走査線なので
 * 走査線番号は 4 の倍数で渡る (行は 16 走査線 = 4 ラスタ)。d1=(src<<8)|dst, d2=ラスタ数,
 * d3=プレーンマスク(0xF)。実機で row5->row3 のコピーを確認済み。
 * 下方向 (dst>src) は降順フラグが未検証なので 32bit 転送のままにする。 */
static void vram_move_lines(int dst_line, int src_line, int nlines)
{
  int p;
  long bytes = (long)nlines * VBYTES;
  PROF(3, nlines);
#ifndef VT_HOSTTEST
  if (dst_line < src_line && (dst_line & 3) == 0 && (src_line & 3) == 0 && (nlines & 3) == 0) {
    _iocs_txrascpy(((src_line / 4) << 8) | (dst_line / 4), nlines / 4, 0x000f);
    PROF(2, 1);
    return;
  }
#endif
  for (p = 0; p < 4; p++) {
    volatile unsigned char *base = TVRAM + (unsigned long)p * PLANE;
    volatile u32 *d = (volatile u32 *)(base + (long)dst_line * VBYTES);
    volatile u32 *s = (volatile u32 *)(base + (long)src_line * VBYTES);
    long n = bytes / 4, i;
    if ((unsigned long)d <= (unsigned long)s) {
      for (i = 0; i < n; i++) d[i] = s[i];
    } else {
      for (i = n - 1; i >= 0; i--) d[i] = s[i];
    }
  }
}

static void clear_cell(cell_t *c) { c->cp = 0x20; c->fg = c_fg; c->bg = c_bg; c->flags = 0; }

/* スクロール領域 [top,bot] を n 行上へ (正: 上スクロール / 負: 下スクロール) */
static void scroll(int n)
{
  int r, col, keep;
  if (n == 0) return;
  PROF(1, 1);
  if (n > bot - top + 1) n = bot - top + 1;
  if (-n > bot - top + 1) n = -(bot - top + 1);
  keep = (bot - top + 1) - (n > 0 ? n : -n);
  if (n > 0) {
    /* 論理バッファと drawn を上へ */
    for (r = top; r <= bot - n; r++)
      for (col = 0; col < COLS; col++) { cur[r][col] = cur[r + n][col]; drawn[r][col] = drawn[r + n][col]; }
    /* VRAM を上へ */
    if (keep > 0) vram_move_lines(top * CH, (top + n) * CH, keep * CH);
    /* 空いた行は論理画面だけ消す。VRAM のその行はずらす前の絵が残っていて drawn もその絵のまま
     * (上の移動で触っていない) なので、flush は前の行と違うセルだけ描けばよい (less の 1 行送りなど) */
    for (r = bot - n + 1; r <= bot; r++)
      for (col = 0; col < COLS; col++) clear_cell(&cur[r][col]);
  } else {
    n = -n;
    for (r = bot; r >= top + n; r--)
      for (col = 0; col < COLS; col++) { cur[r][col] = cur[r - n][col]; drawn[r][col] = drawn[r - n][col]; }
    if (keep > 0) vram_move_lines((top + n) * CH, top * CH, keep * CH);
    for (r = top; r < top + n; r++)
      for (col = 0; col < COLS; col++) clear_cell(&cur[r][col]);
  }
  touch_rows(top, bot);
}

/* ================= flush: logical と drawn の差分を描く ================= */
/* 見た目が同じか: 同じ字・属性。空白 (下線なし) どうしなら前景色は見えないので背景だけ比べる */
#define BLANKCELL(x) ((x)->cp == 0x20 && (x)->flags == 0)
static int same_look(const cell_t *a, const cell_t *b)
{
  if (a->cp != b->cp || a->bg != b->bg || a->flags != b->flags) return 0;
  return a->fg == b->fg || BLANKCELL(a);
}

/* 1 行 (COLS 桁 x 16 ライン x 4 プレーン) を背景色で塗る。空白だけの行 (消去・スクロールで空いた行) 用 */
static void row_fill(int row, int bg)
{
  int p, s, c;
  for (p = 0; p < 4; p++) {
    volatile unsigned char *base = TVRAM + (unsigned long)p * PLANE + (unsigned long)row * CH * VBYTES;
    unsigned char b = (bg >> p) & 1 ? 0xff : 0x00;
    if ((COLS & 3) == 0) {
      u32 w = b ? 0xffffffffU : 0;
      for (s = 0; s < CH; s++) {
        volatile u32 *v = (volatile u32 *)(base + s * VBYTES);
        for (c = 0; c < COLS / 4; c++) v[c] = w;
      }
    } else {
      for (s = 0; s < CH; s++) for (c = 0; c < COLS; c++) base[s * VBYTES + c] = b;
    }
  }
}

static void flush(void)
{
  int r, c, s;
  PROF(4, 1);
  s = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }   /* 先にカーソルを消す */
  for (r = 0; r < ROWS; r++) {
    cell_t *lrow = cur[r], *drow = drawn[r];
    int ndiff = 0, blank = 1, bg0;
    if (!rdirty[r]) continue;
    rdirty[r] = 0;
    bg0 = lrow[0].bg;
    for (c = 0; c < COLS; c++) {
      if (!same_look(&lrow[c], &drow[c])) ndiff++;
      if (!BLANKCELL(&lrow[c]) || lrow[c].bg != bg0) blank = 0;
    }
    if (ndiff == 0) { for (c = 0; c < COLS; c++) drow[c] = lrow[c]; continue; }
    if (blank && ndiff > 8) {   /* 空白だけの行で描き直しが多い: 行ごと塗る */
      row_fill(r, bg0);
      PROF(7, 1);
      for (c = 0; c < COLS; c++) drow[c] = lrow[c];
      continue;
    }
    for (c = 0; c < COLS; c++) {
      if (!same_look(&lrow[c], &drow[c])) blit(c, r, &lrow[c]);
      drow[c] = lrow[c];
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

/* 書き込みで全角ペアを割る場合、相方のセルを空白に戻す (孤立した半分を残さない) */
static void break_pair(int r, int c)
{
  if (c < 0 || c >= COLS) return;
  TOUCH(r);
  if ((cur[r][c].flags & F_WIDE) && c + 1 < COLS) clear_cell(&cur[r][c + 1]);
  else if ((cur[r][c].flags & F_WIDECONT) && c - 1 >= 0) clear_cell(&cur[r][c - 1]);
}

/* コードポイント/インデックスを現在位置に置く。width=1 (半角) / 2 (全角)。
 * extra は追加フラグ (F_SPECIAL = DEC 罫線 / F_EMOJI = cp は絵文字インデックス)。 */
static void put_cell(unsigned short cp, int extra, int width)
{
  cell_t *c;
  PROF(6, 1);
  if (pend_wrap) {        /* 前回末尾に置いた後の遅延折り返し */
    cx = 0;
    if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++;
    pend_wrap = 0;
  }
  if (width == 2 && COLS < 2) { cp = '?'; extra = 0; width = 1; }   /* 1 桁幅の画面に全角は置けない */
  if (width == 2 && cx == COLS - 1) {   /* 全角が右端に 1 桁しか入らない: 空けて次行へ */
    break_pair(cy, cx); clear_cell(&cur[cy][cx]); TOUCH(cy);
    cx = 0;
    if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++;
  }
  break_pair(cy, cx);
  TOUCH(cy);
  c = &cur[cy][cx];
  c->cp = cp;
  resolve_attr(c);
  c->flags |= extra;
  if (width == 2) {
    c->flags |= F_WIDE;
    break_pair(cy, cx + 1);
    c = &cur[cy][cx + 1];
    c->cp = 0x20; resolve_attr(c); c->flags |= F_WIDECONT;
    if (cx + 1 == COLS - 1) { if (decawm) pend_wrap = 1; }
    else cx += 2;
  } else {
    if (cx == COLS - 1) { if (decawm) pend_wrap = 1; }
    else cx++;
  }
}

/* ================= カーソル移動・消去 ================= */
static void clamp_cursor(void) { if (cx < 0) cx = 0; if (cx >= COLS) cx = COLS - 1; if (cy < 0) cy = 0; if (cy >= ROWS) cy = ROWS - 1; }

static void erase_cells(int r, int c0, int c1)
{
  int c;
  TOUCH(r);
  for (c = c0; c <= c1 && c < COLS; c++) clear_cell(&cur[r][c]);
}

/* ================= パラメータ ================= */
static int p_def(int i, int def) {
  if (i >= nparam) return def;
  return param[i] ? param[i] : def;
}

/* ================= SGR ================= */
/* 24bit 色をパレット 16 色 (原色 6 + 白黒、各 2 段) に寄せる。RGB の距離で選ぶと中間色 (Claude Code の
 * 橙 215,119,87 など) がみな灰色になるので、彩度があれば色相で 6 色から選び、明るさで bright を決める。
 * 彩度が低ければ明るさだけで 黒/暗灰/灰/白。 */
static int nearest16(int r, int g, int b)
{
  int mx = r, mn = r, c, h;
  if (g > mx) mx = g;
  if (b > mx) mx = b;
  if (g < mn) mn = g;
  if (b < mn) mn = b;
  c = mx - mn;
  if (c < 64) {   /* 無彩色 */
    int l = (r * 3 + g * 6 + b) / 10;
    return l < 48 ? 0 : l < 128 ? 8 : l < 214 ? 7 : 15;
  }
  if (mx == r) { h = 60 * (g - b) / c; if (h < 0) h += 360; }
  else if (mx == g) h = 120 + 60 * (b - r) / c;
  else h = 240 + 60 * (r - g) / c;
  /* 色相 330-30 赤 / 30-90 黄 / 90-150 緑 / 150-210 シアン / 210-270 青 / 270-330 マゼンタ */
  {
    static const unsigned char hue6[6] = { 1, 3, 2, 6, 4, 5 };
    int idx = hue6[((h + 30) % 360) / 60];
    return mx >= 200 ? idx + 8 : idx;
  }
}
/* 256 色: 0-15 はそのまま、16-231 は 6x6x6 キューブ、232-255 は灰色。xterm の RGB に直して最寄りの 16 色へ */
static int map256(int n)
{
  static const unsigned char lv[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
  if (n < 0 || n > 255) return -1;
  if (n < 16) return n;
  if (n >= 232) { int v = 8 + (n - 232) * 10; return nearest16(v, v, v); }
  n -= 16;
  return nearest16(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
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
      case 38: case 48: {   /* 38;5;N / 38;2;R;G;B (':' 区切りも ';' と同じに読む)。足りない引数は 0 */
        int to_fg = (v == 38), col = -1;
        if (i + 1 < nparam && param[i + 1] == 5) { col = map256(i + 2 < nparam ? param[i + 2] : 0); i += 2; }
        else if (i + 1 < nparam && param[i + 1] == 2) {
          int r = i + 2 < nparam ? param[i + 2] : 0, g = i + 3 < nparam ? param[i + 3] : 0, b = i + 4 < nparam ? param[i + 4] : 0;
          col = nearest16(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b); i += 4;
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
  touch_rows(0, ROWS - 1);
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
/* 応答は丸ごと入るときだけ積む (途中で切れた応答はホスト側のパーサを壊す) */
static void queue(const char *s)
{
  int n = 0;
  while (s[n]) n++;
  if (outq_len + n > (int)sizeof outq) return;
  while (*s) outq[outq_len++] = (unsigned char)*s++;
}
static char *put_dec(char *b, int v)   /* 10 進を書いて次の位置を返す (0..9999) */
{
  if (v >= 1000) *b++ = '0' + v / 1000 % 10;
  if (v >= 100) *b++ = '0' + v / 100 % 10;
  if (v >= 10) *b++ = '0' + v / 10 % 10;
  *b++ = '0' + v % 10;
  return b;
}

/* 行の挿入/削除 (IL/DL): カーソル行から下端までを領域としてスクロールする (VRAM も TXRASCOPY でずらす)。
 * 領域外では無視。DEC に合わせてカーソルは行頭へ。 */
static void insdel_lines(int cnt)
{
  int save_top = top;
  if (cy < top || cy > bot) return;
  top = cy;
  scroll(cnt);   /* 正: DL (上へ) / 負: IL (下へ) */
  top = save_top;
  cx = 0; pend_wrap = 0;
}

static int mode_state(int v)   /* DECRQM の答え: 1=set 2=reset 0=知らない */
{
  switch (v) {
    case 1: return decckm ? 1 : 2;
    case 7: return decawm ? 1 : 2;
    case 25: return cur_vis ? 1 : 2;
    case 47: case 1047: case 1049: return alt_active ? 1 : 2;
    default: return 0;
  }
}

static void do_csi(unsigned char f)
{
  int n, a, b;
  char buf[32], *q;
  if (csi_inter) {   /* 中間バイト付き: DECRQM (CSI ? Ps $ p) だけ答え、他 (DECSCUSR "CSI Ps SP q" など) は無視 */
    if (csi_inter == '$' && f == 'p') {
      q = buf; *q++ = 0x1b; *q++ = '[';
      if (csi_priv == '?') *q++ = '?';
      q = put_dec(q, p_def(0, 0)); *q++ = ';';
      q = put_dec(q, csi_priv == '?' ? mode_state(p_def(0, 0)) : 0);
      *q++ = '$'; *q++ = 'y'; *q = 0; queue(buf);
    }
    return;
  }
  if (csi_priv && csi_priv != '?') {   /* '>' '=' '<' 前置: DA2 だけ答え、他 (kitty キーボード CSI > u、modifyOtherKeys CSI > 4;2 m 等) は無視 */
    if (csi_priv == '>' && f == 'c') queue("\x1b[>0;95;0c");
    return;
  }
  switch (f) {
    case 'A': if (cy >= top) { cy -= p_def(0, 1); if (cy < top) cy = top; } else { cy -= p_def(0, 1); if (cy < 0) cy = 0; } pend_wrap = 0; break;
    case 'B': if (cy <= bot) { cy += p_def(0, 1); if (cy > bot) cy = bot; } else { cy += p_def(0, 1); if (cy >= ROWS) cy = ROWS - 1; } pend_wrap = 0; break;
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
      else if (a == 2) { for (n = 0; n < ROWS; n++) erase_cells(n, 0, COLS - 1); }
      break;   /* 3 (スクロールバック消去) は持っていないので何もしない */
    case 'K':
      a = p_def(0, 0);
      if (a == 0) erase_cells(cy, cx, COLS - 1);
      else if (a == 1) erase_cells(cy, 0, cx);
      else erase_cells(cy, 0, COLS - 1);
      break;
    case 'L': insdel_lines(-p_def(0, 1)); break;   /* IL */
    case 'M': insdel_lines(p_def(0, 1)); break;    /* DL */
    case '@': { int cnt = p_def(0, 1), c;    /* ICH */
      if (cnt > COLS - cx) cnt = COLS - cx;
      TOUCH(cy);
      for (c = COLS - 1; c >= cx + cnt; c--) cur[cy][c] = cur[cy][c - cnt];
      erase_cells(cy, cx, cx + cnt - 1); pend_wrap = 0; break; }
    case 'P': { int cnt = p_def(0, 1), c;    /* DCH */
      if (cnt > COLS - cx) cnt = COLS - cx;
      TOUCH(cy);
      for (c = cx; c + cnt < COLS; c++) cur[cy][c] = cur[cy][c + cnt];
      erase_cells(cy, COLS - cnt, COLS - 1); pend_wrap = 0; break; }
    case 'X': { int cnt = p_def(0, 1); if (cnt > COLS - cx) cnt = COLS - cx; erase_cells(cy, cx, cx + cnt - 1); break; }  /* ECH */
    case 'S': scroll(p_def(0, 1)); break;
    case 'T': scroll(-p_def(0, 1)); break;
    case 'r':
      a = p_def(0, 1) - 1; b = (nparam >= 2 && param[1]) ? param[1] - 1 : ROWS - 1;
      if (a < 0) a = 0;
      if (b >= ROWS) b = ROWS - 1;
      if (a < b) { top = a; bot = b; cx = 0; cy = 0; pend_wrap = 0; }
      break;
    case 'm': if (csi_priv == 0) do_sgr(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 's': if (csi_priv == 0) { sv_cx = cx; sv_cy = cy; } break;               /* SCOSC */
    case 'u': if (csi_priv == 0) { cx = sv_cx; cy = sv_cy; clamp_cursor(); } break; /* SCORC (CSI ? u 等は上で除外) */
    case 'n':
      if (csi_priv == 0 && p_def(0, 0) == 5) queue("\x1b[0n");        /* DSR: 状態 = 正常 */
      else if (p_def(0, 0) == 6) {                                    /* DSR: カーソル位置を報告 (CSI ? 6 n は DECXCPR) */
        q = buf; *q++ = 0x1b; *q++ = '[';
        if (csi_priv == '?') *q++ = '?';
        q = put_dec(q, cy + 1); *q++ = ';'; q = put_dec(q, cx + 1); *q++ = 'R'; *q = 0; queue(buf);
      }
      break;
    case 'c': if (csi_priv == 0 && p_def(0, 0) == 0) queue("\x1b[?1;2c"); break;   /* DA: VT100 を名乗る */
    case 't':   /* XTWINOPS: 18 = 文字単位の大きさを報告。他 (タイトル退避など) は無視 */
      if (p_def(0, 0) == 18) {
        q = buf; *q++ = 0x1b; *q++ = '['; *q++ = '8'; *q++ = ';';
        q = put_dec(q, ROWS); *q++ = ';'; q = put_dec(q, COLS); *q++ = 't'; *q = 0; queue(buf);
      }
      break;
    case 'g':
      if (p_def(0, 0) == 3) { int c; for (c = 0; c < COLS; c++) tabstop[c] = 0; }
      else tabstop[cx] = 0;
      break;
    default: break;
  }
}

/* ================= 文字列シーケンス (OSC/DCS/APC/PM) ================= */
/* 中身は捨てる (先頭 STRBUF バイトだけ控えて OSC の色問い合わせに答える)。長さに上限は要らない (溜めないので)。
 * 終端は BEL か ST (ESC \)。ESC の後が \ 以外なら文字列はそこで打ち切り、その ESC から新しいシーケンスとして読む
 * (終端の無い OSC が後ろの CSI を飲み込まないように)。DCS の中の ESC ESC (tmux パススルーの二重 ESC) は中身扱い。 */
static void do_esc(unsigned char b);
static void str_end(int st)   /* st: 1=ST で終わった / 0=BEL */
{
  if (str_kind == ']' && str_len == 4 && str_buf[0] == '1' && (str_buf[1] == '0' || str_buf[1] == '1') &&
      str_buf[2] == ';' && str_buf[3] == '?') {   /* OSC 10;? / 11;? (前景・背景色の問い合わせ) */
    queue(str_buf[1] == '0' ? "\x1b]10;rgb:aaaa/aaaa/aaaa" : "\x1b]11;rgb:0000/0000/0000");
    queue(st ? "\x1b\\" : "\x07");
  }
  state = S_GROUND;
}
static void str_byte(unsigned char b)
{
  if (str_esc) {
    str_esc = 0;
    if (b == '\\') { str_end(1); return; }
    if (b == 0x1b && str_kind == 'P') return;    /* DCS 内の ESC ESC */
    str_end(1); state = S_ESC; do_esc(b); return;  /* ESC で打ち切り、b は ESC の次として読む */
  }
  if (b == 0x1b) { str_esc = 1; return; }
  if (b == 0x07) { str_end(0); return; }
  if (b == 0x18 || b == 0x1a) { state = S_GROUND; return; }   /* CAN / SUB で取り消し */
  if (str_len < STRBUF) str_buf[str_len] = b;
  if (str_len < 32767) str_len++;
}

/* ================= ESC 直後 ================= */
static void do_esc(unsigned char b)
{
  switch (b) {
    case '[': csi_priv = 0; csi_inter = 0; nparam = 0; param_seen = 0; param[0] = 0; state = S_CSI; return;
    case ']': case 'P': case '^': case '_': case 'X':
      str_kind = b; str_esc = 0; str_len = 0; state = S_STR; return;
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
        for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) clear_cell(&cur[r][c]);
        touch_rows(0, ROWS - 1); }
      break;
    case '#': state = S_ESC_INT; return;   /* DECALN 等 (# の後の 1 文字) */
    default:
      /* 他の中間バイト (ESC % G = UTF-8 選択、ESC SP F、ESC * / + = G2/G3 指定など) は終端の 1 文字まで読み捨てる */
      if (b >= 0x20 && b <= 0x2f) { state = S_ESC_SKIP; return; }
      break;
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

static int is_wide(long cp)   /* 全角 (2 桁) か。widthtab.h (wcwidth 由来、East Asian Width W/F + 絵文字) */
{
  if (cp < 0x1100) return 0;
  return in_ranges(widetab, (int)WIDETAB_N, (unsigned long)cp);
}

/* 字形の無い文字の代用: ASCII (*a) か DEC 特殊グリフ (*m) に寄せる (Claude Code などの TUI の記号)。
 * 見つかれば 1。全角の記号 (✅ ⚡ など) にも使い、そのときは代用字 + 空白で 2 桁を保つ。 */
static int fallback(long cp, unsigned char *a, unsigned char *m)
{
  static const char latin1[] =   /* U+00C0-00FF のアクセント付きラテン文字 -> アクセントを外した字 */
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  *a = 0; *m = 0;
  if (cp >= 0xc0 && cp <= 0xff) { *a = (unsigned char)latin1[cp - 0xc0]; return 1; }
  switch (cp) {
    case 0x23bf: *m = 0x6d; break;                                  /* ⎿ → └ */
    case 0x00b7: case 0x2022: case 0x2219: case 0x30fb: *m = 0x7e; break;   /* · • → 中点 */
    case 0x00b0: *m = 0x66; break;                                  /* ° */
    case 0x00b1: *a = '+'; break;                                   /* ± */
    case 0x2264: *a = '<'; break;                                   /* ≤ */
    case 0x2265: *a = '>'; break;                                   /* ≥ */
    case 0x23fa: case 0x25cf: case 0x25cb: case 0x25c9: case 0x25ef: case 0x26aa: case 0x26ab: *a = 'o'; break;   /* ⏺ ● ○ ◉ */
    case 0x273b: case 0x273d: case 0x2736: case 0x2733: case 0x2722: case 0x2217: case 0x2731: case 0x2728: *a = '*'; break;   /* ✻ ✽ ✶ ✳ ✢ ✨ */
    case 0x276f: case 0x203a: case 0x25b8: case 0x25b6: case 0x23f5: case 0x2192: case 0x21d2: case 0x23e9: *a = '>'; break;   /* ❯ › ▸ ▶ ⏵ → */
    case 0x2039: case 0x25c2: case 0x25c0: case 0x2190: case 0x23ea: *a = '<'; break;
    case 0x2191: case 0x25b4: case 0x25b2: *a = '^'; break;
    case 0x2193: case 0x25be: case 0x25bc: *a = 'v'; break;
    case 0x2713: case 0x2714: case 0x2705: case 0x2611: *a = 'v'; break;   /* ✓ ✅ */
    case 0x2717: case 0x2718: case 0x00d7: case 0x2715: case 0x274c: case 0x274e: *a = 'x'; break;   /* ✗ × ❌ */
    case 0x2026: *a = '.'; break;                                   /* … (1 桁なので 1 字) */
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: *a = '-'; break;
    case 0x2018: case 0x2019: *a = '\''; break;
    case 0x201c: case 0x201d: *a = '"'; break;
    case 0x26a0: case 0x2757: case 0x2755: *a = '!'; break;         /* ⚠ ❗ */
    case 0x2753: case 0x2754: *a = '?'; break;
    case 0x26a1: *a = 'z'; break;                                   /* ⚡ */
    case 0x23f0: case 0x23f1: case 0x23f2: case 0x23f3: case 0x231a: case 0x231b: *a = '@'; break;   /* ⏰ ⏳ ⌚ (時計) */
    case 0x2b50: case 0x2605: case 0x2606: *a = '*'; break;          /* ⭐ ★ ☆ */
    case 0x2795: *a = '+'; break;
    case 0x2796: *a = '-'; break;
    case 0x00a0: *a = ' '; break;
    case 0x00a9: *a = 'c'; break;
    case 0x00ae: *a = 'r'; break;
    default: return 0;
  }
  return 1;
}

/* 確定コードポイントを画面に置く */
static void put_cp(long cp)
{
  int special = g_special[gl];
  unsigned char a, m;
  if (cp >= 0x20 && cp < 0x7f) { put_cell((unsigned short)cp, special ? F_SPECIAL : 0, 1); return; }   /* ASCII (最頻) */
  if (cp == 0x7f) return;
  /* 幅 0 の文字は捨てる (桁を進めない): 異体字セレクタ、ZWJ/ZWSP・方向制御、結合文字 (widthtab.h)。
   * 前の文字との合成はしない。絵文字の肌色修飾 (U+1F3FB-1F3FF) は wcwidth では 2 だが、前の絵文字に
   * 重なって 1 字に見えるのが普通なので捨てる。 */
  if ((cp >= 0x1f3fb && cp <= 0x1f3ff) || in_ranges(zerotab, (int)ZEROTAB_N, (unsigned long)cp)) return;
  { int ei = emoji_index((unsigned long)cp);   /* 絵文字: 16x16・16色で全角 2 セル */
    if (ei >= 0) { put_cell((unsigned short)ei, F_EMOJI, 2); return; } }
  if ((cp >= 0xff61 && cp <= 0xff9f) || cp == 0x00a5 || cp == 0x203e) { put_cell((unsigned short)cp, 0, 1); return; }   /* CGROM の JIS X 0201 にある半角 */
  if (cp >= 0x2500 && cp <= 0x257f) {   /* Unicode 罫線 → DEC 特殊グリフに寄せる (半角) */
    m = 0;
    switch (cp) {
      case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509: case 0x254c: case 0x254d: case 0x2550: m = 0x71; break;  /* ─ ═ ┄ */
      case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250a: case 0x250b: case 0x254e: case 0x254f: case 0x2551: m = 0x78; break;  /* │ ║ */
      case 0x250c: case 0x250f: case 0x256d: case 0x2554: m = 0x6c; break;  /* ┌ ╭ ╔ */
      case 0x2510: case 0x2513: case 0x256e: case 0x2557: m = 0x6b; break;  /* ┐ ╮ ╗ */
      case 0x2514: case 0x2517: case 0x2570: case 0x255a: m = 0x6d; break;  /* └ ╰ ╚ */
      case 0x2518: case 0x251b: case 0x256f: case 0x255d: m = 0x6a; break;  /* ┘ ╯ ╝ */
      case 0x251c: case 0x2523: case 0x2560: m = 0x74; break;  /* ├ */
      case 0x2524: case 0x252b: case 0x2563: m = 0x75; break;  /* ┤ */
      case 0x252c: case 0x2533: case 0x2566: m = 0x77; break;  /* ┬ */
      case 0x2534: case 0x253b: case 0x2569: m = 0x76; break;  /* ┴ */
      case 0x253c: case 0x254b: case 0x256c: m = 0x6e; break;  /* ┼ */
      default: m = 0; break;
    }
    if (m) { put_cell(m, F_SPECIAL, 1); return; }
    put_cell('+', 0, 1); return;
  }
  if (cp >= 0x2580 && cp <= 0x259f) { put_cell((unsigned short)cp, 0, 1); return; }   /* ブロック要素 (自前グリフ) */
  if (is_wide(cp)) {
    unsigned short jis = ucs_to_jis(cp);
    if (jis) { put_cell((unsigned short)cp, 0, 2); return; }   /* 全角: CGROM 16x16 漢字 */
    if (fallback(cp, &a, &m)) { if (m) put_cell(m, F_SPECIAL, 1); else put_cell(a, 0, 1); put_cell(' ', 0, 1); return; }
    put_cell('?', 0, 1); put_cell('?', 0, 1);   /* 変換不能な全角は '??' で桁を保つ */
    return;
  }
  if (fallback(cp, &a, &m)) { if (m) put_cell(m, F_SPECIAL, 1); else put_cell(a, 0, 1); return; }
  put_cell('?', 0, 1);   /* 半角の未対応文字 */
}

void vt_write(const unsigned char *p, int n)
{
  int i, sup;
  PROF(5, 1);
  /* 解析中に scroll()/blit() が VRAM・CGROM を直接触るので、vt_write 全体をスーパーバイザで動かす。
   * flush()/vt_init() 内の sup_enter はこの間は -1 を返す (入れ子は無害)。 */
  sup = sup_enter();
  /* 解析中の scroll() が VRAM をラスタ単位で動かすので、描いてあるソフトカーソルを先に消す
   * (消さないとカーソルの塊ごとスクロールし、flush の消去 XOR が旧位置に塊を残す。実機で確認)。 */
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }
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
        if (b == '8') { int r, c; for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) { cur[r][c].cp = 'E'; resolve_attr(&cur[r][c]); } touch_rows(0, ROWS - 1); }
        state = S_GROUND; break;
      case S_ESC_SKIP:   /* 中間バイトは続けて読み、終端 (0x30-0x7e) で終わり */
        if (b == 0x1b) state = S_ESC;
        else if (b < 0x20) ground_ctrl(b);
        else if (b >= 0x30) state = S_GROUND;
        break;
      case S_CHARSET:
        g_special[cs_which] = (b == '0');   /* '0'=DEC 特殊, 'B'/'A' などは ASCII */
        state = S_GROUND; break;
      case S_CSI:
        if (b >= '0' && b <= '9') {   /* 値は MAXPVAL で頭打ち、MAXPARAM 個を越えた分は捨てる */
          if (nparam == 0) nparam = 1;
          if (nparam <= MAXPARAM) { int *v = &param[nparam - 1]; *v = *v * 10 + (b - '0'); if (*v > MAXPVAL) *v = MAXPVAL; }
          param_seen = 1;
        }
        else if (b == ';' || b == ':') {   /* ':' (38:5:N などのサブパラメータ) も区切りとして読む */
          if (nparam == 0) nparam = 1;    /* 先頭が空 (CSI ;5H) */
          if (nparam < MAXPARAM) param[nparam] = 0;
          if (nparam <= MAXPARAM) nparam++;
        }
        else if (b == '?' || b == '>' || b == '<' || b == '=') { csi_priv = b; }
        else if (b >= 0x20 && b <= 0x2f) { csi_inter = b; }
        else if (b >= 0x40 && b <= 0x7e) { if (nparam > MAXPARAM) nparam = MAXPARAM; do_csi(b); state = S_GROUND; }
        else if (b == 0x1b) { state = S_ESC; }
        else if (b < 0x20) { ground_ctrl(b); }   /* CSI の途中の制御文字はその場で実行 (VT100 の動き) */
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
    { for (c = 0; c < COLS; c++) { clear_cell(&cur[r][c]); drawn[r][c] = cur[r][c]; } row_fill(r, c_bg); }
  for (r = 0; r < MAXROWS; r++) rdirty[r] = 0;
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
#ifdef VT_HOSTTEST
/* 試験用の不変条件の確認: いまの VRAM が「論理画面を全部描き直した絵 (+ カーソル)」と同じか。
 * 差分描画・スクロールの最適化で描き漏れ / 描き過ぎがあれば 0 を返す (vtcat -V が vt_write ごとに呼ぶ)。 */
int vt_host_verify(void)
{
  static unsigned char save[4 * 0x20000];
  int r, c, p, ok = 1;
  for (p = 0; p < 4 * 0x20000; p++) save[p] = vt_host_vram[p];
  for (r = 0; r < ROWS; r++)
    for (c = 0; c < COLS; c++) blit(c, r, &cur[r][c]);
  if (csr_shown) cursor_xor(csr_x, csr_y);
  for (p = 0; p < 4 && ok; p++)
    for (r = 0; r < ROWS * CH && ok; r++)
      for (c = 0; c < COLS; c++) {
        long o = (long)p * PLANE + (long)r * VBYTES + c;
        if (save[o] != vt_host_vram[o]) { ok = 0; break; }
      }
  for (p = 0; p < 4 * 0x20000; p++) vt_host_vram[p] = save[p];
  return ok;
}
#endif

/* 論理画面の 1 セルを取り出す。cp=コードポイント, fg/bg=色, fl=フラグ。 */
void vt_get_cell(int r, int c, unsigned short *cp, unsigned char *fg, unsigned char *bg, unsigned char *fl)
{
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) { if (cp) *cp = ' '; if (fg) *fg = 7; if (bg) *bg = 0; if (fl) *fl = 0; return; }
  if (cp) *cp = cur[r][c].cp;
  if (fg) *fg = cur[r][c].fg;
  if (bg) *bg = cur[r][c].bg;
  if (fl) *fl = cur[r][c].flags;
}
void vt_get_cursor(int *x, int *y) { if (x) *x = cx; if (y) *y = cy; }
/* 絵文字インデックス -> 実際の Unicode コードポイント (F_EMOJI セルの cp を渡す)。 */
unsigned long vt_emoji_cp(int idx) { return (idx >= 0 && idx < EMOJITAB_N) ? emojitab[idx].cp : '?'; }
