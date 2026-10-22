/*
 * accel.h -- X68000 Z 側の暗号アクセラレータ呼び出し (npd.sys 経由で Pico W に X25519 / Ed25519 を計算させる)
 *
 * 使い方: accel_open() で npd.sys を探して Pico に PING し、返った caps (ACCEL_CAP_*) で使える段階を知る。
 * 各関数は 0 (または verify なら 1/0) を返す。-1 は「アクセラレータが使えない」(npd.sys が無い、Pico が古い、
 * 時間切れ、BUSY) なので、呼び出し側は自前の計算に切り替える。
 * 同時に 1 つしか頼めない (npd.sys が 1 つだけ預かる)。ユーザモードから呼ぶ (中で _dos_super する)。
 */
#ifndef ACCEL_H
#define ACCEL_H
#include <stdint.h>
#include "accel_proto.h"

int accel_open(void);                       /* 戻り値: caps (0 以上) / -1 npd.sys が無い / -2 Pico が答えない (3 秒待つ) */
int accel_caps(void);                       /* accel_open の結果 (開いていなければ 0) */
int accel_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]);      /* 0 / -1 */
int accel_x25519_base(uint8_t q[32], const uint8_t n[32]);                      /* 0 / -1 */
int accel_ed25519_verify(const uint8_t *m, unsigned mlen, const uint8_t sig[64], const uint8_t pk[32]);  /* 1 正しい / 0 違う / -1 */
int accel_ed25519_sign(uint8_t sig[64], const uint8_t *m, unsigned mlen, const uint8_t seed[32], const uint8_t pk[32]);  /* 0 / -1 */

/* 直前の呼び出しの計測: Pico での計算時間 (µs) と、Z で頼んでから結果を取るまでの時間 (1/100 秒)、Pico の status */
uint32_t accel_last_usec(void);
unsigned accel_last_ticks(void);
int accel_last_status(void);

/* 1 回の待ち時間の上限 (1/100 秒)。既定 1500 (15 秒)。0 なら待たない */
void accel_set_timeout(unsigned cs);

#endif
