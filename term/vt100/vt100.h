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

/* 既定の桁数・行数 (96x32)。 */
void vt_default_size(int *cols, int *rows);

/* --- デバッグ用: 論理画面の取り出し (vtcat / ホストテスト用) --- */
/* out[0]=文字コード, out[1]=前景色, out[2]=背景色, out[3]=フラグ (F_UNDERLINE=1, F_SPECIAL=2) */
void vt_get_cell(int r, int c, unsigned char out[4]);
void vt_get_cursor(int *x, int *y);

#endif /* VT100_H */
