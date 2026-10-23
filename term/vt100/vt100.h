/*
 * vt100.h -- X68000 (Human68k 3.02) 用 VT100 (xterm サブセット) 端末エミュレータ
 *
 * Human68k の CON デバイスが解釈する ANSI は一部だけ (スクロール領域・DEC 罫線・
 * アプリケーションカーソル/キーパッド無し、折り返しも独特) なので、vim / less / top の
 * ような全画面プログラムが崩れる。このライブラリはバイト列を自前で解釈し、X68000 の
 * テキスト画面 (0xE00000、4 プレーン) に直接描く。SSH クライアントから link して使う。
 *
 * 使い方:
 *   vt_init(cols, rows);              画面を占有する (既定は vt_default_size の 96x32)
 *   for (;;) {
 *     n = ssh_recv(buf, sizeof buf);
 *     vt_write(buf, n);               受信バイト列を解釈して描く
 *     m = vt_key(kbuf, sizeof kbuf);  キーボードを読み、VT100 のバイト列にして送信へ
 *     if (m) ssh_send(kbuf, m);
 *   }
 *   vt_fini();                        コンソールを元に戻す
 *
 * ・プレーン C。elf2x68k gcc / newlib。動的確保なし (画面は静的バッファ)。
 * ・スーパーバイザ切り替えはライブラリ内部で完結する。クライアントはユーザモードのままでよい。
 * ・vt_init〜vt_fini の間、クライアントは DOS のコンソール出力 (_dos_print 等) に触れないこと。
 */
#ifndef VT100_H
#define VT100_H

/* 画面を占有して初期化する。cols/rows が 0 以下なら既定サイズになる。
 * cols は最大 96、rows は最大 32 に丸められる (768x512・8x16 フォント)。 */
void vt_init(int cols, int rows);

/* 受信バイト列 (UTF-8/ASCII + エスケープシーケンス) を解釈して描画する。
 * シーケンスが複数回の呼び出しに分割されても状態を保持して継続する。 */
void vt_write(const unsigned char *p, int n);

/* キーボードを非ブロッキングで読み、押されたキーを VT100 のバイト列に変換して out に入れる。
 * ホストへの応答 (カーソル位置報告 DSR、装置識別 DA など) もここから出る。
 * 戻り値は書き込んだバイト数 (最大 max)、何も無ければ 0。 */
int vt_key(unsigned char *out, int max);

/* コンソール (パレット・カーソル・ブレーク) を元に戻す。 */
void vt_fini(void);

/* 現在の画面サイズを得る (pty のウィンドウサイズ要求用)。 */
void vt_get_size(int *cols, int *rows);

/* 描いている枠の原点 (桁・行)。全面なら 0,0。Z-OS (zos.x) のペインの中では、_B_CONSOL が返したペインの左上。
 * vt_init のときに _B_CONSOL を読み、幅 96 桁未満か原点が左端でなければ枠の中だけに描く (vt_get_size はその枠の大きさ)。 */
void vt_get_origin(int *col, int *row);
void vt_get_origin_px(int *x, int *y);   /* 同じ原点をドットで (zos.x の小さい字のペインは行が 12 ドット) */

/* 小さい字のモード: 6x12 ドットの字で 128x42 (既定は 8x16 で 96x32)。vt_init の前に呼ぶ。全体で 1 つ。
 * 字形は ANK 6x12 と全角 12x12 (IOCS _FNTADR)。絵文字は描けない (??)。 */
void vt_set_small(int on);

/* 既定の桁数・行数 (96x32)。 */
void vt_default_size(int *cols, int *rows);

/* --- デバッグ用: 論理画面の取り出し (vtcat / ホストテスト用) --- */
/* cp=コードポイント, fg=前景色, bg=背景色, fl=フラグ
 * (F_UNDERLINE=1, F_SPECIAL=2, F_WIDE=4 全角左, F_WIDECONT=8 全角右, F_EMOJI=16 絵文字) */
void vt_get_cell(int r, int c, unsigned short *cp, unsigned char *fg, unsigned char *bg, unsigned char *fl);
void vt_get_cursor(int *x, int *y);
unsigned long vt_emoji_cp(int idx);   /* F_EMOJI セルの cp (インデックス) -> Unicode コードポイント */

/* --- 仮想コンソール (-DVT_VCONS): VRAM 1 枚で複数の端末を切り替える (vt100.c の末尾) ---
 * 入れ物 (vt_con_size バイト) に端末の状態を写して持ち替える。画面のセルは vt_con_new に渡した置き場
 * (vt_con_scrsize バイト = main + alt の 2 面) にあり、入れ物はそれを指すだけ。最初の端末 (vt_init) は内蔵の置き場。
 *   新しい端末: vt_con_save(old); vt_con_new(scr);  (裏に置かれる)  表に出す: vt_con_visible(1)
 *   切り替え:   vt_con_visible(0); vt_con_save(a); vt_con_load(b); vt_con_visible(1);
 * 裏の端末は vt_write しても VRAM に描かない。表に出すとき全面を描き直す。vt_init 済みの間の vt_init は
 * いまの端末の状態だけを初期化する (zterm の HELLO)。 */
int vt_con_size(void);
int vt_con_scrsize(void);
void vt_con_save(void *st);
void vt_con_load(const void *st);
void vt_con_new(void *scr);
void vt_con_visible(int on);
void vt_follow(void);                                /* zos.x のペインの枠を見直す (出力が無いときも。ssh.x のキー待ちから) */
void vt_con_rect(int col, int row, int w, int h);
extern int vt_nomove;                                /* (zos.x) vt_defer のときスクロールで VRAM を動かさない (既定 1) */
extern int vt_defer;                                 /* (zos.x) 1: vt_write は描かない。描くのは vt_flush_some */
int vt_flush_some(int maxrows);                      /* (zos.x) 描き直しの要る行を多くて maxrows 行描く。残っていれば 1 */
void vt_con_cursor(int on);                          /* いまの端末のカーソルを描く (1) / 描かない (0)。既定 1 */   /* いまの端末を画面の枠に置き直す (表の端末を何枚でも並べられる) */
unsigned short vt_jis_ucs(unsigned short jis);       /* JIS X 0208 -> Unicode (0 = 無い) */
void vt_con_status(const char *s);     /* 最上行の右端に s を反転で一時表示 (論理画面は変えない)。NULL で消す */
int vt_reply(unsigned char *out, int max);   /* DSR/DA の応答だけ取り出す (キーボードは読まない) */
/* vt_key が IOCS のキー (bit8-15 スキャン、bit0-7 ASCII) を読むたびに呼ぶ。1 を返すとそのキーを捨てる、
 * 2 なら捨てて vt_key を抜ける (端末を切り替えるので後のキーは次の vt_key で)。 */
extern int (*vt_keyhook)(int code);

#endif /* VT100_H */
