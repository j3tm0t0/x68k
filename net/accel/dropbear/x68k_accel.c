/*
 * x68k_accel.c -- dropbear (ssh.x) と暗号アクセラレータ (net/accel/accel.c) のつなぎ
 *
 * 段階 (-oAccel=... または環境変数 ACCEL。既定は none = 使わない):
 *   verify : ed25519 verify だけ Pico に任せる (公開情報しか渡さない。安全性は変わらない)
 *   kex    : + X25519 の鍵生成と共有秘密 (Pico がセッション鍵を知る)
 *   all    : + ed25519 sign (署名鍵の seed を Pico に渡す)
 * 最初の呼び出しで npd.sys を探して Pico に PING し、使えなければ (古いファーム、npd.x 無し) 以後は自前で計算する。
 * Pico が返した caps に無い段階は使わない。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accel.h"
#include "x68k_accel.h"

#define MODE_NONE   0
#define MODE_VERIFY ACCEL_CAP_VERIFY
#define MODE_KEX    (ACCEL_CAP_VERIFY | ACCEL_CAP_X25519)
#define MODE_ALL    (ACCEL_CAP_VERIFY | ACCEL_CAP_X25519 | ACCEL_CAP_SIGN)

static int mode = -1;                   /* -1: まだ決めていない (環境変数を見る) */
static int opened;                      /* 0 まだ / 1 使える / -1 使えない */

static int parse(const char *m)
{
  if (!m || !*m || !strcmp(m, "none") || !strcmp(m, "no") || !strcmp(m, "off") || !strcmp(m, "0")) return MODE_NONE;
  if (!strcmp(m, "verify")) return MODE_VERIFY;
  if (!strcmp(m, "kex")) return MODE_KEX;
  if (!strcmp(m, "all") || !strcmp(m, "yes") || !strcmp(m, "on") || !strcmp(m, "1")) return MODE_ALL;
  fprintf(stderr, "accel: unknown mode '%s' (none|verify|kex|all), using none\n", m);
  return MODE_NONE;
}

void x68k_accel_setmode(const char *m)
{
  mode = parse(m);
}

/* cap の段階が使えるか。初回は npd.sys / Pico を確かめる */
static int usable(int cap)
{
  if (mode < 0)
    mode = parse(getenv("ACCEL"));
  if (!(mode & cap))
    return 0;
  if (opened == 0) {
    int caps = accel_open();
    if (caps < 0) {
      fprintf(stderr, "accel: not available (%s), computing on the X68000\n",
              caps == -1 ? "npd.sys not loaded" : "no answer from the Pico (old firmware or npd.x not running)");
      opened = -1;
    } else {
      fprintf(stderr, "accel: Pico caps=%s%s%s\n", caps & ACCEL_CAP_VERIFY ? "verify " : "",
              caps & ACCEL_CAP_X25519 ? "x25519 " : "", caps & ACCEL_CAP_SIGN ? "sign" : "");
      mode &= caps;
      opened = 1;
    }
  }
  return opened > 0 && (mode & cap);
}

static void note(const char *what, int r)
{
  if (r < 0)
    fprintf(stderr, "accel: %s failed on the Pico (status %d), computing on the X68000\n", what, accel_last_status());
  else
    fprintf(stderr, "accel: %s on the Pico in %lu us (round trip %u.%02u s)\n", what,
            (unsigned long)accel_last_usec(), accel_last_ticks() / 100, accel_last_ticks() % 100);
}

int x68k_accel_x25519(unsigned char *q, const unsigned char *n, const unsigned char *p)
{
  static const unsigned char basepoint[32] = { 9 };
  int r;
  if (!usable(ACCEL_CAP_X25519))
    return -1;
  if (memcmp(p, basepoint, 32) == 0) {
    r = accel_x25519_base(q, n);
    note("x25519 keygen", r);
  } else {
    r = accel_x25519(q, n, p);
    note("x25519 shared", r);
  }
  return r;
}

int x68k_accel_ed25519_verify(const unsigned char *m, unsigned long mlen,
                              const unsigned char *s, unsigned long slen, const unsigned char *pk)
{
  int r;
  if (slen != 64 || mlen > ACCEL_MSG_MAX || !usable(ACCEL_CAP_VERIFY))
    return -1;
  r = accel_ed25519_verify(m, (unsigned)mlen, s, pk);
  note("ed25519 verify", r);
  return r < 0 ? -1 : r == 1 ? 0 : 1;
}

int x68k_accel_ed25519_sign(const unsigned char *m, unsigned long mlen, unsigned char *s, unsigned long *slen,
                            const unsigned char *sk, const unsigned char *pk)
{
  int r;
  if (mlen > ACCEL_MSG_MAX || !usable(ACCEL_CAP_SIGN))
    return -1;
  r = accel_ed25519_sign(s, m, (unsigned)mlen, sk, pk);
  note("ed25519 sign", r);
  if (r == 0)
    *slen = 64;
  return r;
}
