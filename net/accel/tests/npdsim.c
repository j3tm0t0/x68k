/* npdsim -- npd.sys (net/npd/npdsys.c) の ETD 命令 19..24 と Pico 側 (accel_ops.c) をホストでつないで、
 * Z 側の預かり / 送信レコードへの載せ方 / 応答の受け取り / 取り消しを試す。npd.x の rec_in と Pico の
 * bridge_out_rec のレコード歩きもここで模す (同じ枠: Z から [長さ 2][中身][詰め物]、Z へ [長さ 2][空き 14][中身][詰め物])。
 * 本物の npdsys.c をそのまま include する (スタブ: call_handler / dos_print)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef void (*handler_t)(long, void *, long);
void call_handler(handler_t f, long a, void *b, long c) { (void)f; (void)a; (void)b; (void)c; }
void dos_print(const char *s) { (void)s; }
char _end, __bss_start;
#include "../../npd/npdsys.c"
#include "../accel_ops.h"

static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)

/* Pico: Z から来たレコードを歩く (netpipe.c bridge_out_rec と同じ)。制御レコードは accel_compute で即計算し応答を返す */
static int pico_in(const uint8_t *s, int n, uint8_t *res, int *ip_seen)
{
  int rl_res = 0;
  *ip_seen = 0;
  while (n >= 2) {
    int len = (s[0] << 8) | s[1];
    int rl = 2 + ((len + 1) & ~1);
    if (len == 0 || rl > n) break;
    const uint8_t *pkt = s + 2;
    if (pkt[0] == ACCEL_MARK) {
      uint8_t r[ACCEL_RES_MAX];
      int m = accel_compute(pkt, len, r, NULL);
      res[0] = m >> 8; res[1] = m; memset(res + 2, 0, 14); memcpy(res + 16, r, m);
      rl_res = 16 + ((m + 1) & ~1);
    } else if ((pkt[0] >> 4) == 4) {
      (*ip_seen)++;
    }
    s += rl; n -= rl;
  }
  return rl_res;
}

/* Z (npd.x rec_in と同じ歩き方): 応答レコードを npd.sys に渡す */
static void z_in(uint8_t *s, int n)
{
  while (n >= 16) {
    int len = (s[0] << 8) | s[1];
    int rl = 16 + ((len + 1) & ~1);
    uint8_t *ip = s + 16;
    if (rl > n) break;
    if (len >= ACCEL_RES_HDR && ip[0] == ACCEL_MARK) {
      long arg[2] = { len, (long)ip };
      etd_dispatch(23, arg);
    }
    s += rl; n -= rl;
  }
}

static int submit(int op, const uint8_t *fixed, const uint8_t *msg, int mlen)
{
  static uint8_t req[ACCEL_REQ_MAX];
  int n = accel_req_build(req, op, 0, fixed, msg, mlen);
  long arg[2] = { (long)req, n };
  return (int)etd_dispatch(20, arg);
}

int main(void)
{
  static uint8_t tx[4080], rx[4096], res[ACCEL_RES_MAX];
  long arg[4];
  int n, id, ip_seen, rl, st, rop, rsv; unsigned rid; uint32_t usec; const uint8_t *pr;
  uint8_t fixed[96], msg[ACCEL_MSG_MAX];
  /* 1. IP レコードを 2 つ積んでから要求を預ける: ETD 19 は IP の後ろに制御レコードを載せる */
  {
    uint8_t frame[14 + 60] = { 0 };
    frame[12] = 0x08; frame[13] = 0x00; frame[14] = 0x45; frame[16] = 0; frame[17] = 60;
    arg[0] = sizeof frame; arg[1] = (long)frame;
    etd_dispatch(4, arg); etd_dispatch(4, arg);
  }
  memcpy(fixed, "\xa5\x46\xe3\x6b\xf0\x52\x7c\x9d\x3b\x16\x15\x4b\x82\x46\x5e\xdd\x62\x14\x4c\x0a\xc1\xfc\x5a\x18\x50\x6a\x22\x44\xba\x44\x9a\xc4", 32);
  memcpy(fixed + 32, "\xe6\xdb\x68\x67\x58\x30\x30\xdb\x35\x94\xc1\xa4\x24\xb1\x5f\x7c\x72\x66\x24\xec\x26\xb3\x35\x3b\x10\xa9\x03\xa6\xd0\xab\x1c\x4c", 32);
  id = submit(ACCEL_OP_X25519, fixed, NULL, 0);
  CHECK("submit returns id > 0", id > 0);
  CHECK("second submit while pending -> -1 (busy)", submit(ACCEL_OP_X25519, fixed, NULL, 0) == -1);
  arg[0] = (long)res; arg[1] = sizeof res;
  CHECK("poll before send -> 0", etd_dispatch(21, arg) == 0);
  arg[0] = (long)tx; arg[1] = sizeof tx;
  n = (int)etd_dispatch(19, arg);
  CHECK("ETD 19 drains 2 IP records + control record", n == 2 * (2 + 60) + 2 + 4 + 64);
  CHECK("poll after send -> 0", (arg[0] = (long)res, arg[1] = sizeof res, etd_dispatch(21, arg)) == 0);
  rl = pico_in(tx, n, rx, &ip_seen);
  CHECK("Pico walks past the IP records and finds the control record", ip_seen == 2 && rl > 0);
  z_in(rx, rl);
  arg[0] = (long)res; arg[1] = sizeof res;
  n = (int)etd_dispatch(21, arg);
  CHECK("poll -> response length", n == ACCEL_RES_HDR + 32);
  accel_res_parse(res, n, &rop, &rid, &st, &rsv, &usec, &pr);
  CHECK("response id matches, status OK, RFC 7748 result",
        rid == (unsigned)id && st == 0 &&
        memcmp(pr, "\xc3\xda\x55\x37\x9d\xe9\xc6\x90\x8e\x94\xea\x4d\xf2\x8d\x08\x4f\x32\xec\xcf\x03\x49\x1c\x71\xf7\x54\xb4\x07\x55\x77\xa2\x85\x52", 32) == 0);
  CHECK("poll after take -> -1 (idle)", (arg[0] = (long)res, arg[1] = sizeof res, etd_dispatch(21, arg)) == -1);

  /* 2. 1024 バイトのメッセージの verify (要求 1124 バイト) は ETD 19 の max に入らなければ次回に回る */
  memset(msg, 0x5a, sizeof msg);
  /* RFC 8032 TEST 1 の鍵と署名、メッセージだけ違う (全零の鍵と署名は小位数の点で、Monocypher はそれを弾かずに通す) */
  memcpy(fixed, "\xd7\x5a\x98\x01\x82\xb1\x0a\xb7\xd5\x4b\xfe\xd3\xc9\x64\x07\x3a\x0e\xe1\x72\xf3\xda\xa6\x23\x25\xaf\x02\x1a\x68\xf7\x07\x51\x1a", 32);
  memcpy(fixed + 32, "\xe5\x56\x43\x00\xc3\x60\xac\x72\x90\x86\xe2\xcc\x80\x6e\x82\x8a\x84\x87\x7f\x1e\xb8\xe5\xd9\x74\xd8\x73\xe0\x65\x22\x49\x01\x55"
                     "\x5f\xb8\x82\x15\x90\xa3\x3b\xac\xc6\x1e\x39\x70\x1c\xf9\xb4\x6b\xd2\x5b\xf5\xf0\x59\x5b\xbe\x24\x65\x51\x41\x43\x8e\x7a\x10\x0b", 64);
  id = submit(ACCEL_OP_ED25519_VERIFY, fixed, msg, ACCEL_MSG_MAX);
  CHECK("submit 1124-byte request", id > 0);
  arg[0] = (long)tx; arg[1] = 1000;
  CHECK("ETD 19 with max 1000 keeps it queued", etd_dispatch(19, arg) == 0);
  arg[0] = (long)tx; arg[1] = sizeof tx;
  n = (int)etd_dispatch(19, arg);
  CHECK("ETD 19 with room sends it (1126 bytes)", n == 2 + ACCEL_REQ_MAX);
  rl = pico_in(tx, n, rx, &ip_seen);
  z_in(rx, rl);
  arg[0] = (long)res; arg[1] = sizeof res;
  n = (int)etd_dispatch(21, arg);
  accel_res_parse(res, n, &rop, &rid, &st, &rsv, &usec, &pr);
  CHECK("verify with wrong message -> BADSIG status delivered", n == ACCEL_RES_HDR && st == ACCEL_ST_BADSIG && rid == (unsigned)id);

  /* 3. 取り消し: 取り消した後に届いた応答は捨てる (stale)、その後の新しい要求は通る */
  id = submit(ACCEL_OP_PING, fixed, NULL, 0);
  arg[0] = (long)tx; arg[1] = sizeof tx;
  n = (int)etd_dispatch(19, arg);
  rl = pico_in(tx, n, rx, &ip_seen);
  etd_dispatch(22, 0);
  z_in(rx, rl);
  {
    uint32_t st4[4];                    /* ETD 24 は 32 ビット 4 つを書く (68000 の long) */
    etd_dispatch(24, (long *)st4);
    CHECK("late response after cancel is dropped (stale++)", st4[0] == 0 && st4[3] == 1);
  }
  id = submit(ACCEL_OP_PING, fixed, NULL, 0);
  arg[0] = (long)tx; arg[1] = sizeof tx;
  n = (int)etd_dispatch(19, arg);
  rl = pico_in(tx, n, rx, &ip_seen);
  z_in(rx, rl);
  arg[0] = (long)res; arg[1] = sizeof res;
  n = (int)etd_dispatch(21, arg);
  accel_res_parse(res, n, &rop, &rid, &st, &rsv, &usec, &pr);
  CHECK("ping after cancel: new id, caps 7, version 1", rid == (unsigned)id && st == 0 && rsv == 7 && pr[0] == 1);
  /* 4. 変な要求は預からない */
  arg[0] = (long)"\x45\x00"; arg[1] = 2;
  CHECK("submit without ACCEL_MARK -> -2", etd_dispatch(20, arg) == -2);
  /* 5. 古い npd.x / 古いファームの振る舞い: IPv4 でない先頭バイトのレコードは捨てられ、次のレコードへ進む (歩き方の確認) */
  {
    uint8_t s[2 + 4 + 2 + 60] = { 0, 4, ACCEL_MARK, 0, 0, 1, 0, 60, 0x45 };
    rl = pico_in(s, sizeof s, rx, &ip_seen);
    CHECK("record walker continues past a control record to the IP record", ip_seen == 1);
  }
  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
