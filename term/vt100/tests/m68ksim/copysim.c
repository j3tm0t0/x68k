/* tests/copytest.sh 用: Musashi で素のバイナリを DONE (0x7fffe0) = 2 まで回し、0x700000 からの結果の表を書き出す。
 * usage: copysim BIN NWORDS OUT */
#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"
static unsigned char mem[0x1000000];
unsigned int m68k_read_memory_8(unsigned int a) { return mem[a & 0xffffff]; }
unsigned int m68k_read_memory_16(unsigned int a) { a &= 0xffffff; return (mem[a] << 8) | mem[a + 1]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { mem[a & 0xffffff] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { a &= 0xffffff; mem[a] = v >> 8; mem[a + 1] = v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
int main(int argc, char **argv)
{
  FILE *f = fopen(argv[1], "rb"); long n = atol(argv[2]); unsigned long long cyc = 0;
  if (!f) { perror(argv[1]); return 1; }
  fread(mem + 0x1000, 1, 0x6f0000, f); fclose(f);
  m68k_write_memory_32(0, 0xbf0000); m68k_write_memory_32(4, 0x1000);
  m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
  while (m68k_read_memory_32(0x7fffe0) != 2) { cyc += m68k_execute(100000); if (cyc > 40000000000ULL) { fprintf(stderr, "timeout\n"); return 1; } }
  f = fopen(argv[3], "wb"); fwrite(mem + 0x700000, 4, n, f); fclose(f);
  return 0;
}
