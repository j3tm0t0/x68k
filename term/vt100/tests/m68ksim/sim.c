/* 68000 の命令サイクル数で vt_write の時間を見積もる (Musashi。テキスト VRAM のウェイトと TXRASCOPY の待ちを足す)。
 * usage: sim BIN NMFILE CGROM FIXTURE CHUNK COLS ROWS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
static unsigned char mem[0x1000000];
static unsigned vram_at = 0xffffffffu, pal_at;   /* vt_host_vram / vt_host_pal のアドレス */
static unsigned long long extra;          /* TXRASCOPY の待ち (CPU は IOCS 内で H-sync を待って止まる) */
static long ntxras;
#define RASTER_CYC 318                    /* 1 ラスタ (4 ライン) = 水平 1 周期 31.5kHz ≒ 31.8us = 318 クロック @10MHz */
static void txras(void)
{
  unsigned d1 = (mem[0x7fffd4] << 24) | (mem[0x7fffd5] << 16) | (mem[0x7fffd6] << 8) | mem[0x7fffd7];
  unsigned d2 = (mem[0x7fffd8] << 24) | (mem[0x7fffd9] << 16) | (mem[0x7fffda] << 8) | mem[0x7fffdb];
  unsigned d3 = (mem[0x7fffdc] << 24) | (mem[0x7fffdd] << 16) | (mem[0x7fffde] << 8) | mem[0x7fffdf];
  unsigned src = (d1 >> 8) & 0xff, dst = d1 & 0xff, n = d2 & 0xffff, p, i;
  for (i = 0; i < n; i++) {   /* IPLROM と同じく 1 ラスタずつ (bit15 なら番号を減らしながら) */
    unsigned s_ = (d3 & 0x8000) ? src - i : src + i, d_ = (d3 & 0x8000) ? dst - i : dst + i;
    for (p = 0; p < 4; p++) if (d3 & (1 << p))
      memmove(mem + vram_at + p * 0x20000 + (d_ & 0xff) * 4 * 128, mem + vram_at + p * 0x20000 + (s_ & 0xff) * 4 * 128, 4 * 128);
  }
  extra += (unsigned long long)n * RASTER_CYC + 2000;
  ntxras++;
}
/* テキスト VRAM (vt_host_vram の 512KB) への 1 バスサイクルごとのウェイト。XEiJ の X68000 (10MHz) 設定は 2 クロック */
#define TVRAM_WAIT 2
static unsigned long long vwait;
#define VW(a) do { if ((unsigned)((a) - vram_at) < 0x80000u) vwait += TVRAM_WAIT; } while (0)
unsigned int m68k_read_memory_8(unsigned int a) { a &= 0xffffff; VW(a); return mem[a]; }
unsigned int m68k_read_memory_16(unsigned int a) { a &= 0xffffff; VW(a); return (mem[a] << 8) | mem[a + 1]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { a &= 0xffffff; VW(a); mem[a] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { a &= 0xffffff; VW(a); mem[a] = v >> 8; mem[a + 1] = v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); if ((a & 0xffffff) == 0x7fffd0 && v == 1) txras(); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
static void put32(unsigned a, unsigned v) { m68k_write_memory_32(a, v); }
static unsigned get32(unsigned a) { return m68k_read_memory_32(a); }
static long fload(const char *p, unsigned char *dst, long max) { FILE *f = fopen(p, "rb"); long n; if (!f) { perror(p); exit(1); } n = fread(dst, 1, max, f); fclose(f); return n; }
int main(int argc, char **argv)
{
  unsigned cg = 0; char line[256]; FILE *nm; long fsz; unsigned long long cyc = 0, cyc0 = 0;
  fload(argv[1], mem + 0x1000, 0x700000);
  nm = fopen(argv[2], "r");
  while (fgets(line, sizeof line, nm)) { unsigned a; char t, s[200]; if (sscanf(line, "%x %c %199s", &a, &t, s) == 3) { if (!strcmp(s, "vt_host_cgrom")) cg = a; if (!strcmp(s, "vt_host_vram")) vram_at = a; if (!strcmp(s, "vt_host_pal")) pal_at = a; } }
  fclose(nm);
  if (cg) fload(argv[3], mem + cg, 0xc0000);   /* 旧版 (vt_host_cg) は CGROM 無しで走らせる */
  fsz = fload(argv[4], mem + 0x800000, 0x300000);
  put32(0x7ffff0, 0x800000); put32(0x7ffff4, fsz); put32(0x7ffff8, atoi(argv[5])); put32(0x7ffffc, (atoi(argv[6]) << 16) | atoi(argv[7]));
  put32(0, 0xbf0000); put32(4, 0x1000);
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  {
  static unsigned fa[4096]; static char fn[4096][64]; static unsigned long long hits[4096]; int nf = 0, prof = getenv("PROF") != 0;
  if (prof) {   /* 関数表 (nm の T/t) */
    FILE *n2 = fopen(argv[2], "r");
    while (fgets(line, sizeof line, n2) && nf < 4096) { unsigned a; char t, s2[200]; if (sscanf(line, "%x %c %199s", &a, &t, s2) == 3 && (t == 'T' || t == 't')) { fa[nf] = a; strncpy(fn[nf], s2, 63); nf++; } }
    fclose(n2);
    { int i, j; for (i = 0; i < nf; i++) for (j = i + 1; j < nf; j++) if (fa[j] < fa[i]) { unsigned x = fa[i]; char b[64]; fa[i] = fa[j]; fa[j] = x; memcpy(b, fn[i], 64); memcpy(fn[i], fn[j], 64); memcpy(fn[j], b, 64); } }
  }
  for (;;) {
    cyc += m68k_execute(prof ? 200 : 100000);
    if (prof && cyc0) { unsigned pc = m68k_get_reg(NULL, M68K_REG_PC); int i, k = -1; for (i = 0; i < nf && fa[i] <= pc; i++) k = i; if (k >= 0) hits[k]++; }
    if (get32(0x7fffe0) == 1 && !cyc0) { cyc0 = cyc; extra = 0; ntxras = 0; vwait = 0; }
    if (get32(0x7fffe0) == 2) break;
    if (cyc > 100000000000ULL) { fprintf(stderr, "runaway\n"); return 1; }
  }
  if (getenv("DUMP") && *getenv("DUMP")) {   /* vtcat -v と同じ形式で VRAM を書き出す (ホスト版との比較用) */
    FILE *f = fopen(getenv("DUMP"), "wb"); int p;
    for (p = 0; p < 4; p++) fwrite(mem + vram_at + p * 0x20000, 1, 512 * 128, f);
    fwrite(mem + pal_at, 1, 32, f); fclose(f);
  }
  if (prof) { unsigned long long tot = 0; int i; for (i = 0; i < nf; i++) tot += hits[i];
    for (;;) { int b = -1; for (i = 0; i < nf; i++) if (hits[i] && (b < 0 || hits[i] > hits[b])) b = i; if (b < 0 || hits[b] * 100 < tot) break; printf("  %5.1f%%  %s\n", 100.0 * hits[b] / tot, fn[b]); hits[b] = 0; } }
  }
  {
    double s = (double)(cyc - cyc0 + extra + vwait) / 10e6;
    printf("%6ld bytes %10llu cyc (+txras %ld: %llu, +vram wait %llu)  %7.3f s  %6.0f B/s\n", fsz, cyc - cyc0, ntxras, extra, vwait, s, fsz / s);
  }
  return 0;
}
