/* x68k_accel.h -- dropbear (ssh.x) から暗号アクセラレータ (net/accel) を使うための入口。dropbear-accel.patch が参照する。
 * どの関数も「Pico に任せられなかった」ときは -1 を返すので、呼び出し側は元の関数 (Z 自前の計算) に落ちる */
#ifndef X68K_ACCEL_H
#define X68K_ACCEL_H
void x68k_accel_setmode(const char *mode);      /* "none" | "verify" | "kex" | "all" (-oAccel=... / 環境変数 ACCEL) */
int x68k_accel_x25519(unsigned char *q, const unsigned char *n, const unsigned char *p);   /* 0 計算した / -1 */
int x68k_accel_ed25519_verify(const unsigned char *m, unsigned long mlen,
                              const unsigned char *s, unsigned long slen, const unsigned char *pk);  /* 0 正しい / 1 違う / -1 */
int x68k_accel_ed25519_sign(const unsigned char *m, unsigned long mlen, unsigned char *s, unsigned long *slen,
                            const unsigned char *sk, const unsigned char *pk);   /* 0 署名した / -1 */
#endif
