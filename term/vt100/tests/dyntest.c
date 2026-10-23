/*
 * dyntest.c -- zos.x のペインの中の term/vt100 (ssh.x) が、ペインの枠の変化についていくかのホスト試験 (-DVT_VCONS)。
 *   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o dyntest tests/dyntest.c vt100.c && ./dyntest [-g CGROM] FIXTURE
 * 1. 枠 48,1 48x31 で始めて 1/3 を流す
 * 2. 枠が隠れる (_B_CONSOL の d1 = -1、zos.x の裏のタブ): 次の 1/3 を流しても VRAM が変わらない
 * 3. 枠が 0,1 47x31 で見える (割り直し): 残りを流す。VRAM = 論理画面 (vt_host_verify)、枠の外は 0、
 *    大きさは 47x31 (vt_get_size)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"
extern unsigned char vt_host_vram[4 * 0x20000];
extern unsigned char vt_host_cgrom[0xC0000];
int vt_host_verify(void);
void vt_host_pane(int x, int y, int w, int h);

static int outside_zero(int x, int y, int w, int h)
{
  int p, l, c;
  for (p = 0; p < 4; p++)
    for (l = 0; l < 512; l++)
      for (c = 0; c < 128; c++) {
        int row = l / 16;
        if (row >= y && row < y + h && c >= x && c < x + w) continue;
        if (vt_host_vram[p * 0x20000 + l * 128 + c]) return 0;
      }
  return 1;
}
int main(int argc, char **argv)
{
  FILE *f;
  unsigned char *d;
  long n, a, b;
  int i = 1, fails = 0, cw, ch;
  static unsigned char save[4 * 0x20000];
  if (argc > 2 && !strcmp(argv[1], "-g")) {
    FILE *g = fopen(argv[2], "rb");
    if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) return 2;
    fclose(g); i = 3;
  }
  if (i >= argc || !(f = fopen(argv[i], "rb"))) { fprintf(stderr, "usage: dyntest [-g CGROM] FIXTURE\n"); return 2; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) return 2; fclose(f);
  a = n / 3; b = 2 * n / 3;
  vt_host_pane(48, 1, 48, 31);
  vt_init(0, 0);
  vt_write(d, (int)a);
  if (!vt_host_verify()) { printf("FAIL verify 1\n"); fails++; }
  if (!outside_zero(48, 1, 48, 31)) { printf("FAIL 枠の外に描いた (1)\n"); fails++; }
  memcpy(save, vt_host_vram, sizeof save);
  vt_host_pane(-2, 0, 48, 31);   /* 隠れる */
  vt_write(d + a, (int)(b - a));
  if (memcmp(save, vt_host_vram, sizeof save)) { printf("FAIL 隠れている間に VRAM を描いた\n"); fails++; }
  memset(vt_host_vram, 0, sizeof save);   /* zos.x がタブを描き直した */
  vt_host_pane(0, 1, 47, 31);    /* 別の枠で見える */
  vt_write(d + b, (int)(n - b));
  if (!vt_host_verify()) { printf("FAIL verify 3\n"); fails++; }
  if (!outside_zero(0, 1, 47, 31)) { printf("FAIL 枠の外に描いた (3)\n"); fails++; }
  vt_get_size(&cw, &ch);
  if (cw != 47 || ch != 31) { printf("FAIL 大きさ %dx%d\n", cw, ch); fails++; }
  printf(fails ? "dyntest: %d FAIL\n" : "dyntest: ok\n", fails);
  return fails != 0;
}
