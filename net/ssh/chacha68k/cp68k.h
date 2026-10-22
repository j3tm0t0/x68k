/* cp68k.h -- 68000 向けの ChaCha20 / Poly1305 (dropbear の chacha20-poly1305@openssh.com 用)。
 * libtomcrypt の chacha_crypt / poly1305_* と同じ結果を返す (ホストでも 68000 でも)。
 * CP68K_ASM を定義すると 68000 の手書き asm (chacha68k.s / poly68k.s) を使う。無ければ C 版。 */
#ifndef CP68K_H
#define CP68K_H
#include <stdint.h>

/* Poly1305 の途中状態。h は 2^130-5 を法とする緩い形: h[0..3] が 0..127 ビット (h[0] が最下位の 32 ビット)、
   h4 が 128 ビット以上 (< 16)。r は鍵の前半 (クランプ済み) を 16 ビットずつ (r[0] が最下位)。asm がこの並びを読む。 */
typedef struct {
  uint32_t h[4];
  uint32_t h4;
  uint16_t r[8];
  uint32_t pad[4];
} poly68k_state;

void poly68k_init(poly68k_state *st, const unsigned char key[32]);
/* 16 バイトのブロックを n 個。hibit は 2^128 の項 (完全なブロックは 1、最後の詰めたブロックは 0)。m は偶数番地 */
void poly68k_blocks(poly68k_state *st, const unsigned char *m, unsigned long n, unsigned long hibit);
void poly68k_finish(poly68k_state *st, unsigned char tag[16]);
/* 一括: tag = Poly1305(key, m[0..len)) */
void poly68k_mac(unsigned char tag[16], const unsigned char *m, unsigned long len, const unsigned char key[32]);

/* ChaCha20 (20 ラウンド)。x は libtomcrypt の chacha_state.input と同じ 16 語 (定数 4・鍵 8・カウンタ 2・IV 2、
   ホストのエンディアンの数値)。out = in ^ 鍵ストリーム、len バイト。x[12..13] (64 ビットのカウンタ) は使った
   ブロック数だけ進める (端数のブロックも 1 つと数える。libtomcrypt と違い鍵ストリームの残りは持ち越さない)。 */
void chacha68k_xor(uint32_t x[16], const unsigned char *in, unsigned char *out, unsigned long len);

#endif
