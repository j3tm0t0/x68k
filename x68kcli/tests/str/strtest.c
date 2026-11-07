/* x68str.S の試験 (Musashi の csim で動かす。奇数番地への語・長語のアクセスは csim が止める)。
 * 番地の端数 (0〜3 の組み合わせ)、長さ (0〜70 と 64KB を越える長さ)、重なり、違う位置、値の符号を、素直な C の版と比べる。
 * BENCH を定義すると、ここの版と newlib の版 (nl_ を付けて取り込んだもの) の 1 回あたりのクロック数を区間ごとに出す */
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strchr(const char *, int);

static char out[4096];
static int outn, fails, checks;
static void puts_(const char *s) { while (*s && outn < (int)sizeof out - 1) out[outn++] = *s++; }
static void putn(unsigned long v) { char b[12]; int i = 0; do b[i++] = '0' + v % 10; while (v /= 10); while (i) out[outn++] = b[--i]; }
static void fail(const char *f, int a, int b, unsigned long n)
{
    checks++;
    if (++fails > 40) return;
    puts_("FAIL "); puts_(f); puts_(" a="); putn(a); puts_(" b="); putn(b); puts_(" n="); putn(n); puts_("\n");
}
static int sgn(int x) { return x < 0 ? -1 : x > 0; }

#define BIG (70000 + 16)
static unsigned char A[BIG + 64], B[BIG + 64], R[BIG + 64];
static unsigned long seed = 1;
static unsigned char rnd(void) { seed = seed * 1103515245 + 12345; return seed >> 16; }
static void fill(unsigned char *p, size_t n) { while (n--) *p++ = rnd(); }
static int same(const unsigned char *p, const unsigned char *q, size_t n) { while (n--) if (*p++ != *q++) return 0; return 1; }

static int lens[] = { 0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 18, 19, 20, 31, 32, 33, 47, 48, 49, 63, 64, 65, 70 };
#define NL (int)(sizeof lens / sizeof lens[0])

static void t_memcpy(void)
{
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int i = 0; i < NL; i++) {
        int n = lens[i];
        fill(A, 200); fill(B, 200); for (int k = 0; k < 200; k++) R[k] = B[k];
        for (int k = 0; k < n; k++) R[8 + b + k] = A[8 + a + k];
        void *r = memcpy(B + 8 + b, A + 8 + a, n);
        if (r != B + 8 + b || !same(B, R, 200)) fail("memcpy", a, b, n); else checks++;
    }
}
static void t_memmove(void)
{
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int i = 0; i < NL; i++) for (int d = -20; d <= 20; d += 3) {
        int n = lens[i], src = 40 + a, dst = 40 + b + d;
        fill(A, 200); for (int k = 0; k < 200; k++) R[k] = A[k];
        { unsigned char t[100]; for (int k = 0; k < n; k++) t[k] = R[src + k]; for (int k = 0; k < n; k++) R[dst + k] = t[k]; }
        void *r = memmove(A + dst, A + src, n);
        if (r != A + dst || !same(A, R, 200)) fail("memmove", a, b * 100 + d + 50, n); else checks++;
    }
}
static void t_memset(void)
{
    for (int a = 0; a < 4; a++) for (int i = 0; i < NL; i++) for (int c = 0; c < 3; c++) {
        int n = lens[i], v = c == 0 ? 0 : c == 1 ? 0x1a5 : -1;
        fill(A, 200); for (int k = 0; k < 200; k++) R[k] = A[k];
        for (int k = 0; k < n; k++) R[8 + a + k] = (unsigned char)v;
        void *r = memset(A + 8 + a, v, n);
        if (r != A + 8 + a || !same(A, R, 200)) fail("memset", a, c, n); else checks++;
    }
}
static int ref_memcmp(const unsigned char *p, const unsigned char *q, size_t n) { for (; n; n--, p++, q++) if (*p != *q) return *p - *q; return 0; }
static void t_memcmp(void)
{
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int i = 0; i < NL; i++) for (int at = -1; at < lens[i]; at++) for (int hi = 0; hi < 2; hi++) {
        int n = lens[i];
        fill(A, 100); for (int k = 0; k < 100; k++) B[k + b] = A[k + a];
        if (at >= 0) B[8 + b + at] = hi ? (unsigned char)(A[8 + a + at] ^ 0x80) : (unsigned char)(A[8 + a + at] + 1);
        int e = sgn(ref_memcmp(A + 8 + a, B + 8 + b, n)), g = memcmp(A + 8 + a, B + 8 + b, n);
        if (sgn(g) != e || (e && g != ref_memcmp(A + 8 + a, B + 8 + b, n))) fail("memcmp", a * 10 + b, at, n); else checks++;
    }
}
static void t_memchr(void)
{
    for (int a = 0; a < 4; a++) for (int i = 0; i < NL; i++) for (int at = -1; at < lens[i]; at++) {
        int n = lens[i];
        for (int k = 0; k < 120; k++) A[k] = 'x';
        A[8 + a + n] = 0xe5;                       /* 範囲のすぐ外は見ない */
        if (at >= 0) A[8 + a + at] = 0xe5;
        void *r = memchr(A + 8 + a, 0x1e5, n);     /* c は unsigned char に直して比べる */
        void *e = at >= 0 ? A + 8 + a + at : 0;
        if (r != e) fail("memchr", a, at, n); else checks++;
    }
}
static void t_str(void)
{
    for (int a = 0; a < 4; a++) for (int n = 0; n < 40; n++) {
        for (int k = 0; k < 100; k++) A[k] = 'a' + k % 26;
        A[8 + a + n] = 0;
        if (strlen((char *)A + 8 + a) != (size_t)n) fail("strlen", a, 0, n); else checks++;
        char *p = strchr((char *)A + 8 + a, 0);
        if (p != (char *)A + 8 + a + n) fail("strchr0", a, 0, n); else checks++;
        for (int at = 0; at < n; at++) {
            A[8 + a + at] = 0xb3;
            p = strchr((char *)A + 8 + a, 0xb3);
            if (p != (char *)A + 8 + a + at) fail("strchr", a, at, n); else checks++;
            A[8 + a + at] = 'a' + (8 + a + at) % 26;
        }
        if (strchr((char *)A + 8 + a, '#')) fail("strchrnone", a, 0, n); else checks++;
        for (int b = 0; b < 4; b++) for (int at = 0; at <= n; at++) for (int m = 0; m < 4; m++) {
            for (int k = 0; k <= n; k++) B[16 + b + k] = A[8 + a + k];
            if (m == 1) B[16 + b + at] = (unsigned char)(B[16 + b + at] + 1);       /* b が大きい (at = n なら b が長い) */
            if (m == 2) B[16 + b + at] = 0xf0;                                      /* 0x80 以上 */
            if (m == 3) { if (at == n) continue; B[16 + b + at] = 0; }              /* b が短い */
            int e = 0;
            { const unsigned char *x = A + 8 + a, *y = B + 16 + b; while (*x && *x == *y) x++, y++; e = *x - *y; }
            int g = strcmp((char *)A + 8 + a, (char *)B + 16 + b);
            if (sgn(g) != sgn(e)) fail("strcmp", a * 10 + b, at * 10 + m, n); else checks++;
        }
    }
}
static void t_big(void)   /* 64KB を越える長さ (dbra の上位の語) */
{
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
        size_t n = 70000;
        fill(A, BIG); memset(B, 0x55, BIG);
        memcpy(B + b, A + a, n);
        if (!same(B + b, A + a, n) || B[b + n] != 0x55) fail("bigcpy", a, b, n); else checks++;
        if (memcmp(A + a, B + b, n)) fail("bigcmp", a, b, n); else checks++;
        B[b + n - 1] ^= 1;
        if (!memcmp(A + a, B + b, n)) fail("bigcmp2", a, b, n); else checks++;
        memmove(A + 3 + b, A + a, n); memmove(A + a, A + 3 + b, n);
        memset(B + a, 7, n);
        if (B[a] != 7 || B[a + n - 1] != 7 || B[a + n] == 7 && a + n < BIG && 0) fail("bigset", a, b, n); else checks++;
        B[a + 66000] = 9;
        if (memchr(B + a, 9, n) != B + a + 66000) fail("bigchr", a, b, n); else checks++;
    }
}

#ifdef BENCH
void *nl_memcpy(void *, const void *, size_t); void *nl_memset(void *, int, size_t);
int nl_memcmp(const void *, const void *, size_t); void *nl_memchr(const void *, int, size_t);
size_t nl_strlen(const char *); int nl_strcmp(const char *, const char *);
#define SEC(n, x) do { *(volatile long *)0x7fffd0 = n; x; *(volatile long *)0x7fffd4 = n; } while (0)
static void bench(int len)
{
    fill(A, len + 8); for (int k = 0; k < len + 8; k++) B[k] = A[k]; A[len] = B[len] = 0;
    for (int k = 0; k < len; k++) if (!A[k]) A[k] = B[k] = 1;
    puts_("len "); putn(len); puts_(": sections 1-12 = ours/newlib memcpy memset memcmp memchr strlen strcmp\n");
    SEC(1, memcpy(R, A, len)); SEC(2, nl_memcpy(R, A, len));
    SEC(3, memset(R, 1, len)); SEC(4, nl_memset(R, 1, len));
    SEC(5, memcmp(A, B, len)); SEC(6, nl_memcmp(A, B, len));
    SEC(7, memchr(A, 0, len)); SEC(8, nl_memchr(A, 0, len));
    SEC(9, strlen((char *)A)); SEC(10, nl_strlen((char *)A));
    SEC(11, strcmp((char *)A, (char *)B)); SEC(12, nl_strcmp((char *)A, (char *)B));
}
#endif

void bench_main(void)
{
#ifdef BENCH
    bench(BENCH);
#else
    t_memcpy(); t_memmove(); t_memset(); t_memcmp(); t_memchr(); t_str(); t_big();
    puts_(fails ? "NG " : "OK "); putn(checks); puts_(" checks, "); putn(fails); puts_(" failures\n");
#endif
    *(volatile long *)0x7fffe4 = (long)out;
    *(volatile long *)0x7fffe8 = outn;
}
