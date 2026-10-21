/* x25519_tool -- nacl25519.c (ref または FAST_FIELD) を叩いて 16 進で結果を出す。
 * 差分テスト (ref vs fast) と RFC ベクタ照合に使う。
 *   mul  SCALAR32 POINT32      -> X25519 scalarmult (q)
 *   base SCALAR32              -> X25519 with base point 9
 *   edkey SEED                 -> ed25519 make_key: PK | SK
 *   edsign SK64 PK32 MSGhex    -> 署名 (64 バイト)
 *   edverify PK32 SIG64 MSGhex -> "1" (ok) / "0" (ng)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nacl25519.c"

void genrandom_seed(u64 s);

static int unhex(const char *h, unsigned char *out, int max)
{
  int n = 0;
  while (h[0] && h[1] && n < max) {
    int hi, lo;
    char a = h[0], b = h[1];
    hi = (a <= '9') ? a - '0' : (a | 32) - 'a' + 10;
    lo = (b <= '9') ? b - '0' : (b | 32) - 'a' + 10;
    out[n++] = (unsigned char)((hi << 4) | lo);
    h += 2;
  }
  return n;
}
static void phex(const unsigned char *b, int n) { int i; for (i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: mul|base|edkey|edsign|edverify ...\n"); return 2; }
  if (!strcmp(argv[1], "mul") && argc == 4) {
    unsigned char n[32], p[32], q[32];
    unhex(argv[2], n, 32); unhex(argv[3], p, 32);
    dropbear_curve25519_scalarmult(q, n, p);
    phex(q, 32); return 0;
  }
  if (!strcmp(argv[1], "base") && argc == 3) {
    unsigned char n[32], p[32], q[32];
    memset(p, 0, 32); p[0] = 9;
    unhex(argv[2], n, 32);
    dropbear_curve25519_scalarmult(q, n, p);
    phex(q, 32); return 0;
  }
  if (!strcmp(argv[1], "edkey") && argc == 3) {
    unsigned char pk[32], sk[32];
    genrandom_seed((u64)strtoull(argv[2], 0, 16));
    dropbear_ed25519_make_key(pk, sk);
    phex(pk, 32); phex(sk, 32); return 0;
  }
  if (!strcmp(argv[1], "edsign") && argc == 5) {
    unsigned char sk[32], pk[32], m[4096], s[64]; uint32_t slen = 0;
    int mlen;
    unhex(argv[2], sk, 32); unhex(argv[3], pk, 32); mlen = unhex(argv[4], m, sizeof m);
    dropbear_ed25519_sign(m, mlen, s, &slen, sk, pk);
    phex(s, (int)slen); return 0;
  }
  if (!strcmp(argv[1], "edverify") && argc == 5) {
    unsigned char pk[32], sig[64], m[4096]; int mlen, r;
    unhex(argv[2], pk, 32); unhex(argv[3], sig, 64); mlen = unhex(argv[4], m, sizeof m);
    r = dropbear_ed25519_verify(m, mlen, sig, 64, pk);
    printf("%d\n", r == 0 ? 1 : 0); return 0;
  }
  fprintf(stderr, "bad args\n"); return 2;
}
