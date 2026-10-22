/* accel_tool -- Pico 側の計算本体 (accel_ops.c + Monocypher) をホストで叩く。
 * 要求を accel_req_build で組み、accel_compute に通し、応答を accel_res_parse でほどく (記録形式も一緒に試す)。
 *   ping                        -> "version caps"
 *   mul  SCALAR32 POINT32       -> X25519 (hex 32)
 *   base SCALAR32               -> X25519 基点 (hex 32)
 *   edsign SEED32 PK32 MSGhex   -> 署名 (hex 64)
 *   edverify PK32 SIG64 MSGhex  -> "1" (ok) / "0" (bad)
 *   roundtrip N                 -> 記録形式の組み立て / 解読の往復を N 回 (乱数の長さ)。"OK" か "FAIL ..."
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../accel_proto.h"
#include "../accel_ops.h"

static int unhex(const char *h, unsigned char *out, int max)
{
  int n = 0;
  while (h[0] && h[1] && n < max) {
    int hi = (h[0] <= '9') ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
    int lo = (h[1] <= '9') ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
    out[n++] = (unsigned char)((hi << 4) | lo);
    h += 2;
  }
  return n;
}
static void phex(const unsigned char *b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

/* 要求 → 計算 → 応答。戻り値: status (応答が変なら -1)。result/rlen に結果 */
static int run(int op, const uint8_t *fixed, const uint8_t *msg, int mlen, uint8_t *result, int *rlen, int *reserved)
{
  static uint8_t req[ACCEL_REQ_MAX], res[ACCEL_RES_MAX];
  unsigned id = 0x1234, rid;
  int n = accel_req_build(req, op, id, fixed, msg, mlen), rop, st, rsv, m;
  uint32_t usec;
  const uint8_t *r;
  if (n < 0) return -1;
  m = accel_compute(req, n, res, NULL);
  m = accel_res_parse(res, m, &rop, &rid, &st, &rsv, &usec, &r);
  if (m < 0 || rop != op || rid != id) return -1;
  if (st == ACCEL_ST_OK && m != accel_res_len(op)) return -1;
  memcpy(result, r, m);
  *rlen = m;
  if (reserved) *reserved = rsv;
  return st;
}

int main(int argc, char **argv)
{
  uint8_t fixed[96], msg[ACCEL_MSG_MAX], out[64];
  int rlen, st, rsv, mlen;
  if (argc < 2) { fprintf(stderr, "usage: ping|mul|base|edsign|edverify|roundtrip ...\n"); return 2; }
  if (!strcmp(argv[1], "ping")) {
    st = run(ACCEL_OP_PING, fixed, NULL, 0, out, &rlen, &rsv);
    printf("%d %d\n", st == 0 ? out[0] : -1, rsv); return 0;
  }
  if (!strcmp(argv[1], "mul") && argc == 4) {
    unhex(argv[2], fixed, 32); unhex(argv[3], fixed + 32, 32);
    st = run(ACCEL_OP_X25519, fixed, NULL, 0, out, &rlen, NULL);
    if (st) { printf("ERR %d\n", st); return 1; }
    phex(out, 32); return 0;
  }
  if (!strcmp(argv[1], "base") && argc == 3) {
    unhex(argv[2], fixed, 32);
    st = run(ACCEL_OP_X25519_BASE, fixed, NULL, 0, out, &rlen, NULL);
    if (st) { printf("ERR %d\n", st); return 1; }
    phex(out, 32); return 0;
  }
  if (!strcmp(argv[1], "edsign") && argc == 5) {
    unhex(argv[2], fixed, 32); unhex(argv[3], fixed + 32, 32); mlen = unhex(argv[4], msg, sizeof msg);
    st = run(ACCEL_OP_ED25519_SIGN, fixed, msg, mlen, out, &rlen, NULL);
    if (st) { printf("ERR %d\n", st); return 1; }
    phex(out, 64); return 0;
  }
  if (!strcmp(argv[1], "edverify") && argc == 5) {
    unhex(argv[2], fixed, 32); unhex(argv[3], fixed + 32, 64); mlen = unhex(argv[4], msg, sizeof msg);
    st = run(ACCEL_OP_ED25519_VERIFY, fixed, msg, mlen, out, &rlen, NULL);
    printf("%d\n", st == ACCEL_ST_OK ? 1 : 0); return 0;
  }
  if (!strcmp(argv[1], "roundtrip") && argc == 3) {
    int N = atoi(argv[2]), i, k;
    static uint8_t req[ACCEL_REQ_MAX], res[ACCEL_RES_MAX];
    srand(20261022);
    for (i = 0; i < N; i++) {
      int op = rand() % 5, f = accel_req_fixed(op), ml = accel_req_has_msg(op) ? rand() % (ACCEL_MSG_MAX + 1) : 0;
      int n, rop, m, rst, rrsv, rl = accel_res_len(op);
      unsigned id = rand() & 0xffff, rid;
      const uint8_t *pf, *pm, *pr;
      uint32_t usec, rusec = (uint32_t)rand();
      for (k = 0; k < f; k++) fixed[k] = rand();
      for (k = 0; k < ml; k++) msg[k] = rand();
      n = accel_req_build(req, op, id, fixed, msg, ml);
      if (n != ACCEL_REQ_HDR + f + ml) { printf("FAIL build op%d n=%d\n", op, n); return 1; }
      m = accel_req_parse(req, n, &rop, &rid, &pf, &pm);
      if (m != ml || rop != op || rid != id || memcmp(pf, fixed, f) || memcmp(pm, msg, ml)) { printf("FAIL parse op%d\n", op); return 1; }
      /* 長さを削った要求は弾かれる (固定引数が足りない) */
      if (f && accel_req_parse(req, ACCEL_REQ_HDR + f - 1, &rop, &rid, &pf, &pm) != -1) { printf("FAIL short accepted op%d\n", op); return 1; }
      /* メッセージが上限を超えた要求は組めない */
      if (accel_req_has_msg(op) && accel_req_build(req, op, id, fixed, msg, ACCEL_MSG_MAX + 1) != -1) { printf("FAIL over-long accepted\n"); return 1; }
      for (k = 0; k < rl; k++) out[k] = rand();
      n = accel_res_build(res, op, id, i & 7, i & 255, rusec, out, rl);
      m = accel_res_parse(res, n, &rop, &rid, &rst, &rrsv, &usec, &pr);
      if (m != rl || rop != op || rid != id || rst != (i & 7) || rrsv != (i & 255) || usec != rusec || memcmp(pr, out, rl)) { printf("FAIL res op%d\n", op); return 1; }
      /* 違う印は制御レコードではない */
      req[0] = 0x45;
      if (accel_req_parse(req, ACCEL_REQ_HDR + f + ml, &rop, &rid, &pf, &pm) != -1) { printf("FAIL mark\n"); return 1; }
    }
    /* 壊れた要求でも accel_compute は id を返す */
    {
      int rop, rst, rrsv, m; unsigned rid; uint32_t usec; const uint8_t *pr;
      req[0] = ACCEL_MARK; req[1] = ACCEL_OP_X25519; req[2] = 0xab; req[3] = 0xcd;
      m = accel_compute(req, 10, res, NULL);
      accel_res_parse(res, m, &rop, &rid, &rst, &rrsv, &usec, &pr);
      if (rid != 0xabcd || rst != ACCEL_ST_BADREQ) { printf("FAIL badreq st=%d\n", rst); return 1; }
      req[1] = 99;
      m = accel_compute(req, 4, res, NULL);
      accel_res_parse(res, m, &rop, &rid, &rst, &rrsv, &usec, &pr);
      if (rid != 0xabcd || rst != ACCEL_ST_BADOP) { printf("FAIL badop st=%d\n", rst); return 1; }
    }
    printf("OK %d\n", N); return 0;
  }
  fprintf(stderr, "bad args\n"); return 2;
}
