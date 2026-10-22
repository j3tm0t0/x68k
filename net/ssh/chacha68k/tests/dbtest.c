/* csim 用: パッチを当てた dropbear の chachapoly.o (CHACHA68K) をそのまま呼ぶ。入力は bench.c と同じ
 * (鍵 64 + seq 4 + 平文)。encrypt の暗号文 + タグを 0x900000 に出し (ref.py と比べる)、続けて getlength・
 * その場の decrypt (平文に戻るか)・1 バイト改ざんした decrypt (CRYPT_ERROR になるか) を確かめ、違えば abort。 */
#include <string.h>
#include "tomcrypt.h"
/* dropbear の algo.h / chachapoly.h から必要な分だけ (includes.h は libtommath と衝突するので) */
struct dropbear_cipher_mode {
  int (*start)(int, const unsigned char *, const unsigned char *, int, int, void *);
  int (*encrypt)(const unsigned char *, unsigned char *, unsigned long, void *);
  int (*decrypt)(const unsigned char *, unsigned char *, unsigned long, void *);
  int (*aead_crypt)(unsigned int, const unsigned char *, unsigned char *, unsigned long, unsigned long, void *, int);
  int (*aead_getlength)(unsigned int, const unsigned char *, unsigned int *, unsigned long, void *);
  const void *aead_mac;
};
typedef struct { chacha_state chacha; chacha_state header; } dropbear_chachapoly_state;
extern const struct dropbear_cipher_mode dropbear_mode_chachapoly;
int constant_time_memcmp(const void *a, const void *b, size_t n)
{
  const unsigned char *p = a, *q = b; int c = 0;
  while (n--) c |= *p++ ^ *q++;
  return c;
}
void abort(void);
static unsigned char buf[40016];
void bench(void)
{
  unsigned char *in = (unsigned char *)0x800000, *out = (unsigned char *)0x900000;
  unsigned long n = *(unsigned long *)0x7ffff4, len = n - 68;
  unsigned int seq = (in[64] << 24) | (in[65] << 16) | (in[66] << 8) | in[67], plen = 0;
  const struct dropbear_cipher_mode *m = &dropbear_mode_chachapoly;
  dropbear_chachapoly_state st;
  *(volatile unsigned long *)0x7fffe0 = 1;
  if (m->start(0, NULL, in, 64, 20, &st) != CRYPT_OK) abort();
  if (m->aead_crypt(seq, in + 68, out, len, 16, &st, LTC_ENCRYPT) != CRYPT_OK) abort();
  if (m->aead_getlength(seq, out, &plen, len, &st) != CRYPT_OK) abort();
  if (plen != (unsigned)((in[68] << 24) | (in[69] << 16) | (in[70] << 8) | in[71])) abort();
  memcpy(buf, out, len + 16);
  if (m->aead_crypt(seq, buf, buf, len, 16, &st, LTC_DECRYPT) != CRYPT_OK) abort();
  if (memcmp(buf, in + 68, len)) abort();
  memcpy(buf, out, len + 16);
  buf[(seq * 7 + len) % (len + 16)] ^= 0x10;
  if (m->aead_crypt(seq, buf, buf, len, 16, &st, LTC_DECRYPT) == CRYPT_OK) abort();
  *(volatile unsigned long *)0x7fffe4 = (unsigned long)out;
  *(volatile unsigned long *)0x7fffe8 = len + 16;
}
