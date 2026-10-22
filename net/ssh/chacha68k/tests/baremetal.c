/* csim 用の最小 libc (libtomcrypt と bench が使う分だけ)。 */
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n) { char *p = d; const char *q = s; if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; } return d; }
void *memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *p = a, *q = b; for (; n; n--, p++, q++) if (*p != *q) return *p - *q; return 0; }
void abort(void) { *(volatile long *)0x7fffe0 = 3; for (;;) ; }
int fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
void *_impure_ptr;
void m_burn(void *p, unsigned int n) { volatile char *c = p; while (n--) *c++ = 0; }
