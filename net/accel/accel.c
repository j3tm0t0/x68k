/*
 * accel.c -- X68000 Z 側の暗号アクセラレータ呼び出し (accel.h を参照)
 *
 * npd.sys の ETD 命令 20 (預ける) / 21 (結果を取る) / 22 (取り消す) を使う。npd.sys はスーパーバイザ領域なので
 * 呼び出しは _dos_super で囲む (ユーザモードから ETD を呼ぶとバスエラー)。待つ間は _dos_change_pr で
 * npd.x や xip.x に CPU を譲る。
 */
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdint.h>
#include <string.h>
#include "accel.h"

extern long accel_call_etd(void *entry, long cmd, void *arg);   /* accel_etd.S */

static void *etd;                       /* npd.sys の ETD 入口 */
static int caps;
static uint32_t last_usec;
static unsigned last_ticks;
static int last_status = -1;
static unsigned timeout_cs = 1500;

static int memeq(const void *a, const void *b, int n) { return memcmp(a, b, n) == 0; }

/* デバイスチェインから "/dev/ep0" + "EthD" + "NPD " を探す (npd.x / ztools と同じ) */
static void *find_npd(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        if (memeq(d + 14, "/dev/ep0", 8) && memeq(d + 22, "EthDNPD ", 8))
          return d + 30;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* スーパーバイザで ETD を 1 回呼ぶ */
static long etd_call(long cmd, void *arg)
{
  int ssp = _dos_super(0);
  long r = accel_call_etd(etd, cmd, arg);
  if (ssp > 0)
    _dos_super(ssp);
  return r;
}

static unsigned ticks(void) { return (unsigned)_iocs_ontime().sec; }   /* 1/100 秒 */

/* 要求を 1 つ送って結果を待つ。戻り値: Pico の status (0 以上)、使えなければ -1。
   result には結果 (accel_res_len(op) バイト) を写す */
static int call(int op, const uint8_t *fixed, const uint8_t *msg, int mlen, uint8_t *result)
{
  static uint8_t req[ACCEL_REQ_MAX], res[ACCEL_RES_MAX];
  long arg[2], r;
  int n, rop, st, rsv;
  unsigned id, rid, t0;
  uint32_t usec;
  const uint8_t *pr;
  last_status = -1;
  last_usec = 0;
  if (!etd)
    return -1;
  n = accel_req_build(req, op, 0, fixed, msg, mlen);
  if (n < 0)
    return -1;
  arg[0] = (long)req;
  arg[1] = n;
  r = etd_call(20, arg);
  if (r <= 0)
    return -1;                          /* 別の要求が進行中など */
  id = (unsigned)r;
  t0 = ticks();
  for (;;) {
    arg[0] = (long)res;
    arg[1] = sizeof res;
    r = etd_call(21, arg);
    if (r > 0)
      break;
    if (r < 0 || (unsigned)(ticks() - t0) >= timeout_cs) {
      etd_call(22, 0);                      /* 時間切れ: 取り消す (遅れて来た応答は npd.sys が捨てる) */
      return -1;
    }
    _dos_change_pr();
  }
  last_ticks = ticks() - t0;
  n = accel_res_parse(res, (int)r, &rop, &rid, &st, &rsv, &usec, &pr);
  if (n < 0 || rop != op || rid != id)
    return -1;
  last_usec = usec;
  last_status = st;
  if (st == ACCEL_ST_OK) {
    if (n != accel_res_len(op))
      return -1;
    if (result)
      memcpy(result, pr, n);
  } else if (st == ACCEL_ST_BADSIG) {
    /* 計算はできた (署名が違うだけ) */
  } else {
    return -1;                          /* BADREQ / BADOP / BUSY / DISABLED: 自前で計算してもらう */
  }
  if (op == ACCEL_OP_PING && st == ACCEL_ST_OK)
    caps = rsv;
  return st;
}

int accel_open(void)
{
  uint8_t ver;
  int ssp = _dos_super(0);
  etd = find_npd();
  if (ssp > 0)
    _dos_super(ssp);
  caps = 0;
  if (!etd)
    return -1;
  {
    /* PING は短い時間切れで (古いファームだと答えが来ない。ssh.x を 15 秒も待たせない) */
    unsigned saved = timeout_cs;
    int st;
    if (timeout_cs > 300)
      timeout_cs = 300;
    st = call(ACCEL_OP_PING, 0, 0, 0, &ver);
    timeout_cs = saved;
    if (st != ACCEL_ST_OK) {
      etd = 0;
      return -2;
    }
  }
  return caps;
}

int accel_caps(void) { return caps; }
uint32_t accel_last_usec(void) { return last_usec; }
unsigned accel_last_ticks(void) { return last_ticks; }
int accel_last_status(void) { return last_status; }
void accel_set_timeout(unsigned cs) { timeout_cs = cs; }

int accel_x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32])
{
  uint8_t f[64];
  memcpy(f, n, 32);
  memcpy(f + 32, p, 32);
  return call(ACCEL_OP_X25519, f, 0, 0, q) == ACCEL_ST_OK ? 0 : -1;
}

int accel_x25519_base(uint8_t q[32], const uint8_t n[32])
{
  return call(ACCEL_OP_X25519_BASE, n, 0, 0, q) == ACCEL_ST_OK ? 0 : -1;
}

int accel_ed25519_verify(const uint8_t *m, unsigned mlen, const uint8_t sig[64], const uint8_t pk[32])
{
  uint8_t f[96];
  int st;
  if (mlen > ACCEL_MSG_MAX)
    return -1;
  memcpy(f, pk, 32);
  memcpy(f + 32, sig, 64);
  st = call(ACCEL_OP_ED25519_VERIFY, f, m, (int)mlen, 0);
  return st == ACCEL_ST_OK ? 1 : st == ACCEL_ST_BADSIG ? 0 : -1;
}

int accel_ed25519_sign(uint8_t sig[64], const uint8_t *m, unsigned mlen, const uint8_t seed[32], const uint8_t pk[32])
{
  uint8_t f[64];
  int r;
  if (mlen > ACCEL_MSG_MAX)
    return -1;
  memcpy(f, seed, 32);
  memcpy(f + 32, pk, 32);
  r = call(ACCEL_OP_ED25519_SIGN, f, m, (int)mlen, sig) == ACCEL_ST_OK ? 0 : -1;
  memset(f, 0, sizeof f);
  return r;
}
