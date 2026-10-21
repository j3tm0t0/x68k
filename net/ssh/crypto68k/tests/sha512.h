/* テスト用の小さな SHA-512 (dropbear/libtomcrypt と同じ呼び出し名)。 */
#ifndef SHA512_H
#define SHA512_H
#include <stdint.h>
typedef struct { uint64_t state[8], length; unsigned char buf[128]; unsigned curlen; } hash_state;
void sha512_init(hash_state *md);
int  sha512_process(hash_state *md, const unsigned char *in, unsigned long inlen);
int  sha512_done(hash_state *md, unsigned char *out);
#endif
