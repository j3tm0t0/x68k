/*
 * ztclient.h -- zterm の Z 側 (クライアント)。リモートの zterm ヘルパと 1 本のチャネルで話す。
 *
 * チャネルの中身 (詳細は term/zterm/README.md):
 *   1. 起動スクリプト (zc_command) が行で答える: "ZTERM OK <arch>" / "ZTERM NEED <arch> gz|raw" /
 *      "ZTERM FAIL <理由>" / "ZTERM GO"。NEED なら "<バイト数>\n" と B: の <dir>/<arch>.gz を送る。
 *   2. GO の後はフレーム ('Z' len16 本体) が流れてくる。1 フレーム描くたびに ACK を返す。
 *      キーは 'k' len バイト列 (vt_key が作った VT のバイト列そのまま)。
 *
 * 使い方 (ssh.x の x68k_shim.c / Mac の zthost.c):
 *   zc_init(dir, cols, rows, usercmd);
 *   send_exec(zc_command());
 *   受信:  zc_input(buf, n);           (リモートの stdout)
 *   送信:  n = zc_output(buf, max);    (リモートの stdin へ。zc_pending() > 0 の間)
 *   キー:  zc_keys(kbuf, k);
 *   zc_state() が ZC_DONE / ZC_FAIL になったら終わり。
 */
#ifndef ZTCLIENT_H
#define ZTCLIENT_H

enum { ZC_HANDSHAKE, ZC_PUSH, ZC_WAITGO, ZC_RUN, ZC_DONE, ZC_FAIL };

void zc_init(const char *dir, int cols, int rows, const char *usercmd);   /* usercmd: リモートで動かすコマンド (NULL ならログインシェル) */
const char *zc_command(void);
void zc_input(const unsigned char *p, int n);
int zc_output(unsigned char *p, int max);
int zc_pending(void);
void zc_keys(const unsigned char *p, int n);
void zc_refresh(void);
int zc_state(void);
int zc_started(void);           /* HELLO を受けて画面を占有したか (vt_init 済み) */
const char *zc_error(void);
int zc_exit_code(void);
void zc_stats(long *in_bytes, long *frames, long *push_bytes);
void zc_log(void (*fn)(const char *));   /* 位相の印 (OK/NEED/GO/HELLO) を出す関数 */

/* ztdraw.c (term/vt100/vt100.c の論理画面・描画を直接使う) */
void ztd_init(int cols, int rows);
int ztd_frame(const unsigned char *p, int n);   /* 1 フレームを適用して描く。BYE なら 1、壊れていれば -1 */
int ztd_bye_code(void);
int ztd_mode(void);
void ztd_cursor(int *x, int *y, int *vis);
void ztd_predict(const unsigned char *k, int n, unsigned long seq);   /* 予測エコー: 送るキーの結果を先に描く */
void ztd_echo(int on);                                                /* 予測エコーの有効/無効 (既定 有効) */
void ztd_echo_stats(long *hit, long *miss);
void zc_echo(int on);

#endif /* ZTCLIENT_H */
