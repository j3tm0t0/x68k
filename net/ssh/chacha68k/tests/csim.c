/* 68000 の命令サイクル数で暗号ルーチンを測る (Musashi。MULU は 38+2n を数える)。
 * usage: csim BIN NMFILE [INFILE [OUTFILE]]
 *   BIN は 0x1000 に置く素のバイナリ (start.s から bench() を呼ぶ)。INFILE は 0x800000 に置き、長さを 0x7ffff4 に書く。
 *   bench が 0x7fffe0 に 1 を書いてから 2 を書くまでのクロック数を出す。0x7fffd0 に n (1..15) を書くと区間 n の開始、
 *   0x7fffd4 に n で終了 (区間ごとの合計を出す)。出力は 0x7fffe4 (アドレス) / 0x7fffe8 (長さ) の範囲を OUTFILE に書く。
 *   環境変数 PROF=1 で 1 命令ずつ実行して関数ごとのクロックを出す (nm の T/t)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
static unsigned char mem[0x1000000];
static unsigned long long cyc, sec_t0[16], sec_sum[16];
static unsigned get32(unsigned a) { a &= 0xffffff; return (mem[a] << 24) | (mem[a + 1] << 16) | (mem[a + 2] << 8) | mem[a + 3]; }
static void put32(unsigned a, unsigned v) { a &= 0xffffff; mem[a] = v >> 24; mem[a + 1] = v >> 16; mem[a + 2] = v >> 8; mem[a + 3] = v; }
unsigned int m68k_read_memory_8(unsigned int a) { return mem[a & 0xffffff]; }
unsigned int m68k_read_memory_16(unsigned int a) { a &= 0xffffff; if (a & 1) { fprintf(stderr, "odd word read %06x pc=%06x\n", a, m68k_get_reg(NULL, M68K_REG_PPC)); exit(2); } return (mem[a] << 8) | mem[a + 1]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { mem[a & 0xffffff] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { a &= 0xffffff; if (a & 1) { fprintf(stderr, "odd word write %06x pc=%06x\n", a, m68k_get_reg(NULL, M68K_REG_PPC)); exit(2); } mem[a] = v >> 8; mem[a + 1] = v; }
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
  m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v);
  a &= 0xffffff;
  if (a == 0x7fffd0 && v < 16) sec_t0[v] = cyc + m68k_cycles_run();
  if (a == 0x7fffd4 && v < 16) sec_sum[v] += cyc + m68k_cycles_run() - sec_t0[v];
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
static long fload(const char *p, unsigned char *dst, long max) { FILE *f = fopen(p, "rb"); long n; if (!f) { perror(p); exit(1); } n = fread(dst, 1, max, f); fclose(f); return n; }
static unsigned fa[8192]; static char fn[8192][64]; static unsigned long long hits[8192];
static unsigned long long pcc[0x80000];
int main(int argc, char **argv)
{
  char line[256]; FILE *nm; int nf = 0, prof = getenv("PROF") != 0, i;
  unsigned long long cyc0 = 0;
  if (argc < 3) { fprintf(stderr, "usage: csim BIN NM [IN [OUT]]\n"); return 1; }
  fload(argv[1], mem + 0x1000, 0x700000);
  if (argc > 3) put32(0x7ffff4, fload(argv[3], mem + 0x800000, 0x300000));
  nm = fopen(argv[2], "r");
  while (fgets(line, sizeof line, nm) && nf < 8192) { unsigned a; char t, s2[200]; if (sscanf(line, "%x %c %199s", &a, &t, s2) == 3 && (t == 'T' || t == 't')) { fa[nf] = a; strncpy(fn[nf], s2, 63); nf++; } }
  fclose(nm);
  { int j; for (i = 0; i < nf; i++) for (j = i + 1; j < nf; j++) if (fa[j] < fa[i]) { unsigned x = fa[i]; char b[64]; fa[i] = fa[j]; fa[j] = x; memcpy(b, fn[i], 64); memcpy(fn[i], fn[j], 64); memcpy(fn[j], b, 64); } }
  put32(0, 0xbf0000); put32(4, 0x1000);
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  for (;;) {
    unsigned pc = m68k_get_reg(NULL, M68K_REG_PC); int c = m68k_execute(prof ? 1 : 100);
    if (prof && cyc0 && pc < 0x100000) pcc[pc >> 1] += c;
    cyc += c;
    if (get32(0x7fffe0) == 1 && !cyc0) cyc0 = cyc;
    if (get32(0x7fffe0) >= 2) break;
    if (cyc > 200000000000ULL) { fprintf(stderr, "runaway\n"); return 1; }
  }
  if (get32(0x7fffe0) == 3) { fprintf(stderr, "abort()\n"); return 1; }
  if (argc > 4) { FILE *f = fopen(argv[4], "wb"); fwrite(mem + (get32(0x7fffe4) & 0xffffff), 1, get32(0x7fffe8), f); fclose(f); }
  if (prof) {
    unsigned a; int k = -1; unsigned long long tot = 0;
    for (a = 0; a < 0x80000; a++) if (pcc[a]) { while (k + 1 < nf && fa[k + 1] <= a * 2) k++; if (k >= 0) hits[k] += pcc[a]; }
    for (i = 0; i < nf; i++) tot += hits[i];
    for (;;) { int b = -1; for (i = 0; i < nf; i++) if (hits[i] && (b < 0 || hits[i] > hits[b])) b = i; if (b < 0 || hits[b] * 200 < tot) break; printf("  %5.1f%%  %12llu  %s\n", 100.0 * hits[b] / tot, hits[b], fn[b]); hits[b] = 0; }
  }
  for (i = 1; i < 16; i++) if (sec_sum[i]) printf("section %d: %llu cyc\n", i, sec_sum[i]);
  printf("total %llu cyc  %.3f s @10MHz\n", cyc - cyc0, (cyc - cyc0) / 10e6);
  return 0;
}
