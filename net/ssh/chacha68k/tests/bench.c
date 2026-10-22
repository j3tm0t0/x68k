/* csim 用: dropbear の chachapoly.c の encrypt と同じ手順で 1 パケットを暗号化し、chacha と poly1305 の
 * クロックを区間 1 / 2 に分けて数える。入力は 0x800000 (長さ 0x7ffff4)、先頭 64 バイトを鍵、次の 4 バイトを seq
 * (BE)、残りを平文 (len >= 4) とする。出力 (暗号文 + タグ) を 0x900000 に置く。 */
#include <string.h>
#include "tomcrypt.h"
#define SEC_ON(n)  (*(volatile unsigned long *)0x7fffd0 = (n))
#define SEC_OFF(n) (*(volatile unsigned long *)0x7fffd4 = (n))
void bench(void)
{
  unsigned char *in = (unsigned char *)0x800000, *out = (unsigned char *)0x900000;
  unsigned long n = *(unsigned long *)0x7ffff4, len = n - 68, taglen = 16;
  unsigned char seqbuf[8], pk[64];
  chacha_state ch, hd;
  poly1305_state poly;
  const unsigned char *pt = in + 68;
  chacha_setup(&ch, in, 32, 20);
  chacha_setup(&hd, in + 32, 32, 20);
  memset(seqbuf, 0, 4); memcpy(seqbuf + 4, in + 64, 4);
  *(volatile unsigned long *)0x7fffe0 = 1;
  SEC_ON(1);
  chacha_ivctr64(&ch, seqbuf, 8, 0);
  chacha_keystream(&ch, pk, 64);
  chacha_ivctr64(&hd, seqbuf, 8, 0);
  chacha_crypt(&hd, pt, 4, out);
  chacha_ivctr64(&ch, seqbuf, 8, 1);
  chacha_crypt(&ch, pt + 4, len - 4, out + 4);
  SEC_OFF(1);
  SEC_ON(2);
  poly1305_init(&poly, pk, 32);
  poly1305_process(&poly, out, len);
  poly1305_done(&poly, out + len, &taglen);
  SEC_OFF(2);
  *(volatile unsigned long *)0x7fffe4 = (unsigned long)out;
  *(volatile unsigned long *)0x7fffe8 = len + 16;
}
