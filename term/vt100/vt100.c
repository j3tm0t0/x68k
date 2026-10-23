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
#ifdef VT_HOSTKEYS   /* キーは試験が渡す (net/ssh/dropbear/tests/vcshim.c) */
int vt_host_keysns(void);
int vt_host_keyinp(void);
#define _iocs_b_keysns vt_host_keysns
#define _iocs_b_keyinp vt_host_keyinp
#else
static int _iocs_b_keysns(void) { return 0; }
static int _iocs_b_keyinp(void) { return 0; }
#endif
#ifdef VT_SIM_TXRAS
void _iocs_txrascpy(int d1, int d2, int d3);   /* 68000 シミュレータでの計測用: シミュレータが TXRASCOPY を演じる */
#endif
unsigned char vt_host_vram[4 * 0x20000];
unsigned short vt_host_pal[16];
unsigned char vt_host_cgrom[0xC0000];   /* CGROM 0xF00000-0xFBFFFF の写し */
unsigned short vt_host_r21;             /* CRTC R21 (68000 シミュレータは同時アクセスを演じる。ホスト版はただの変数) */
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
#define TVRAM_PHYS ((volatile unsigned char *)vt_host_vram)
#define TPAL   ((volatile unsigned short *)vt_host_pal)
#define CRTC_R21 (*(volatile unsigned short *)&vt_host_r21)
#define CGBASE ((const unsigned char *)vt_host_cgrom)
#else
#define TVRAM_PHYS ((volatile unsigned char *)0xE00000UL)
#define TPAL   ((volatile unsigned short *)0xE82200UL)
#define CRTC_R21 (*(volatile unsigned short *)0xE8002AUL)   /* bit8 同時アクセス、bit4-7 その対象プレーン、bit0-3 ラスタコピーのプレーン */
#define CGBASE ((const unsigned char *)0xF00000UL)    /* CGROM 先頭 (16x16 漢字) */
#endif
#define CGROM16 (CGBASE + 0x3A800)   /* 8x16 ANK フォント (0xF3A800) */

/* ペイン: Z-OS (zos.x) の下では、IOCS _B_CONSOL が自分のペインの位置と大きさを返す。vt_init がそれを読み、
 * 幅 96 桁未満か原点が左端でなければ、その枠の中だけに描く (TVRAM の原点をずらし、スクロールは TXRASCOPY
 * (全幅) を使わず枠の幅だけ写す)。普通の Human68k では _B_CONSOL は 96 桁の全面なので、今までどおり全面に描く。
 * ペインの左端は 4 桁の倍数 (スクロールを 32bit で写す。はみ出しは仕切りの桁で、どの行も同じ)。 */
static volatile unsigned char *vt_base = TVRAM_PHYS;
static int vt_pane;            /* 枠の中に描いている */
static int pane_x, pane_y, pane_w, pane_h;   /* 枠 (桁・行) */
#define TVRAM vt_base

/* ---- セル ---- */
/* cp: 描くコードポイント。0x20-0x7e は ANK、それ以外は Unicode (漢字など)。UTF-8 デコーダ
 * (utf8_feed) と描画層 (blit) を分けてあり、cp はコードポイントのまま持つ。
 * 全角 (East Asian Width = W/F) は 2 セルを占める: 左に F_WIDE、右に F_WIDECONT (継続・描かない)。 */
typedef struct {
  unsigned short cp;
  unsigned short at;    /* 属性語: fg<<12 | bg<<8 | flags。fg/bg は 0..15 のパレット番号 (bold / reverse 解決済み)、
                         * flags は F_UNDERLINE | F_SPECIAL | F_WIDE | F_WIDECONT | F_EMOJI */
} cell_t;
#define AT(fg, bg, fl) ((unsigned short)(((fg) << 12) | ((bg) << 8) | (fl)))
#define AT_FG(a) ((a) >> 12)
#define AT_BG(a) (((a) >> 8) & 15)
#define AT_FL(a) ((a) & 0xff)
/* 属性語の上位バイト (fg<<4|bg) と下位バイト (flags) をメモリから直接読む (68000 は 12 ビットのシフトが遅い) */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AT_HI(c) (((const unsigned char *)&(c)->at)[0])
#define AT_LO(c) (((const unsigned char *)&(c)->at)[1])
#else
#define AT_HI(c) (((const unsigned char *)&(c)->at)[1])
#define AT_LO(c) (((const unsigned char *)&(c)->at)[0])
#endif
/* セル 1 つ = 4 バイトを 32bit 1 語として比べる・写す (flush の走査・消去・スクロールが最頻なので速く)。
 * may_alias で cell_t を 32bit として読む (厳密な別名規則での最適化に崩されないように)。
 * 32bit の値の中の並びはエンディアンで違うので、値を組み立てるときはセルに書いてから読む (clr_cell)。 */
typedef u32 __attribute__((may_alias)) cell_u32;
#define CELL32(p) (*(cell_u32 *)(p))
#define CELLEQ(a, b) (CELL32(a) == CELL32(b))
#define F_UNDERLINE 0x01
#define F_SPECIAL   0x02   /* cp を DEC 罫線グリフ表で描く */
#define F_WIDE      0x04   /* 全角の左セル (16 ドット幅で描く) */
#define F_WIDECONT  0x08   /* 全角の右セル (描かない。左が両方を描く) */
#define F_EMOJI     0x10   /* cp は絵文字表のインデックス。16x16・16色で描く (F_WIDE と併用) */
#define BLANKCELL(x) ((x)->cp == 0x20 && AT_FL((x)->at) == 0)   /* 空白 (下線なし): 字形が 0 なので背景色だけで見える */

/* 行は行ポインタで引く (スクロールは行ポインタを回すだけで、セルを動かさない)。cur[r][c] / drawn[r][c] で読み書きする */
static cell_t store_main[MAXROWS][MAXCOLS];
static cell_t store_alt[MAXROWS][MAXCOLS];
static cell_t store_drawn[MAXROWS][MAXCOLS];
static cell_t *rows_main[MAXROWS], *rows_alt[MAXROWS];
static cell_t *drawn[MAXROWS];
static cell_t **cur = rows_main;   /* いま表示している面 */

static int COLS = MAXCOLS, ROWS = MAXROWS;

/* 行ごとの「論理画面を書き換えた」桁の範囲 [rlo, rhi]。flush はその範囲だけ drawn と比べる
 * (全 3072 セルの走査を避ける。Claude Code の入力行のように 1 行の数桁だけ変わる場面が多い)。rlo > rhi は無傷 */
static unsigned char rlo[MAXROWS], rhi[MAXROWS];
#define TOUCH(r) (rlo[r] = 0, rhi[r] = MAXCOLS - 1)
static void touch_span(int r, int c0, int c1)   /* c0..c1 (画面内に丸める) を書き換えた印 */
{
  if (c0 < 0) c0 = 0;
  if (c1 >= COLS) c1 = COLS - 1;
  if (c0 < rlo[r]) rlo[r] = (unsigned char)c0;
  if (c1 > rhi[r]) rhi[r] = (unsigned char)c1;
}
static void touch_rows(int r0, int r1) { int r; for (r = r0; r <= r1; r++) TOUCH(r); }

/* ---- カーソル・属性の状態 ---- */
static int cx, cy;            /* カーソル位置 (0 起算) */
static int pend_wrap;         /* DECAWM の遅延折り返し待ち */
static int top, bot;          /* スクロール領域 (0 起算・両端含む) */
static int c_fg = 7, c_bg = 0;
static int a_bold, a_uline, a_reverse;
/* 上の属性から作った、字を置くときの属性語と消去セル (属性が変わるたびに attr_update で作り直す。字ごとには計算しない) */
static unsigned short cur_at = AT(7, 0, 0);
static u32 clr_cell;          /* 空白 + c_fg/c_bg そのまま (bold/reverse は効かない) のセル */
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
static short param[MAXPARAM];         /* 値は MAXPVAL まで (16bit で足りる) */
static int nparam;
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

/* 仮想コンソール (-DVT_VCONS、下の「仮想コンソール」): 裏の端末は VRAM に描かない (解析と論理画面だけ) */
#ifdef VT_VCONS
static int vt_hidden;
#define VT_HIDDEN vt_hidden
static int vc_inited;   /* vt_init 済み (vt_fini まで)。この間の vt_init は端末の状態だけ初期化する */
int (*vt_keyhook)(int code);
#else
#define VT_HIDDEN 0
#endif

/* ---- 計測 (-DVT_PROF で数える。vtcat -b が表示する) ---- */
#ifdef VT_PROF
long vt_prof[8];   /* 0=blit したセル 1=scroll 呼び出し 2=TXRASCOPY 3=32bit 転送のライン 4=flush 5=vt_write 6=置いた字 7=行塗り */
#define PROF(i, n) (vt_prof[i] += (n))
#else
#define PROF(i, n)
#endif

/* ---- スーパーバイザ ---- */
/* 戻り値を _dos_super に渡して戻す。負なら元からスーパーバイザなので触らない。 */
#ifdef VT_ZOS
/* zos.x の中では IOCS のフック (スーパーバイザ、ときに DOS の CON の中) から呼ばれる。そこで DOS _SUPER を呼ぶと
 * DOS コールの入れ子になるので、すでにスーパーバイザなら呼ばない (68000 の move from SR はユーザでも使える) */
static int sup_enter(void)
{
  unsigned short sr;
  __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
  return (sr & 0x2000) ? -1 : _dos_super(0);
}
#else
static int sup_enter(void) { return _dos_super(0); }
#endif
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
  if ((c->at & F_SPECIAL) && c->cp >= 0x60 && c->cp <= 0x7f)
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

/* Unicode -> JIS の小さな直接写像キャッシュ (同じ字を何度も描くので、6879 件の二分探索を毎回しない) */
static unsigned short jis_of(unsigned long u)
{
  static unsigned short ck[64], cj[64];
  int h = (int)((u ^ (u >> 6)) & 63);
  if (u > 0xffff) return 0;
  if (ck[h] != u || !u) { ck[h] = (unsigned short)u; cj[h] = ucs_to_jis(u); }
  return cj[h];
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
#ifndef VT_EMOJI_SA
#define VT_EMOJI_SA 1   /* 1: 描く前に R21 を必ず 0 に書く / 2: 同時アクセスで 1 プレーンずつ選んで書く / 0: 旧版 */
#endif
static int sa_ok;              /* 定義は下の「同時アクセス」の節 */
static unsigned short sa_cur;
static void sa_mask(int m);
static void blit_emoji(int col, int row, int idx)
{
  const unsigned char *bm = emojitab[idx].bm;   /* 16 行 x 8 バイト, 2px/byte, 上位ニブル = 左 */
  unsigned char pl[16][4][2];
  int s, p, px;
  for (s = 0; s < 16; s++) {
    for (p = 0; p < 4; p++) { pl[s][p][0] = 0; pl[s][p][1] = 0; }
    for (px = 0; px < 16; px++) {
      unsigned char n = (px & 1) ? (bm[s * 8 + px / 2] & 0x0f) : (bm[s * 8 + px / 2] >> 4);
      int half = px >> 3, bit = 7 - (px & 7);
      for (p = 0; p < 4; p++) if ((n >> p) & 1) pl[s][p][half] |= (unsigned char)(1 << bit);
    }
  }
#if VT_EMOJI_SA == 2
  if (sa_ok) {   /* Z 実機で sa_off 後も同時アクセスが残る疑い: 切れていなくても正しい書き方 (プレーン 0 の番地に 1 枚ずつ) */
    for (p = 0; p < 4; p++) {
      sa_mask(1 << p);
      for (s = 0; s < 16; s++) {
        volatile unsigned char *v = TVRAM + (unsigned long)(row * CH + s) * VBYTES + col;
        v[0] = pl[s][p][0]; v[1] = pl[s][p][1];
      }
    }
    return;
  }
#elif VT_EMOJI_SA == 1
  CRTC_R21 = 0; sa_cur = 0;   /* 記録 (sa_cur) と実機の R21 がずれていても必ず切る */
#endif
  for (s = 0; s < 16; s++) {
    volatile unsigned char *v = TVRAM + (unsigned long)(row * CH + s) * VBYTES + col;
    for (p = 0; p < 4; p++) { v[p * PLANE] = pl[s][p][0]; v[1 + p * PLANE] = pl[s][p][1]; }
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

/* ---- テキスト VRAM の同時アクセス (CRTC R21 bit8 + bit4-7) ----
 * 同時アクセスを有効にすると、プレーン 0 の位置への書き込みが選んだプレーン全部に入る。1 セルの 4 プレーンは
 * 「0 で塗る / 0xff で塗る / グリフ / 反転グリフ」の 4 通りしかないので、同じ書き方のプレーンをまとめて 1 回で書ける
 * (白地に黒の字ならグリフ 1 回 + 0 塗り 1 回。旧版は 4 プレーンを 1 枚ずつ)。IPLROM も画面消去に使っている機能だが、
 * 念のため vt_init で画面外のラインに書いて確かめ (sa_probe)、効かなければプレーンごとに書く。
 * R21 を触るのは flush の中だけ: 読み書き両方のある cursor_xor・TXRASCOPY (IOCS が R21 の下位を書く) の前に 0 に戻す
 * (R21 は読まない。実機で読めるか確かめていないので。起動時は 0 で、IOCS の TXRASCOPY は呼ぶたびに自分で設定する)。 */
static int sa_ok;              /* 同時アクセスが使える (vt_init で確かめた) */
static unsigned short sa_cur; /* いま R21 に設定しているプレーン (0 = 同時アクセスを切ってある) */
static void sa_mask(int m)
{
  if (sa_cur == m) return;
  CRTC_R21 = (unsigned short)(0x100 | (m << 4));
  sa_cur = m;
}
static void sa_off(void) { if (sa_cur) { CRTC_R21 = 0; sa_cur = 0; } }
/* 画面外のライン 1020 で同時アクセスを試す: プレーン 1-3 を選んでプレーン 0 の位置に書き、プレーン 0 は変わらず
 * 1-3 に入れば使える (バイトと 32bit の両方)。書いた所は元に戻す。スーパーバイザで呼ぶ。 */
static int sa_probe(void)
{
  volatile unsigned char *v = TVRAM_PHYS + 1020UL * VBYTES + 100;
  unsigned char keep[4][4];
  int p, k, ok = 1;
  for (p = 0; p < 4; p++) for (k = 0; k < 4; k++) keep[p][k] = v[p * PLANE + k];
  for (p = 0; p < 4; p++) for (k = 0; k < 4; k++) v[p * PLANE + k] = 0;
  sa_mask(0xe);
  v[0] = 0x5a;
  *(volatile u32 *)(v + 0) = 0x12345678U;   /* v は 4 の倍数 (1020*128+100) */
  v[0] = 0xa5;
  sa_off();
  if (v[0] != 0 || v[1] != 0) ok = 0;
  for (p = 1; p < 4 && ok; p++)
    if (v[p * PLANE] != 0xa5 || v[p * PLANE + 1] != 0x34 || v[p * PLANE + 3] != 0x78) ok = 0;
  for (p = 0; p < 4; p++) for (k = 0; k < 4; k++) v[p * PLANE + k] = keep[p][k];
  return ok;
}
/* プレーンの組 m (bit p = プレーン p) に、v (プレーン 0 の位置) から X を書く。同時アクセスなら 1 回、無ければ 1 枚ずつ */
#define PASS(m, X) do { \
    if (sa_ok) { volatile unsigned char *v = v0; sa_mask(m); X; } \
    else { int p_; for (p_ = 0; p_ < 4; p_++) if (((m) >> p_) & 1) { volatile unsigned char *v = v0 + p_ * PLANE; X; } } \
  } while (0)

/* 1 セルを VRAM に描く (スーパーバイザ前提)。全角は左セルで 16 ドット幅 (2 バイト) を描く。
 * old はいま VRAM に描いてあるセル (drawn。NULL なら分からない): 前も今も同じ値で塗りつぶしのプレーンは書かない。
 * 空白 (下線なし) の字形は 0 なので、前景色に依らず背景色で塗るのと同じ (same_look が前景色だけ違う空白を描かずに
 * 済ませるので、drawn の空白の前景色は VRAM と関係ない。空白は前も今も背景色だけで見る)。 */
static void blit(int col, int row, const cell_t *c, const cell_t *old)
{
#ifdef VT_PROF_NOBLIT
  (void)col; (void)row; (void)c; (void)old; PROF(0, 1); return;   /* 計測: 描画を抜いた版 (解析・差分走査だけの時間) */
#endif
  int fg = AT_FG(c->at), bg = AT_BG(c->at), fl = AT_FL(c->at), wide = 0, m0, m1, mg, mi;
  const unsigned char *gp;
  unsigned char ul[32];
  volatile unsigned char *v0;
  if (fl & F_WIDECONT) return;   /* 右半分は左の F_WIDE セルが描く */
  PROF(0, 1);
  if (fl & F_EMOJI) { sa_off(); blit_emoji(col, row, c->cp); return; }   /* cp = 絵文字インデックス */
  if (fl & F_WIDE) {
    unsigned short jis = jis_of(c->cp);
    if (jis) { gp = kanji_glyph(jis); wide = 1; }
    else gp = glyph_of(c);   /* 変換できない全角は空白相当 (put_cp 側で '?' に落とすので通常来ない) */
  } else {
    gp = glyph_of(c);
    if (BLANKCELL(c)) fg = bg;
  }
  if (fl & F_UNDERLINE) {   /* 下線: 最下ラスタを埋めた写しを使う */
    int i, n = wide ? 32 : 16;
    for (i = 0; i < n; i++) ul[i] = gp[i];
    ul[n - 1] = 0xff; if (wide) ul[n - 2] = 0xff;
    gp = ul;
  }
  m1 = fg & bg;               /* 0xff で塗るプレーン */
  m0 = ~(fg | bg) & 15;       /* 0 で塗る */
  mg = fg & ~bg;              /* グリフ */
  mi = ~fg & bg & 15;         /* 反転グリフ */
  if (old && !wide && !(AT_FL(old->at) & (F_EMOJI | F_WIDECONT))) {   /* 前も同じ値の塗りつぶしなら書かない */
    int ofg = AT_FG(old->at), obg = AT_BG(old->at);
    if (BLANKCELL(old)) ofg = obg;
    m1 &= ~(ofg & obg);
    m0 &= ofg | obg;
  }
  v0 = TVRAM + (unsigned long)row * CH * VBYTES + col;
  if (m0) PASS(m0, { pl_fill(v, 0); if (wide) pl_fill(v + 1, 0); });
  if (m1) PASS(m1, { pl_fill(v, 0xff); if (wide) pl_fill(v + 1, 0xff); });
  if (mg) PASS(mg, { if (wide) { pl_copy2(v, gp); pl_copy2(v + 1, gp + 1); } else pl_copy1(v, gp); });
  if (mi) PASS(mi, { if (wide) { pl_inv2(v, gp); pl_inv2(v + 1, gp + 1); } else pl_inv1(v, gp); });
}

/* 属性語の上位バイト (fg<<4|bg) -> プレーンの書き方。bit0-3 グリフ / bit4-7 0 塗り / bit8-11 0xff 塗り / bit12-15 反転グリフ。
 * pm_old は「前の絵がそのプレーンで塗りつぶし」の印 (bit4-7 = 0 で塗ってある / bit8-11 = 0xff で塗ってある)。 */
static unsigned short pm_tab[512];   /* [0..255] = pm_new、[256..511] = pm_old (vt_blitrow は 1 本の表として引く) */
#define pm_new pm_tab
#define pm_old (pm_tab + 256)
static void pm_build(void)
{
  int a;
  for (a = 0; a < 256; a++) {
    int fg = a >> 4, bg = a & 15;
    pm_new[a] = (unsigned short)((fg & ~bg & 15) | ((~(fg | bg) & 15) << 4) | ((fg & bg) << 8) | ((~fg & bg & 15) << 12));
    pm_old[a] = (unsigned short)(((~(fg | bg) & 15) << 4) | ((fg & bg) << 8));
  }
}

/* 最頻の「下線なし・半角・罫線でない」セルを同時アクセスで描く (blit と同じ絵。v0 はそのセルのプレーン 0・最上ラスタ)。
 * 行の先頭アドレスを flush が 1 回だけ計算し、グリフは ASCII なら CGROM を直接引き、書き方は表で決める。 */
static void blit_plain(volatile unsigned char *v0, const cell_t *c, const cell_t *old)
{
#ifdef VT_PROF_NOBLIT
  (void)v0; (void)c; (void)old; PROF(0, 1); return;
#endif
  unsigned cp = c->cp, a = AT_HI(c), m;
  const unsigned char *gp;
  PROF(0, 1);
  if (cp == 0x20) { a &= 15; a |= a << 4; gp = CGROM16; }   /* 空白: 背景色で塗るだけ (fg = bg とみなす) */
  else if (cp - 0x21 < 0x5e && cp != 0x5c && cp != 0x7e) gp = CGROM16 + (cp << 4);
  else gp = glyph_of(c);
  m = pm_new[a];
  if (!(AT_LO(old) & (F_EMOJI | F_WIDECONT))) {   /* 前も同じ値の塗りつぶしなら書かない */
    unsigned oa = AT_HI(old);
    if (BLANKCELL(old)) { oa &= 15; oa |= oa << 4; }
    m &= ~pm_old[oa];
  }
  if (m & 0x000f) { volatile unsigned char *v = v0; sa_mask(m & 15); pl_copy1(v, gp); }
  if (m & 0x00f0) { volatile unsigned char *v = v0; sa_mask((m >> 4) & 15); pl_fill(v, 0); }
  if (m & 0x0f00) { volatile unsigned char *v = v0; sa_mask((m >> 8) & 15); pl_fill(v, 0xff); }
  if (m & 0xf000) { volatile unsigned char *v = v0; sa_mask(m >> 12); pl_inv1(v, gp); }
}

#if defined(__mc68000__) && !defined(VT_PROF_NOBLIT) && !defined(VT_NO_ASM)   /* -DVT_NO_ASM で C だけに */
/* blit_plain の 68000 手書き版 (同時アクセスが使えるときだけ)。C だとセルごとの判定・表引き・レジスタの出し入れに
 * グリフの 16 バイト転送の倍以上かかっていた。行の差分セル diff[0..nd-1] を前から描き、drawn も写す。
 * ASCII 以外・下線・全角・罫線・\・~ のセルに来たらそこで止め、処理した数を返す (そのセルは C の blit が描く)。
 *   引数 (スタック): rv=行のプレーン 0 の先頭, lrow, drow, diff, nd, pm=pm_tab, cg=CGROM16, r21=&CRTC_R21, cur=&sa_cur
 *   d0=処理数 d1=桁 d2=桁*4 d3=グリフの位置 d5=書き方 d6=いまの R21 プレーン d7=nd
 *   a0=rv a1=lrow a2=drow a3=diff a4=pm a5=グリフ/R21 a6=セルの VRAM */
int vt_blitrow(volatile unsigned char *rv, cell_t *lrow, cell_t *drow, const unsigned char *diff, int nd,
               const unsigned short *pm, const unsigned char *cg, volatile unsigned short *r21, unsigned short *cur);
#define BR_R21 /* d4 = プレーン (0 以外)。違えば R21 = 0x100 | d4<<4 */ \
  "	cmp.w	%d4,%d6\n	beq.s	1f\n	move.w	%d4,%d6\n	lsl.w	#4,%d4\n	ori.w	#0x100,%d4\n" \
  "	movea.l	76(%sp),%a5\n	move.w	%d4,(%a5)\n1:\n"
#define BR_G16(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define BR_CP(i) "	move.b	(%a5)+," #i "*128(%a6)\n"
#define BR_INV(i) "	move.b	(%a5)+,%d4\n	not.b	%d4\n	move.b	%d4," #i "*128(%a6)\n"
#define BR_FILL(i) "	move.b	%d4," #i "*128(%a6)\n"
__asm__(
"	.text\n	.even\n	.globl	vt_blitrow\n"
"vt_blitrow:\n"
"	movem.l	%d2-%d7/%a2-%a6,-(%sp)\n"
"	movea.l	48(%sp),%a0\n	movea.l	52(%sp),%a1\n	movea.l	56(%sp),%a2\n	movea.l	60(%sp),%a3\n"
"	move.l	64(%sp),%d7\n	movea.l	68(%sp),%a4\n	movea.l	80(%sp),%a5\n	move.w	(%a5),%d6\n"
"	moveq	#0,%d0\n	tst.w	%d7\n	beq	.Lbr_done\n"
".Lbr_loop:\n"
"	moveq	#0,%d1\n	move.b	(%a3,%d0.w),%d1\n	move.w	%d1,%d2\n	add.w	%d2,%d2\n	add.w	%d2,%d2\n"
"	move.b	3(%a1,%d2.w),%d3\n	andi.b	#0x1f,%d3\n	bne	.Lbr_done\n"          /* 下線・罫線・全角・絵文字 */
"	move.w	(%a1,%d2.w),%d3\n	moveq	#0,%d4\n	move.b	2(%a1,%d2.w),%d4\n"   /* d3=cp d4=fg<<4|bg */
"	cmpi.w	#0x20,%d3\n	bne.s	.Lbr_nb\n"
"	andi.w	#15,%d4\n	move.w	%d4,%d5\n	lsl.w	#4,%d5\n	or.w	%d5,%d4\n	bra.s	.Lbr_at\n"   /* 空白: fg = bg */
".Lbr_nb:\n"
"	cmpi.w	#0x21,%d3\n	bcs	.Lbr_done\n	cmpi.w	#0x7e,%d3\n	bcc	.Lbr_done\n	cmpi.w	#0x5c,%d3\n	beq	.Lbr_done\n"
".Lbr_at:\n"
"	lsl.w	#4,%d3\n	add.w	%d4,%d4\n	move.w	(%a4,%d4.w),%d5\n"           /* d3=グリフの位置 d5=pm_new[a] */
"	move.b	3(%a2,%d2.w),%d4\n	andi.b	#0x18,%d4\n	bne.s	.Lbr_pass\n"     /* 前が絵文字・全角の右半分: 分からない */
"	moveq	#0,%d4\n	move.b	2(%a2,%d2.w),%d4\n"
"	cmpi.w	#0x20,(%a2,%d2.w)\n	bne.s	.Lbr_on\n	tst.b	3(%a2,%d2.w)\n	bne.s	.Lbr_on\n"
"	andi.w	#15,%d4\n	move.w	%d4,%d1\n	lsl.w	#4,%d1\n	or.w	%d1,%d4\n	move.b	(%a3,%d0.w),%d1\n"   /* 前が空白: fg = bg */
".Lbr_on:\n"
"	add.w	%d4,%d4\n	addi.w	#512,%d4\n	move.w	(%a4,%d4.w),%d4\n	not.w	%d4\n	and.w	%d4,%d5\n"   /* d5 &= ~pm_old[oa] */
".Lbr_pass:\n"
"	lea	(%a0,%d1.w),%a6\n"
"	move.w	%d5,%d4\n	andi.w	#15,%d4\n	beq	.Lbr_p0\n" BR_R21
"	movea.l	72(%sp),%a5\n	adda.w	%d3,%a5\n" BR_G16(BR_CP)
".Lbr_p0:\n"
"	move.w	%d5,%d4\n	lsr.w	#4,%d4\n	andi.w	#15,%d4\n	beq	.Lbr_p1\n" BR_R21
"	moveq	#0,%d4\n" BR_G16(BR_FILL)
".Lbr_p1:\n"
"	move.w	%d5,%d4\n	lsr.w	#8,%d4\n	andi.w	#15,%d4\n	beq	.Lbr_pi\n" BR_R21
"	moveq	#-1,%d4\n" BR_G16(BR_FILL)
".Lbr_pi:\n"
"	move.w	%d5,%d4\n	lsr.w	#8,%d4\n	lsr.w	#4,%d4\n	beq	.Lbr_next\n" BR_R21
"	movea.l	72(%sp),%a5\n	adda.w	%d3,%a5\n" BR_G16(BR_INV)
".Lbr_next:\n"
"	move.l	(%a1,%d2.w),(%a2,%d2.w)\n"                                          /* drawn = 論理画面 */
"	addq.w	#1,%d0\n	cmp.w	%d7,%d0\n	bcs	.Lbr_loop\n"
".Lbr_done:\n"
"	movea.l	80(%sp),%a5\n	move.w	%d6,(%a5)\n"
"	movem.l	(%sp)+,%d2-%d7/%a2-%a6\n	rts\n");
#define BLIT_ASM 1
#endif

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
/* VRAM のライン [src_line, src_line+nlines) を dst_line へ動かす (4 プレーンとも)。
 * 実機ではライン番号が 4 の倍数なら IOCS TXRASCOPY を使う。1 ラスタ = 4 走査線 (行は 16 走査線 = 4 ラスタ)。
 * d1=(src<<8)|dst, d2=ラスタ数, d3=プレーンマスク(0xF)。実機で row5->row3 のコピーを確認済み。
 * 下方向 (dst>src) は重なるので後ろのラスタから: d3 の bit15 を立てると 1 ラスタごとに src/dst を 1 ずつ減らす
 * (IPLROM の _TXRASCPY = $FF781A を逆アセンブルして確認: tst.w d3 / bpl で増分 $0101 と $FEFF を切り替える)。
 * このとき d1 は最後 (いちばん下) のラスタ番号を渡す。それ以外は 32bit 転送 (画面の COLS バイトだけ)。 */
static void vram_move_lines(int dst_line, int src_line, int nlines)
{
  int p, y, k, nl = (COLS + 3) / 4;   /* 1 ラインのうち画面の COLS バイトだけ (128 のうち 96) 動かす */
#if !defined(VT_HOSTTEST) || defined(VT_SIM_TXRAS)
  if (!vt_pane && (dst_line & 3) == 0 && (src_line & 3) == 0 && (nlines & 3) == 0 && dst_line != src_line) {
    if (dst_line < src_line)
      _iocs_txrascpy(((src_line / 4) << 8) | (dst_line / 4), nlines / 4, 0x000f);
    else
      _iocs_txrascpy((((src_line + nlines) / 4 - 1) << 8) | ((dst_line + nlines) / 4 - 1), nlines / 4, (int)0x800f);
    PROF(2, 1);
    return;
  }
#endif
  PROF(3, nlines);   /* 32bit 転送で動かしたライン数 (TXRASCOPY の分は数えない) */
  for (p = 0; p < 4; p++) {
    volatile unsigned char *base = TVRAM + (unsigned long)p * PLANE;
    if (dst_line <= src_line) {
      for (y = 0; y < nlines; y++) {
        volatile u32 *d = (volatile u32 *)(base + (long)(dst_line + y) * VBYTES);
        volatile u32 *s = (volatile u32 *)(base + (long)(src_line + y) * VBYTES);
        for (k = nl; k >= 4; k -= 4) { *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; }
        while (k-- > 0) *d++ = *s++;
      }
    } else {   /* 下へ: 後ろのラインから */
      for (y = nlines - 1; y >= 0; y--) {
        volatile u32 *d = (volatile u32 *)(base + (long)(dst_line + y) * VBYTES);
        volatile u32 *s = (volatile u32 *)(base + (long)(src_line + y) * VBYTES);
        for (k = nl; k >= 4; k -= 4) { *d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++; }
        while (k-- > 0) *d++ = *s++;
      }
    }
  }
}

static void clear_cell(cell_t *c) { CELL32(c) = clr_cell; }

/* 行ポインタ表 t[a..b] (と行ごとの書き換え範囲) を k 行ぶん回す (正: 上へ、負: 下へ)。
 * % は 68000 では __modsi3 の呼び出しになるので、添字を 1 ずつ進めて折り返す */
static void rotate_rows(cell_t **t, unsigned char *lo, unsigned char *hi, int a, int b, int k)
{
  static cell_t *tmp[MAXROWS];
  static unsigned char tlo[MAXROWS], thi[MAXROWS];
  int i, j, n = b - a + 1;
  if (k < 0) k += n;
  if (k >= n) k -= n;
  if (k == 1) {   /* 最頻 (1 行送り): 1 つずつ詰める */
    cell_t *t0 = t[a];
    unsigned char l0 = lo ? lo[a] : 0, h0 = lo ? hi[a] : 0;
    for (i = a; i < b; i++) t[i] = t[i + 1];
    t[b] = t0;
    if (lo) { for (i = a; i < b; i++) { lo[i] = lo[i + 1]; hi[i] = hi[i + 1]; } lo[b] = l0; hi[b] = h0; }
    return;
  }
  if (k == n - 1) {   /* 1 行戻し (less の上スクロール・RI) */
    cell_t *t0 = t[b];
    unsigned char l0 = lo ? lo[b] : 0, h0 = lo ? hi[b] : 0;
    for (i = b; i > a; i--) t[i] = t[i - 1];
    t[a] = t0;
    if (lo) { for (i = b; i > a; i--) { lo[i] = lo[i - 1]; hi[i] = hi[i - 1]; } lo[a] = l0; hi[a] = h0; }
    return;
  }
  for (i = 0, j = a + k; i < n; i++) {
    tmp[i] = t[j];
    if (lo) { tlo[i] = lo[j]; thi[i] = hi[j]; }
    if (++j > b) j = a;
  }
  for (i = 0; i < n; i++) {
    t[a + i] = tmp[i];
    if (lo) { lo[a + i] = tlo[i]; hi[a + i] = thi[i]; }
  }
}

/* スクロール領域 [top,bot] を n 行上へ (正: 上スクロール / 負: 下スクロール) */
static void scroll(int n)
{
  int r, col, keep, m;
  static u32 save[MAXROWS][MAXCOLS];
  if (n == 0) return;
  PROF(1, 1);
  if (n > bot - top + 1) n = bot - top + 1;
  if (-n > bot - top + 1) n = -(bot - top + 1);
  m = n > 0 ? n : -n;
  keep = (bot - top + 1) - m;
  /* 空く行 (上スクロールなら下端の m 行、下スクロールなら上端の m 行) の VRAM には、ずらす前のその行の絵が
   * 残る (VRAM の移動はそこを触らない)。drawn もその絵にしておけば、flush は前の絵と違うセルだけ描けばよい
   * (less の 1 行送りなど)。行ポインタを回すと drawn のその位置に別の行が来るので、絵を控えて戻す。 */
  if (VT_HIDDEN) {   /* 裏の端末: 論理画面だけ回す (drawn と VRAM は表の端末のもの) */
    rotate_rows(cur, rlo, rhi, top, bot, n);
    for (r = 0; r < m; r++) {
      int rr = n > 0 ? bot - m + 1 + r : top + r;
      cell_u32 *l = (cell_u32 *)cur[rr];
      for (col = 0; col < COLS; col++) l[col] = clr_cell;
      TOUCH(rr);
    }
    return;
  }
  for (r = 0; r < m; r++) {
    int rr = n > 0 ? bot - m + 1 + r : top + r;
    const cell_u32 *d = (const cell_u32 *)drawn[rr];
    for (col = 0; col < COLS; col++) save[r][col] = d[col];
  }
  /* 残る行は cur・drawn・VRAM がそろって動くので、書き換え範囲も一緒に回せば描き直さずに済む */
  rotate_rows(cur, rlo, rhi, top, bot, n);
  rotate_rows(drawn, 0, 0, top, bot, n);
  if (keep > 0) {
    if (n > 0) vram_move_lines(top * CH, (top + n) * CH, keep * CH);   /* VRAM を上へ */
    else vram_move_lines((top + m) * CH, top * CH, keep * CH);         /* 下へ */
  }
  for (r = 0; r < m; r++) {
    int rr = n > 0 ? bot - m + 1 + r : top + r;
    cell_u32 *l = (cell_u32 *)cur[rr], *d = (cell_u32 *)drawn[rr];
    u32 z = clr_cell;
    for (col = 0; col < COLS; col++) { l[col] = z; d[col] = save[r][col]; }
    TOUCH(rr);
  }
}

/* ================= flush: logical と drawn の差分を描く ================= */
/* 見た目が同じか: 同じ字・属性。空白 (下線なし) どうしなら前景色は見えないので背景だけ比べる */
static int same_look(const cell_t *a, const cell_t *b)
{
  if (a->cp != b->cp || ((a->at ^ b->at) & 0x0fff)) return 0;
  return a->at == b->at || BLANKCELL(a);
}

/* 1 行 (COLS 桁 x 16 ライン x 4 プレーン) を背景色で塗る。空白だけの行 (消去・スクロールで空いた行) 用。
 * 同時アクセスなら 0 のプレーンと 1 のプレーンの 2 回 (背景が黒なら 1 回) で済む */
static void row_fill1(volatile unsigned char *base, unsigned char b)
{
  int s, c;
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
static void row_fill(int row, int bg)
{
  volatile unsigned char *v0 = TVRAM + (unsigned long)row * CH * VBYTES;
  if (bg & 15) PASS(bg & 15, row_fill1(v, 0xff));
  if (~bg & 15) PASS(~bg & 15, row_fill1(v, 0));
}

/* vt_write に展開させない (巨大な関数でレジスタが足りず、描画の内側のループで変数がスタックに溢れる) */
#define NOINLINE __attribute__((noinline))
static NOINLINE void flush(void)
{
  int r, c, s, i;
  static unsigned char diff[MAXCOLS];
  if (VT_HIDDEN) return;   /* 裏の端末は描かない (表に出すとき vt_con_visible が全面を描く) */
  PROF(4, 1);
  s = sup_enter();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }   /* 先にカーソルを消す */
  for (r = 0; r < ROWS; r++) {
    cell_t *lrow, *drow;
    int nd = 0, lo = rlo[r], hi = rhi[r];
    if (lo > hi) continue;
    rlo[r] = MAXCOLS; rhi[r] = 0;
    if (hi >= COLS) hi = COLS - 1;
    lrow = cur[r]; drow = drawn[r];
    /* 1 回目: 書き換え範囲で描き直しの要るセルを拾う (同じなら飛ばす。空白で前景色だけ違うものは控えだけ直す) */
#if defined(__mc68000__) && !defined(VT_NO_ASM)
    {   /* 同じセルを飛ばすのは cmpm.l / dbne の 2 命令のループで (C の比較ループの半分以下のクロック) */
      const cell_u32 *l = (const cell_u32 *)&lrow[lo], *d = (const cell_u32 *)&drow[lo];
      short cnt = (short)(hi - lo);   /* dbne の回数 (残りのセル数 - 1) */
      for (;;) {
        unsigned char ne;
        __asm__ volatile ("1:\tcmpm.l\t(%0)+,(%1)+\n\tdbne\t%2,1b\n\tsne\t%3"
                          : "+a"(l), "+a"(d), "+d"(cnt), "=d"(ne) : : "cc", "memory");
        if (!ne) break;   /* 最後まで同じ */
        c = (int)((const cell_t *)l - lrow) - 1;   /* 食い違ったセル (両方とも 1 つ先まで進んでいる) */
        if (same_look(&lrow[c], &drow[c])) drow[c] = lrow[c];
        else diff[nd++] = (unsigned char)c;
        if (cnt == 0) break;   /* 食い違いで抜けたとき cnt はその先の残り数 */
        cnt--;
      }
    }
#else
    for (c = lo; c <= hi; c++) {
      if (CELLEQ(&lrow[c], &drow[c])) continue;
      if (same_look(&lrow[c], &drow[c])) drow[c] = lrow[c];
      else diff[nd++] = (unsigned char)c;
    }
#endif
    if (nd == 0) continue;
    if (nd > 8) {   /* 空白だけの行 (消去・スクロールで空いた行) で描き直しが多い: 行ごと塗る */
      int bg0 = AT_BG(lrow[0].at), blank = 1;
      unsigned short bgat = AT(0, bg0, 0);
      for (c = 0; c < COLS; c++) if (lrow[c].cp != 0x20 || (lrow[c].at & 0x0fff) != bgat) { blank = 0; break; }
      if (blank) {
        row_fill(r, bg0);
        PROF(7, 1);
        for (c = 0; c < COLS; c++) drow[c] = lrow[c];
        continue;
      }
    }
    {
      volatile unsigned char *rv = TVRAM + (unsigned long)r * CH * VBYTES;
      for (i = 0; i < nd; i++) {
#ifdef BLIT_ASM
        if (sa_ok && !(AT_LO(&lrow[diff[i]]) & 0x1f)) {   /* ASCII・下線なしのセルは手書きの版でまとめて (止まったセルだけ下の C で) */
          int k = vt_blitrow(rv, lrow, drow, diff + i, nd - i, pm_tab, CGROM16, &CRTC_R21, &sa_cur);
          PROF(0, k);
          i += k;
          if (i >= nd) break;
        }
#endif
        c = diff[i];
        if (!sa_ok || (AT_LO(&lrow[c]) & (F_UNDERLINE | F_SPECIAL | F_WIDE | F_WIDECONT | F_EMOJI))) blit(c, r, &lrow[c], &drow[c]);
        else blit_plain(rv + c, &lrow[c], &drow[c]);
        drow[c] = lrow[c];
      }
    }
  }
  sa_off();
  if (cur_vis) { cursor_xor(cx, cy); csr_shown = 1; csr_x = cx; csr_y = cy; }
  sup_leave(s);
}

/* ================= 文字の書き込み ================= */
/* 属性 (c_fg/c_bg/bold/reverse/underline) が変わったら呼ぶ: 字を置く属性語 cur_at と消去セル clr_cell を作り直す */
static void attr_update(void)
{
  int fg = c_fg, bg = c_bg, t;
  cell_t z;
  if (a_bold && fg < 8) fg += 8;
  if (a_reverse) { t = fg; fg = bg; bg = t; }
  cur_at = AT(fg, bg, a_uline ? F_UNDERLINE : 0);
  z.cp = 0x20; z.at = AT(c_fg, c_bg, 0);
  clr_cell = CELL32(&z);
}

/* 書き込みで全角ペアを割る場合、相方のセルを空白に戻す (孤立した半分を残さない)。
 * 相方が本当に対の片割れ (F_WIDE の右が F_WIDECONT / F_WIDECONT の左が F_WIDE) のときだけ消す
 * (ECH などで残った片割れの隣にある普通の字は消さない)。書く前の状態で、書くセルの両端について呼ぶ。 */
static void break_pair(int r, int c)
{
  cell_t *row = cur[r];
  if (c < 0 || c >= COLS) return;
  if ((row[c].at & F_WIDE) && c + 1 < COLS && (row[c + 1].at & F_WIDECONT)) { clear_cell(&row[c + 1]); touch_span(r, c + 1, c + 1); }
  else if ((row[c].at & F_WIDECONT) && c > 0 && (row[c - 1].at & F_WIDE)) { clear_cell(&row[c - 1]); touch_span(r, c - 1, c - 1); }
}

/* 遅延折り返し・自動改行: 行頭へ戻り、下端ならスクロール */
static void wrap_line(void)
{
  cx = 0;
  if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++;
  pend_wrap = 0;
}

/* コードポイント/インデックスを現在位置に置く。width=1 (半角) / 2 (全角)。
 * extra は追加フラグ (F_SPECIAL = DEC 罫線 / F_EMOJI = cp は絵文字インデックス)。
 * 連なり (put_run) に乗らない字だけがここを通る。 */
static void put_cell(unsigned short cp, int extra, int width)
{
  cell_t *c;
  PROF(6, 1);
  if (pend_wrap) wrap_line();   /* 前回末尾に置いた後の遅延折り返し */
  if (width == 2 && COLS < 2) { cp = '?'; extra = 0; width = 1; }   /* 1 桁幅の画面に全角は置けない */
  if (width == 2 && cx == COLS - 1) {   /* 全角が右端に 1 桁しか入らない: 空けて次行へ */
    break_pair(cy, cx); clear_cell(&cur[cy][cx]); touch_span(cy, cx, cx);
    wrap_line();
  }
  break_pair(cy, cx);
  if (width == 2) break_pair(cy, cx + 1);
  touch_span(cy, cx, cx + width - 1);
  c = &cur[cy][cx];
  c->cp = cp;
  c->at = cur_at | extra;
  if (width == 2) {
    c->at |= F_WIDE;
    c[1].cp = 0x20; c[1].at = cur_at | F_WIDECONT;
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
  cell_u32 *l;
  u32 z = clr_cell;
  int k;
  if (c1 >= COLS) c1 = COLS - 1;
  if (c0 > c1) return;
  touch_span(r, c0, c1);
  l = (cell_u32 *)&cur[r][c0];
  for (k = c1 - c0; k >= 0; k--) *l++ = z;
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
static int nearest16_calc(int r, int g, int b)
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
/* 同じ色が何度も来る (Claude Code は字の色を 38;2;R;G;B で毎回送る) ので、割り算の要る計算を小さな表で省く */
static int nearest16(int r, int g, int b)
{
  static u32 ck[16];
  static unsigned char cv[16];
  u32 key = ((u32)r << 16) | ((u32)g << 8) | (u32)b | 0x80000000U;
  int h = (r ^ (g >> 2) ^ (b >> 4)) & 15;
  if (ck[h] != key) { ck[h] = key; cv[h] = (unsigned char)nearest16_calc(r, g, b); }
  return cv[h];
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
  if (nparam == 0) { a_bold = a_uline = a_reverse = 0; c_fg = 7; c_bg = 0; attr_update(); return; }
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
  attr_update();
}

/* ================= モード設定 ================= */
static void switch_screen(int to_alt, int clear_on_enter)
{
  touch_rows(0, ROWS - 1);
  if (to_alt && !alt_active) {
    cur = rows_alt; alt_active = 1;
    if (clear_on_enter) { int r; for (r = 0; r < ROWS; r++) erase_cells(r, 0, COLS - 1); }
  } else if (!to_alt && alt_active) {
    cur = rows_main; alt_active = 0;
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
          else { switch_screen(0, 0); cx = sv_cx; cy = sv_cy; c_fg = sv_fg; c_bg = sv_bg; attr_update(); }
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
      touch_span(cy, cx, COLS - 1);
      for (c = COLS - 1; c >= cx + cnt; c--) cur[cy][c] = cur[cy][c - cnt];
      erase_cells(cy, cx, cx + cnt - 1); pend_wrap = 0; break; }
    case 'P': { int cnt = p_def(0, 1), c;    /* DCH */
      if (cnt > COLS - cx) cnt = COLS - cx;
      touch_span(cy, cx, COLS - 1);
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
static void csi_begin(void) { csi_priv = 0; csi_inter = 0; nparam = 0; param[0] = 0; state = S_CSI; }
static void do_esc(unsigned char b)
{
  switch (b) {
    case '[': csi_begin(); return;
    case ']': case 'P': case '^': case '_': case 'X':
      str_kind = b; str_esc = 0; str_len = 0; state = S_STR; return;
    case '(': cs_which = 0; state = S_CHARSET; return;
    case ')': cs_which = 1; state = S_CHARSET; return;
    case '7': sv_cx = cx; sv_cy = cy; sv_fg = c_fg; sv_bg = c_bg; sv_bold = a_bold; sv_ul = a_uline;
              sv_rev = a_reverse; sv_sp0 = g_special[0]; sv_sp1 = g_special[1]; sv_gl = gl; break;    /* DECSC */
    case '8': cx = sv_cx; cy = sv_cy; c_fg = sv_fg; c_bg = sv_bg; a_bold = sv_bold; a_uline = sv_ul;
              a_reverse = sv_rev; g_special[0] = sv_sp0; g_special[1] = sv_sp1; gl = sv_gl; clamp_cursor(); attr_update(); break; /* DECRC */
    case '=': break;   /* DECKPAM (キーパッドはキー変換側で扱わないので無視) */
    case '>': break;   /* DECKPNM */
    case 'D': if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++; pend_wrap = 0; break;   /* IND */
    case 'M': if (cy == top) scroll(-1); else if (cy > 0) cy--; pend_wrap = 0; break;         /* RI */
    case 'E': cx = 0; if (cy == bot) scroll(1); else if (cy < ROWS - 1) cy++; pend_wrap = 0; break; /* NEL */
    case 'H': tabstop[cx] = 1; break;   /* HTS */
    case 'c': /* RIS: 全リセット */
      { int r; cx = cy = 0; top = 0; bot = ROWS - 1; c_fg = 7; c_bg = 0;
        a_bold = a_uline = a_reverse = 0; decawm = 1; decckm = 0; cur_vis = 1; pend_wrap = 0;
        g_special[0] = g_special[1] = 0; gl = 0; switch_screen(0, 0);
        attr_update();
        for (r = 0; r < ROWS; r++) erase_cells(r, 0, COLS - 1);
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

/* 確定コードポイント (ASCII 以外) をどう置くかを決める。結果は K_* と、置くセルの cp (*code) と追加フラグ (*fl)。
 * 端末の状態に依らないので、put_run はコードポイントごとに覚えておいて表を引き直さない (cpk_*)。 */
enum {
  K_DROP,   /* 幅 0: 捨てる (桁を進めない) */
  K_N1,     /* 半角 1 セル */
  K_W2,     /* 全角 1 字 (F_WIDE + F_WIDECONT の 2 セル) */
  K_N2,     /* 代用字 + 空白の半角 2 セル (字形の無い全角の記号) */
  K_QQ,     /* '??' (変換できない全角) */
  K_SLOW    /* put_cp に任せる (C1 制御・ASCII に化けた不正な UTF-8 など) */
};
static int cp_kind(long cp, unsigned short *code, unsigned char *fl)
{
  unsigned char a, m;
  *fl = 0; *code = '?';
  if (cp < 0xa0) return K_SLOW;
  if ((cp >= 0x3040 && cp <= 0x9fff && cp != 0x3099 && cp != 0x309a) || (cp >= 0xff01 && cp <= 0xff60)) {
    /* かな・漢字・全角英数 (最頻の全角) は表を引かずに全角と決める */
    if (jis_of((unsigned long)cp)) { *code = (unsigned short)cp; return K_W2; }
    return K_QQ;
  }
  /* 罫線・ブロック要素 (TUI で最頻の非 ASCII) は幅 0 の表を引く前に片付ける */
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
    if (m) { *code = m; *fl = F_SPECIAL; }
    else *code = '+';
    return K_N1;
  }
  if (cp >= 0x2580 && cp <= 0x259f) { *code = (unsigned short)cp; return K_N1; }   /* ブロック要素 (自前グリフ) */
  /* 幅 0 の文字は捨てる (桁を進めない): 異体字セレクタ、ZWJ/ZWSP・方向制御、結合文字 (widthtab.h)。
   * 前の文字との合成はしない。絵文字の肌色修飾 (U+1F3FB-1F3FF) は wcwidth では 2 だが、前の絵文字に
   * 重なって 1 字に見えるのが普通なので捨てる。 */
  if ((cp >= 0x1f3fb && cp <= 0x1f3ff) || in_ranges(zerotab, (int)ZEROTAB_N, (unsigned long)cp)) return K_DROP;
  if (cp >= EMOJITAB_MIN) { int ei = emoji_index((unsigned long)cp);   /* 絵文字: 16x16・16色で全角 2 セル (表の最小未満は引かない) */
    if (ei >= 0) { *code = (unsigned short)ei; *fl = F_EMOJI; return K_W2; } }
  if ((cp >= 0xff61 && cp <= 0xff9f) || cp == 0x00a5 || cp == 0x203e) { *code = (unsigned short)cp; return K_N1; }   /* CGROM の JIS X 0201 にある半角 */
  if (is_wide(cp)) {
    if (jis_of((unsigned long)cp)) { *code = (unsigned short)cp; return K_W2; }   /* 全角: CGROM 16x16 漢字 */
    if (fallback(cp, &a, &m)) { if (m) { *code = m; *fl = F_SPECIAL; } else *code = a; return K_N2; }
    return K_QQ;   /* 変換不能な全角は '??' で桁を保つ */
  }
  if (fallback(cp, &a, &m)) { if (m) { *code = m; *fl = F_SPECIAL; } else *code = a; }
  return K_N1;   /* 代用字、無ければ '?' (半角の未対応文字) */
}

/* 確定コードポイントを画面に置く (1 字ずつの遅い道。分割された UTF-8 や右端で折り返す字など) */
static void put_cp(long cp)
{
  unsigned short code;
  unsigned char fl;
  if (cp >= 0x20 && cp < 0x7f) { put_cell((unsigned short)cp, g_special[gl] ? F_SPECIAL : 0, 1); return; }   /* ASCII */
  if (cp == 0x7f) return;
  if (cp < 0xa0) { put_cell('?', 0, 1); return; }   /* C1 制御・不正な UTF-8 (U+FFFD 以外で 0xa0 未満になったもの) */
  switch (cp_kind(cp, &code, &fl)) {
    case K_DROP: return;
    case K_N1: put_cell(code, fl, 1); return;
    case K_W2: put_cell(code, fl, 2); return;
    case K_N2: put_cell(code, fl, 1); put_cell(' ', 0, 1); return;
    default: put_cell('?', 0, 1); put_cell('?', 0, 1); return;
  }
}

/* cp_kind の結果の直接写像キャッシュ (BMP のみ。key 0 は空き) */
#define CPK_N 256
static unsigned short cpk_key[CPK_N], cpk_code[CPK_N];
static unsigned char cpk_fl[CPK_N], cpk_kind[CPK_N];

/* 字の連なりを現在位置からまとめて置く (put_cell を 1 字ずつ呼ぶのと同じ結果になる)。p から n バイト。
 * 印字 ASCII と、バッファ内で完結した 2・3 バイトの UTF-8 (cp_kind が K_DROP/K_N1/K_W2/K_N2/K_QQ のもの) を
 * 読む。右端に届いたら DECAWM に従って次の行へ続ける。置いたバイト数を返す (0 なら 1 字目から遅い道へ)。
 *   全角ペアの割れは行ごとの書いた範囲の両端だけ見ればよい (中は全部上書きされる)。左端のセルが対の右半分なら
 *   左隣を、右端のセルが対の左半分だったなら右隣を消す (break_pair と同じ規則)。
 *   右端に 1 桁しか残らない全角 (と 2 セルの代用) は遅い道 (put_cell の「空けて次行へ」) に任せる。
 *   右端の後は pend_wrap: cx は最後の字の左端 (半角なら COLS-1、全角なら COLS-2) のまま。 */
static NOINLINE int put_run(const unsigned char *p, int n)
{
  const unsigned char *s = p, *e = p + n;
  cell_t *row = cur[cy], *c;
  unsigned short at = cur_at, aat = cur_at | (g_special[gl] ? F_SPECIAL : 0);
  unsigned short olast = 0;          /* 最後に上書きしたセルの元の属性語 */
  int x = cx, lo = MAXCOLS, hi = -1, lastw;
  if (pend_wrap) { lastw = COLS - cx; x = COLS; } else lastw = 1;
  while (s < e) {
    unsigned short code;
    unsigned char fl, kind;
    int len, need;
    unsigned b = *s;
    if (b - 0x20 < 0x5f) { code = (unsigned short)b; kind = K_N1; len = 1; fl = 0xff; }   /* fl=0xff: ASCII (aat を使う) */
    else {
      unsigned long u;
      int h;
      if (b >= 0xc2 && b <= 0xdf) {
        if (s + 1 >= e || (s[1] & 0xc0) != 0x80) break;
        u = ((b & 0x1f) << 6) | (s[1] & 0x3f); len = 2;
      } else if (b >= 0xe0 && b <= 0xef) {
        if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) break;
        u = ((unsigned long)(b & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); len = 3;
      } else break;
      if (u < 0xa0) break;
      h = (int)((u ^ (u >> 7)) & (CPK_N - 1));
      if (cpk_key[h] != u) { cpk_kind[h] = (unsigned char)cp_kind((long)u, &cpk_code[h], &cpk_fl[h]); cpk_key[h] = (unsigned short)u; }
      kind = cpk_kind[h]; code = cpk_code[h]; fl = cpk_fl[h];
      if (kind == K_DROP) { s += len; continue; }   /* 幅 0: 何も置かず、折り返しも起こさない */
      if (kind == K_SLOW) break;
    }
    need = kind == K_N1 ? 1 : 2;
    if (x >= COLS) {   /* 右端の後: 折り返し (DECAWM が切れていれば右端に重ね書きなので遅い道へ) */
      if (!decawm) break;
      if (hi >= lo) {
        if ((olast & F_WIDE) && hi + 1 < COLS && (row[hi + 1].at & F_WIDECONT)) { clear_cell(&row[hi + 1]); hi++; }
        touch_span(cy, lo, hi);
      }
      wrap_line();
      row = cur[cy]; x = 0; lo = MAXCOLS; hi = -1;
    }
    if (x + need > COLS) break;
    if (lo > x) {   /* この行で最初に書くセル: 対の右半分なら左隣を消す */
      lo = x;
      if ((row[x].at & F_WIDECONT) && x > 0 && (row[x - 1].at & F_WIDE)) { clear_cell(&row[x - 1]); lo = x - 1; }
    }
    c = &row[x];
    if (fl == 0xff) {   /* 印字 ASCII: 行末か ASCII 以外まで続けて置く (最頻なので専用の短いループ) */
      const unsigned char *lim = s + (COLS - x), *s0 = s;
      if (lim > e) lim = e;
      do {
        olast = c->at; c->cp = (unsigned short)b; c->at = aat; c++;
        if (++s >= lim) break;
        b = *s;
      } while (b - 0x20 < 0x5f);
      x += (int)(s - s0); hi = x - 1; lastw = 1;
      PROF(6, s - s0);
      continue;
    }
    if (kind == K_N1) {
      olast = c->at; c->cp = code; c->at = at | fl; lastw = 1;
    } else if (kind == K_W2) {
      olast = c[1].at; c->cp = code; c->at = at | fl | F_WIDE; c[1].cp = 0x20; c[1].at = at | F_WIDECONT; lastw = 2;
    } else {   /* K_N2 / K_QQ: 半角 2 セル */
      olast = c[1].at;
      if (kind == K_N2) { c->cp = code; c->at = at | fl; c[1].cp = ' '; }
      else { c->cp = '?'; c->at = at; c[1].cp = '?'; }
      c[1].at = at; lastw = 1;
    }
    x += need; hi = x - 1;
    s += len;
    PROF(6, 1);
  }
  if (hi >= lo) {   /* 最後に書いたセルが対の左半分だったなら右隣を消す */
    if ((olast & F_WIDE) && hi + 1 < COLS && (row[hi + 1].at & F_WIDECONT)) { clear_cell(&row[hi + 1]); hi++; }
    touch_span(cy, lo, hi);
    if (x >= COLS) { cx = COLS - lastw; pend_wrap = decawm; }
    else { cx = x; pend_wrap = 0; }
  }   /* 幅 0 の字だけなら cx・pend_wrap はそのまま */
  return (int)(s - p);
}

/* CSI の引数・中間・終端バイトを続けて読む (1 バイトごとに vt_write の switch を回らない。Claude Code の出力は
 * 6 割強が CSI)。p[0] から n バイトのうち使った数 (1 以上) を返す。終端・ESC・取り消しで止まる。 */
static int csi_feed(const unsigned char *p, int n, int fresh)
{
  int k = 0, np, priv, inter;   /* 前置・中間・引数の数は途中では局所変数で持つ (大域変数の読み書きは 68000 で重い) */
  if (fresh) { np = 0; priv = 0; inter = 0; param[0] = 0; state = S_CSI; }   /* ESC [ の直後から */
  else { np = nparam; priv = csi_priv; inter = csi_inter; }
  while (k < n) {
    unsigned b = p[k++], d = b - '0';
    if (d <= 9) {   /* 値は MAXPVAL で頭打ち、MAXPARAM 個を越えた分は捨てる。数字が続く間は局所変数で積む */
      if (np == 0) np = 1;
      if (np <= MAXPARAM) {
        short *pv = &param[np - 1];
        unsigned v = (unsigned short)*pv;
        for (;;) {
          v = v >= 1000 ? MAXPVAL : v * 10 + d;   /* 1000 以上なら 10 倍で必ず MAXPVAL を越える */
          if (k >= n || (d = p[k] - '0') > 9) break;
          k++;
        }
        *pv = (short)v;
      }
      continue;
    }
    if (b == ';' || b == ':') {   /* ':' (38:5:N などのサブパラメータ) も区切りとして読む */
      if (np == 0) np = 1;        /* 先頭が空 (CSI ;5H) */
      if (np < MAXPARAM) param[np] = 0;
      if (np <= MAXPARAM) np++;
      continue;
    }
    if (b >= 0x40 && b <= 0x7e) {
      nparam = np > MAXPARAM ? MAXPARAM : np; csi_priv = priv; csi_inter = inter;
      state = S_GROUND; do_csi((unsigned char)b); return k;
    }
    if (b == '?' || b == '>' || b == '<' || b == '=') priv = b;
    else if (b >= 0x20 && b <= 0x2f) inter = b;
    else if (b == 0x1b) { state = S_ESC; return k; }
    else if (b < 0x20) ground_ctrl((unsigned char)b);   /* CSI の途中の制御文字はその場で実行 (VT100 の動き) */
    else { state = S_GROUND; return k; }
  }
  nparam = np; csi_priv = priv; csi_inter = inter;   /* 続きは次の vt_write で */
  return k;
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
  for (i = 0; i < n; ) {
    unsigned char b = p[i];
    if (state == S_GROUND) {
      /* 最頻: 行の中に収まる印字 ASCII の連なり (Claude Code は CSI の間に数字ずつ置くので、put_run を呼ぶ手間も惜しい)。
       * put_run の ASCII と同じ処理: 行末で止め (次の字で put_run が折り返す)、両端だけ全角ペアの割れを見る。 */
      if ((unsigned)(b - 0x20) < 0x5f && !pend_wrap && !g_special[gl]) {
        cell_t *row = cur[cy], *c;
        const unsigned char *q = p + i;
        unsigned short at = cur_at, olast;
        int x = cx, k = 0, lim = n - i, lo = x, hi;
        if (lim > COLS - x) lim = COLS - x;
        c = &row[x];
        if ((c->at & F_WIDECONT) && x > 0 && (c[-1].at & F_WIDE)) { clear_cell(&c[-1]); lo--; }
        do { olast = c->at; c->cp = b; c->at = at; c++; } while (++k < lim && (unsigned)((b = q[k]) - 0x20) < 0x5f);
        hi = x + k - 1;
        if ((olast & F_WIDE) && hi + 1 < COLS && (c->at & F_WIDECONT)) { clear_cell(c); hi++; }
        touch_span(cy, lo, hi);
        PROF(6, k);
        x += k;
        if (x >= COLS) { cx = COLS - 1; pend_wrap = decawm; } else cx = x;
        u_need = 0; i += k;
        continue;
      }
      /* 字の連なり (折り返し・UTF-8 を含む)。UTF-8 の途中 (u_need) は ASCII 以外を遅い道で */
      if (b >= 0x20 && b != 0x7f && (b < 0x80 || !u_need)) {
        int k = put_run(p + i, n - i);
        if (k) { u_need = 0; i += k; continue; }
      }
      i++;
      if (b == 0x1b) {
        u_need = 0;
        if (i < n && p[i] == '[') { i++; i += csi_feed(p + i, n - i, 1); }   /* ESC [ に続く引数をそのまま読む */
        else state = S_ESC;
      }
      else if (b < 0x20) { ground_ctrl(b); u_need = 0; }
      else { long cp = utf8_feed(b); if (cp >= 0) put_cp(cp); }
      continue;
    }
    i++;
    switch (state) {
      case S_ESC: do_esc(b); if (state == S_CSI) i += csi_feed(p + i, n - i, 0); break;   /* ESC [ に続く引数をそのまま読む */
      case S_ESC_INT: /* '#' の次 */
        if (b == '8') { int r, c; for (r = 0; r < ROWS; r++) for (c = 0; c < COLS; c++) { cur[r][c].cp = 'E'; cur[r][c].at = cur_at; } touch_rows(0, ROWS - 1); }
        state = S_GROUND; break;
      case S_ESC_SKIP:   /* 中間バイトは続けて読み、終端 (0x30-0x7e) で終わり */
        if (b == 0x1b) state = S_ESC;
        else if (b < 0x20) ground_ctrl(b);
        else if (b >= 0x30) state = S_GROUND;
        break;
      case S_CHARSET:
        g_special[cs_which] = (b == '0');   /* '0'=DEC 特殊, 'B'/'A' などは ASCII */
        state = S_GROUND; break;
      case S_CSI: i += csi_feed(p + i - 1, n - i + 1, 0) - 1; break;
      case S_STR: str_byte(b); break;
      default: state = S_GROUND; break;
    }
  }
  flush();
  sup_leave(sup);
}

/* ================= 初期化・終了 ================= */
void vt_default_size(int *cols, int *rows) { if (cols) *cols = MAXCOLS; if (rows) *rows = MAXROWS; }
static int vt_up;   /* vt_init 済み (vt_fini で 0) */
static void pane_probe(void);
/* vt_init の前 (ssh.x の pty 要求は画面を切り替える前) は、これから描く枠の大きさを返す */
void vt_get_size(int *cols, int *rows)
{
  if (!vt_up) {
    pane_probe();
    if (cols) *cols = vt_pane ? pane_w : MAXCOLS;
    if (rows) *rows = vt_pane ? pane_h : MAXROWS;
    return;
  }
  if (cols) *cols = COLS; if (rows) *rows = ROWS;
}
void vt_get_origin(int *col, int *row) { if (col) *col = vt_pane ? pane_x : 0; if (row) *row = vt_pane ? pane_y : 0; }

#ifdef VT_HOSTTEST
static int host_pane_x = -1, host_pane_y, host_pane_w, host_pane_h;
/* 試験用: _B_CONSOL の代わりに枠を与える (vthost -P x,y,w,h)。x < 0 で全面 */
void vt_host_pane(int x, int y, int w, int h) { host_pane_x = x; host_pane_y = y; host_pane_w = w; host_pane_h = h; }
#endif
/* _B_CONSOL (IOCS $2E) を d1 = d2 = -1 で呼ぶと、変えずに今の枠を d1 = 原点 (x ドット << 16 | y ドット)、
 * d2 = (桁数-1) << 16 | (行数-1) で返す */
static void pane_probe(void)
{
  long org, size;
  int x, y, w, h;
#ifdef VT_HOSTTEST
  if (host_pane_x < 0) { vt_pane = 0; return; }
  org = ((long)host_pane_x * CW << 16) | (host_pane_y * CH);
  size = ((long)(host_pane_w - 1) << 16) | (host_pane_h - 1);
#else
  {
    register long d0 __asm__("d0") = 0x2e;
    register long d1 __asm__("d1") = -1;
    register long d2 __asm__("d2") = -1;
    __asm__ volatile ("trap #15" : "+r"(d0), "+r"(d1), "+r"(d2) : : "cc", "memory");
    org = d1; size = d2;
  }
#endif
  x = ((org >> 16) & 0xffff) / CW; y = (org & 0xffff) / CH;
  w = ((size >> 16) & 0xffff) + 1; h = (size & 0xffff) + 1;
  vt_pane = 0;
  if (w < MAXCOLS || x != 0) {
    /* 枠らしいものだけ (左端が 4 桁の倍数、画面に収まる) */
    if ((x & 3) == 0 && w >= 8 && h >= 4 && x + w <= MAXCOLS && y + h <= MAXROWS) {
      vt_pane = 1; pane_x = x; pane_y = y; pane_w = w; pane_h = h;
    }
  }
}

void vt_init(int cols, int rows)
{
  int r, c, s;
#ifdef VT_VCONS
  if (vc_inited) { vt_con_new(0); return; }   /* 画面は占有済み (2 つ目以降の端末の HELLO など): 端末の状態だけ初期化 */
  vt_hidden = 0;
#endif
  if (cols <= 0) cols = MAXCOLS;
  if (rows <= 0) rows = MAXROWS;
  if (cols > MAXCOLS) cols = MAXCOLS;
  if (rows > MAXROWS) rows = MAXROWS;
  pane_probe();
  vt_base = TVRAM_PHYS;
  if (vt_pane) {
    vt_base = TVRAM_PHYS + (unsigned long)pane_y * CH * VBYTES + pane_x;
    if (cols > pane_w) cols = pane_w;
    if (rows > pane_h) rows = pane_h;
  }
  COLS = cols; ROWS = rows;
  vt_up = 1;
  if (!sg_ready) { sg_build(); pm_build(); }
  cx = cy = 0; top = 0; bot = ROWS - 1; pend_wrap = 0;
  c_fg = 7; c_bg = 0; a_bold = a_uline = a_reverse = 0; attr_update();
  decckm = 0; decawm = 1; cur_vis = 1; alt_active = 0; cur = rows_main;
  for (r = 0; r < MAXROWS; r++) { rows_main[r] = store_main[r]; rows_alt[r] = store_alt[r]; drawn[r] = store_drawn[r]; }
  g_special[0] = g_special[1] = 0; gl = 0; outq_len = 0; state = S_GROUND; u_need = 0;
  csr_shown = 0;
  for (c = 0; c < MAXCOLS; c++) tabstop[c] = (c % 8 == 0) ? 1 : 0;
  s = sup_enter();
  sa_cur = 0;
#ifdef VT_NO_SA
  sa_ok = 0;              /* -DVT_NO_SA: 同時アクセスを使わない (プレーンごとに書く) */
#else
  sa_ok = sa_probe();
#endif
  palette_setup();
  _iocs_b_curoff();           /* BIOS のカーソルは使わない (自前で描く) */
  for (r = 0; r < ROWS; r++)
    { for (c = 0; c < COLS; c++) { clear_cell(&cur[r][c]); drawn[r][c] = cur[r][c]; } row_fill(r, c_bg); }
  for (r = 0; r < MAXROWS; r++) { rlo[r] = MAXCOLS; rhi[r] = 0; }
  sa_off();
  sup_leave(s);
  _dos_breakck(0);            /* Ctrl-C ブレークを止める (キーはこちらで拾う) */
#ifdef VT_VCONS
  vc_inited = 1;
#endif
}

void vt_fini(void)
{
  int s, i;
  s = sup_enter();
  sa_off();
  if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }
  if (pal_saved) for (i = 0; i < 16; i++) TPAL[i] = pal_save[i];
  _iocs_b_curon();
  sup_leave(s);
  _dos_breakck(1);
  vt_up = 0;
#ifdef VT_VCONS
  vc_inited = 0;
#endif
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
#ifdef VT_VCONS
    if (vt_keyhook) { int h = vt_keyhook(code); if (h == 1) continue; if (h == 2) break; }   /* 呼び出し側が使ったキー */
#endif
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
    for (c = 0; c < COLS; c++) blit(c, r, &cur[r][c], 0);
  if (csr_shown) cursor_xor(csr_x, csr_y);
  for (p = 0; p < 4 && ok; p++)
    for (r = 0; r < ROWS * CH && ok; r++)
      for (c = 0; c < COLS; c++) {
        long o = (long)(vt_base - TVRAM_PHYS) + (long)p * PLANE + (long)r * VBYTES + c;
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
  if (fg) *fg = (unsigned char)AT_FG(cur[r][c].at);
  if (bg) *bg = (unsigned char)AT_BG(cur[r][c].at);
  if (fl) *fl = (unsigned char)AT_FL(cur[r][c].at);
}
void vt_get_cursor(int *x, int *y) { if (x) *x = cx; if (y) *y = cy; }
/* 絵文字インデックス -> 実際の Unicode コードポイント (F_EMOJI セルの cp を渡す)。 */
unsigned long vt_emoji_cp(int idx) { return (idx >= 0 && idx < EMOJITAB_N) ? emojitab[idx].cp : '?'; }

/* ================= 仮想コンソール (-DVT_VCONS) =================
 * VRAM は 1 枚のまま、端末 (論理画面・カーソル・属性・モード・解析の途中・応答キュー) を複数持って切り替える
 * (ssh.x が 1 本の接続の複数のセッションチャネルを Ctrl+F1..F4 で切り替える)。
 * ・端末の状態 (下の VC_VARS / VC_ARRS) は vt_con_save / vt_con_load で入れ物と写し合う (~1.3 KB。解析の速い道は
 *   大域変数のまま)。画面 2 面のセル (24 KB) は写さず、入れ物の行ポインタ表がそれぞれの置き場を指す。
 * ・裏の端末 (vt_hidden) は解析と論理画面だけ: flush と scroll の VRAM・drawn は触らない。drawn は表の VRAM の控えのまま。
 * ・カーソルの描画状態 (csr_*) も端末ごと。裏に回すとき csr_shown を 0 にする (表に出すとき全面を描き直すので消さなくてよい)。
 * ・グリフ表・pm_tab・cp_kind の控え・同時アクセス・パレットの退避・drawn・rlo/rhi は共通。 */
#ifdef VT_VCONS
#include <string.h>
#define VC_VARS X(cx) X(cy) X(pend_wrap) X(top) X(bot) X(c_fg) X(c_bg) X(a_bold) X(a_uline) X(a_reverse) X(cur_at) X(clr_cell) \
  X(decckm) X(decawm) X(cur_vis) X(alt_active) X(gl) X(sv_cx) X(sv_cy) X(sv_fg) X(sv_bg) X(sv_bold) X(sv_ul) X(sv_rev) \
  X(sv_sp0) X(sv_sp1) X(sv_gl) X(csr_shown) X(csr_x) X(csr_y) X(state) X(csi_priv) X(csi_inter) X(nparam) X(str_kind) \
  X(str_esc) X(str_len) X(cs_which) X(u_cp) X(u_need) X(outq_len) X(vt_hidden) \
  X(vt_base) X(COLS) X(ROWS) X(vt_pane) X(pane_x) X(pane_y) X(pane_w) X(pane_h)
#define VC_ARRS X(g_special) X(tabstop) X(param) X(str_buf) X(outq) X(rows_main) X(rows_alt) X(drawn)
typedef struct {
#define X(v) __typeof__(v) v;
  VC_VARS VC_ARRS
#undef X
} vc_state;

int vt_con_size(void) { return (int)sizeof(vc_state); }
int vt_con_scrsize(void) { return (int)(3 * sizeof store_main); }   /* main・alt・drawn (VRAM の控え) */

void vt_con_save(void *st)
{
  vc_state *s = (vc_state *)st;
#define X(v) s->v = v;
  VC_VARS
#undef X
#define X(v) memcpy(s->v, v, sizeof v);
  VC_ARRS
#undef X
}

void vt_con_load(const void *st)
{
  const vc_state *s = (const vc_state *)st;
#define X(v) v = s->v;
  VC_VARS
#undef X
#define X(v) memcpy(v, s->v, sizeof v);
  VC_ARRS
#undef X
  cur = alt_active ? rows_alt : rows_main;
}

/* いまの端末を初期状態・空の画面にする。scr (vt_con_scrsize バイト) を新しい端末の画面の置き場にして裏に置く。
 * scr が NULL なら画面の置き場と表裏はそのまま (表なら次の flush で空の画面が描かれる)。 */
void vt_con_new(void *scr)
{
  int r, c;
  if (scr) {
    cell_t (*m)[MAXCOLS] = (cell_t (*)[MAXCOLS])scr, (*a)[MAXCOLS] = m + MAXROWS, (*d)[MAXCOLS] = a + MAXROWS;
    for (r = 0; r < MAXROWS; r++) { rows_main[r] = m[r]; rows_alt[r] = a[r]; drawn[r] = d[r]; }
    vt_hidden = 1; csr_shown = 0;
  } else if (!rows_main[0]) {
    for (r = 0; r < MAXROWS; r++) { rows_main[r] = store_main[r]; rows_alt[r] = store_alt[r]; }
  }
  cx = cy = 0; top = 0; bot = ROWS - 1; pend_wrap = 0;
  c_fg = 7; c_bg = 0; a_bold = a_uline = a_reverse = 0; attr_update();
  decckm = 0; decawm = 1; cur_vis = 1; alt_active = 0; cur = rows_main;
  g_special[0] = g_special[1] = 0; gl = 0; outq_len = 0; state = S_GROUND; u_need = 0;
  sv_cx = sv_cy = 0; sv_fg = 7; sv_bg = 0; sv_bold = sv_ul = sv_rev = sv_sp0 = sv_sp1 = sv_gl = 0;
  for (c = 0; c < MAXCOLS; c++) tabstop[c] = (c % 8 == 0) ? 1 : 0;
  for (r = 0; r < MAXROWS; r++) {
    for (c = 0; c < MAXCOLS; c++) { clear_cell(&rows_main[r][c]); clear_cell(&rows_alt[r][c]); }
    TOUCH(r);
  }
}

/* drawn を「VRAM の絵は分からない」にする (どのセルとも等しくならず、blit は全プレーンを書く) */
static void vc_unknown(int r, int c0, int c1)
{
  cell_t z;
  int c;
  z.cp = 0xffff; z.at = F_EMOJI | F_WIDECONT;
  for (c = c0; c <= c1; c++) drawn[r][c] = z;
  touch_span(r, c0, c1);
}

/* 表に出す (VRAM を全部この端末の絵に描き直す) / 裏に回す (以後 VRAM に描かない) */
void vt_con_visible(int on)
{
  int r, s;
  if (!on) { vt_hidden = 1; csr_shown = 0; return; }
  vt_hidden = 0; csr_shown = 0;   /* VRAM のカーソル (前の端末の) は全面の描き直しで消える */
  s = sup_enter();
  for (r = 0; r < ROWS; r++) vc_unknown(r, 0, COLS - 1);
  flush();
  sup_leave(s);
}

/* いまの端末を画面の枠 (桁 col・行 row から w x h) に置き直す (Z-OS の zos.x がペインごとに 1 つの端末を持ち、
 * 割り方を変えたときに呼ぶ)。drawn は端末ごとなので、表の端末が何枚あっても互いに干渉しない。
 * 論理画面のセルはそのまま (枠の外になった分は見えなくなるだけ)。VRAM は描かない (表なら vt_con_visible(1) で描き直す)。
 * 左端 col は偶数 (スクロールの 32bit 転送)。 */
void vt_con_rect(int col, int row, int w, int h)
{
  int r;
  if (w > MAXCOLS) w = MAXCOLS;
  if (h > MAXROWS) h = MAXROWS;
  pane_x = col; pane_y = row; pane_w = w; pane_h = h;
  vt_pane = (col != 0 || w < MAXCOLS || row != 0 || h < MAXROWS);
  vt_base = TVRAM_PHYS + (unsigned long)row * CH * VBYTES + col;
  COLS = w; ROWS = h;
  top = 0; bot = ROWS - 1; pend_wrap = 0;
  clamp_cursor();
  csr_shown = 0;
  for (r = 0; r < ROWS; r++) vc_unknown(r, 0, COLS - 1);
}

/* JIS X 0208 -> Unicode (kanjitab の逆。最初の呼び出しで 94x94 の表を作る)。見つからなければ 0。
 * Shift_JIS を出す Human68k のプログラムの出力を UTF-8 にして vt_write に渡すため (zos.x) */
unsigned short vt_jis_ucs(unsigned short jis)
{
  static unsigned short *rev;
  static unsigned short revtab[94 * 94];
  int ku = (jis >> 8) - 0x21, ten = (jis & 0xff) - 0x21;
  if (!rev) {
    int i;
    for (i = 0; i < KANJITAB_N; i++) {
      int k = (kanjitab[i].jis >> 8) - 0x21, t = (kanjitab[i].jis & 0xff) - 0x21;
      if (k >= 0 && k < 94 && t >= 0 && t < 94 && !revtab[k * 94 + t]) revtab[k * 94 + t] = kanjitab[i].ucs;
    }
    rev = revtab;
  }
  if (ku < 0 || ku >= 94 || ten < 0 || ten >= 94) return 0;
  return rev[ku * 94 + ten];
}

/* 状態表示: 最上行の右端に s を反転で重ねる (論理画面は変えない)。NULL で消す (その桁を論理画面の絵に戻す) */
static int vc_st_c0 = -1;
void vt_con_status(const char *s)
{
  int sup, i, n;
  if (vt_hidden || !rows_main[0]) return;
  sup = sup_enter();
  if (vc_st_c0 >= 0) { vc_unknown(0, vc_st_c0, COLS - 1); vc_st_c0 = -1; }
  if (s) {
    if (csr_shown) { cursor_xor(csr_x, csr_y); csr_shown = 0; }
    n = (int)strlen(s);
    if (n > COLS) n = COLS;
    vc_st_c0 = COLS - n;
    if (vc_st_c0 > 0 && (cur[0][vc_st_c0].at & F_WIDECONT)) vc_unknown(0, vc_st_c0 - 1, vc_st_c0 - 1);   /* 割った全角の左も描き直す */
    for (i = 0; i < n; i++) {
      cell_t z;
      z.cp = (unsigned char)s[i]; z.at = AT(0, 7, 0);
      blit(vc_st_c0 + i, 0, &z, 0);
    }
    sa_off();
    for (i = 0; i < n; i++) { cell_t z; z.cp = 0xffff; z.at = F_EMOJI | F_WIDECONT; drawn[0][vc_st_c0 + i] = z; }
    if (cur_vis && !(cy == 0 && cx >= vc_st_c0)) { cursor_xor(cx, cy); csr_shown = 1; csr_x = cx; csr_y = cy; }
  } else {
    flush();
  }
  sup_leave(sup);
}

/* ホストへの応答 (DSR / DA など) だけを取り出す (キーボードは読まない。裏の端末の応答をそのチャネルへ返す) */
int vt_reply(unsigned char *out, int max)
{
  int k = outq_len < max ? outq_len : max;
  memcpy(out, outq, k);
  memmove(outq, outq + k, outq_len - k);
  outq_len -= k;
  return k;
}
#endif
