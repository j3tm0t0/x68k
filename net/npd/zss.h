/*
 * zss.h -- Pico の zsshd と Z の zos.x の間の端末のセッションの記録形式 (0xA7)。x68k-zos の kernel/zss.h の写し
 *          (変えるときは両方)。x68k-zos issue #21、docs/sshd-z-side.md、
 *          docs/pico-sshd-study.md §6.4
 *
 * npd.x の X6NR 交換に、先頭 1 バイトが ZSS_MARK (0xA7) のレコードを混ぜる (0xA5 暗号、0xA6 TSP と同じ流儀)。
 *   Z  -> Pico: [長さ 2][要求のレコード][偶数詰め物]
 *   Pico -> Z: [長さ 2][空き 14][応答のレコード][偶数詰め物]
 *   要求の見出し: [MARK][ver][flags][0][zseq 2][0 2][zepoch 4]
 *   応答の見出し: [MARK][ver][flags][0][zack 2][0 2][pepoch 4]
 *   メッセージ:   [type][sess][gen][0][len 2][本体 len][偶数詰め物]
 * 確認応答と epoch は TSP (net/tistack/tsp.h) と同じ: Z は zack が zseq と同じになるまで同じ要求を送り直す。Pico は最後の
 * 応答を持っておき、同じ zseq には中身を適用せずに同じ応答を返す。値はすべてビッグエンディアン。
 */
#ifndef ZSS_H
#define ZSS_H
#include <stdint.h>

#define ZSS_MARK        0xa7
#define ZSS_VERSION     1
#define ZSS_HDR         12
#define ZSS_MHDR        6
#define ZSS_NSESS       4
#define ZSS_REQ_MAX     4078            /* TSP_REQ_MAX と同じ */
#define ZSS_RES_MAX     4064

/* 応答の flags */
#define ZSS_PF_MORE     0x01            /* Pico に Z へ送れるものがまだある */

/* Pico -> Z */
#define ZSS_E_OPEN      0x81            /* [cols 2][rows 2][peer 4][port 2][ulen 1][user][tlen 1][TERM][flags 1] */
#define ZSS_E_INPUT     0x82            /* バイト列 (WINDOW の範囲で) */
#define ZSS_E_WINCH     0x83            /* [cols 2][rows 2] */
#define ZSS_E_SIGNAL    0x84            /* 名前 ("INT" など) */
#define ZSS_E_EOF       0x85
#define ZSS_E_CLOSED    0x86            /* [rlen 1][理由] */
#define ZSS_E_CREDIT    0x87            /* [bytes 4] Z がこの後 OUTPUT で送ってよいバイト数 (絶対値) */
/* Z -> Pico */
#define ZSS_C_ACCEPT    0x01            /* [free 4] */
#define ZSS_C_REJECT    0x02            /* [rlen 1][理由] */
#define ZSS_C_OUTPUT    0x03            /* バイト列 (CREDIT の範囲で) */
#define ZSS_C_WINDOW    0x04            /* [free 4] Z の入力のリングの空き (絶対値) */
#define ZSS_C_EXIT      0x05            /* [status 4] */

#define ZSS_OF_UTF8     0x01            /* OPEN の flags: クライアントは UTF-8 */

/* npd.x が zos.x の入口 (d0 = 命令、a0 = 引数) を呼ぶ命令。zos.x は始まるときに入口を npd.sys に預ける (ETD 27)、
   npd.x は交換のたびに ETD 28 で受け取る */
#define ZSS_NP_OUT      0x7370          /* a[0] = 書く先, a[1] = 最大 (0 なら問い合わせだけ), a[2] <- 急ぎ。戻り値 = 長さ */
#define ZSS_NP_IN       0x7371          /* a[0] = 長さ, a[1] = 応答のレコード。戻り値 bit 0 = Pico に続きあり */
#define NPD_ETD_ZSS_SET 27
#define NPD_ETD_ZSS_GET 28

static inline void zss_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline unsigned zss_get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static inline void zss_put32(uint8_t *p, uint32_t v) { zss_put16(p, v >> 16); zss_put16(p + 2, v & 0xffff); }
static inline uint32_t zss_get32(const uint8_t *p) { return ((uint32_t)zss_get16(p) << 16) | zss_get16(p + 2); }
static inline uint8_t *zss_msg(uint8_t *p, int type, int sess, int gen, unsigned len)
{
  p[0] = (uint8_t)type; p[1] = (uint8_t)sess; p[2] = (uint8_t)gen; p[3] = 0;
  zss_put16(p + 4, len);
  return p + ZSS_MHDR;
}
static inline unsigned zss_msize(unsigned len) { return ZSS_MHDR + ((len + 1) & ~1u); }

#endif
