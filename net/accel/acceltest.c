/*
 * acceltest.x -- 暗号アクセラレータ (Pico W + npd.sys/npd.x) を RFC 7748 / 8032 のベクタで試し、時間を測る
 *
 *   acceltest.x [-n N]   N 回ずつ繰り返して時間を測る (既定 1)
 * 出力: 項目ごとに OK / FAIL と、Z で頼んでから結果を取るまでの時間 (IOCS _ONTIME、1/100 秒) と Pico の計算時間 (µs)。
 * 前提: CONFIG.SYS に npd.sys 1.1、npd.x が動いていて Pico のファームが accel 入り (PING に答える)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <x68k/iocs.h>
#include "accel.h"

static int fails;

static int unhex(const char *h, uint8_t *out, int max)
{
  int n = 0;
  while (h[0] && h[1] && n < max) {
    int hi = (h[0] <= '9') ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
    int lo = (h[1] <= '9') ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
    out[n++] = (uint8_t)((hi << 4) | lo);
    h += 2;
  }
  return n;
}

static void report(const char *name, int ok, int r)
{
  printf("%s %-34s r=%2d  Z %3u.%02us  Pico %lu us\n", ok ? "OK  " : "FAIL", name, r,
         accel_last_ticks() / 100, accel_last_ticks() % 100, (unsigned long)accel_last_usec());
  if (!ok)
    fails++;
}

/* RFC 7748 5.2 / 6.1 */
static const char *v_scalar = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
static const char *v_point  = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
static const char *v_out    = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
static const char *alice_sk = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
static const char *alice_pk = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
static const char *bob_pk   = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
static const char *shared   = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
/* RFC 8032 7.1 TEST 1..3: seed, pk, msg, sig */
static const char *ed[3][4] = {
  { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
  { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
  { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

int main(int argc, char **argv)
{
  int N = 1, i, k, r, caps;
  uint8_t a[32], b[32], c[32], q[32], pk[32], sig[64], sig2[64], msg[ACCEL_MSG_MAX];
  char name[48];
  for (i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-n") && i + 1 < argc)
      N = atoi(argv[++i]);

  caps = accel_open();
  if (caps < 0) {
    printf("FAIL accel_open = %d (%s)\n", caps, caps == -1 ? "npd.sys が無い" : "Pico が答えない (ファームが古い / npd.x が動いていない)");
    return 1;
  }
  report("ping", 1, caps);
  printf("caps: %s%s%s\n", caps & ACCEL_CAP_VERIFY ? "verify " : "", caps & ACCEL_CAP_X25519 ? "x25519 " : "",
         caps & ACCEL_CAP_SIGN ? "sign" : "");

  for (k = 0; k < N; k++) {
    /* X25519 */
    unhex(v_scalar, a, 32); unhex(v_point, b, 32); unhex(v_out, c, 32);
    r = accel_x25519(q, a, b);
    report("RFC7748 5.2 X25519", r == 0 && !memcmp(q, c, 32), r);
    unhex(alice_sk, a, 32); unhex(alice_pk, c, 32);
    r = accel_x25519_base(q, a);
    report("RFC7748 6.1 keygen (base 9)", r == 0 && !memcmp(q, c, 32), r);
    unhex(bob_pk, b, 32); unhex(shared, c, 32);
    r = accel_x25519(q, a, b);
    report("RFC7748 6.1 shared", r == 0 && !memcmp(q, c, 32), r);

    /* Ed25519 */
    for (i = 0; i < 3; i++) {
      int mlen = unhex(ed[i][2], msg, sizeof msg);
      unhex(ed[i][0], a, 32); unhex(ed[i][1], pk, 32); unhex(ed[i][3], sig, 64);
      r = accel_ed25519_sign(sig2, msg, mlen, a, pk);
      sprintf(name, "RFC8032 TEST %d sign (%d B)", i + 1, mlen);
      report(name, r == 0 && !memcmp(sig, sig2, 64), r);
      r = accel_ed25519_verify(msg, mlen, sig, pk);
      sprintf(name, "RFC8032 TEST %d verify", i + 1);
      report(name, r == 1, r);
    }
    /* 改ざんした署名は弾く (verify は 0 を返す。-1 は「使えない」) */
    sig[10] ^= 1;
    r = accel_ed25519_verify(msg, 2, sig, pk);
    report("tampered signature rejected", r == 0, r);
    sig[10] ^= 1;
    /* 長いメッセージ (上限 1024 = 3 セクタの要求): 署名して検証 */
    for (i = 0; i < ACCEL_MSG_MAX; i++)
      msg[i] = (uint8_t)(i * 7 + 3);
    r = accel_ed25519_sign(sig2, msg, ACCEL_MSG_MAX, a, pk);
    report("sign 1024-byte message", r == 0, r);
    r = accel_ed25519_verify(msg, ACCEL_MSG_MAX, sig2, pk);
    report("verify 1024-byte message", r == 1, r);
    msg[500] ^= 0x80;
    r = accel_ed25519_verify(msg, ACCEL_MSG_MAX, sig2, pk);
    report("verify 1024-byte tampered -> 0", r == 0, r);
    /* ssh の実際の大きさ: ホスト鍵の検証はハッシュ 32 バイト、認証の署名は 200 バイト前後 */
    r = accel_ed25519_sign(sig2, msg, 200, a, pk);
    report("sign 200-byte (userauth size)", r == 0, r);
    r = accel_ed25519_verify(msg, 200, sig2, pk);
    report("verify 200-byte", r == 1, r);
    r = accel_ed25519_verify(msg, 32, sig2, pk);
    report("verify 32-byte (hostkey size, bad)", r == 0, r);
  }
  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
