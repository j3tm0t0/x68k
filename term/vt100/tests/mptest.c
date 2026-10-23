/*
 * mptest.c -- 表の端末を何枚も並べる (vt_con_rect、Z-OS の zos.x のペイン) のホスト試験。
 *   cc -O2 -DVT_HOSTTEST -DVT_VCONS -I. -o mptest tests/mptest.c vt100.c && ./mptest [-g CGROM] -o OUT.vram spec...
 *   spec = フィクスチャ:桁,行,幅,高さ   (最大 4)
 * 各フィクスチャを自分の枠の端末に割り当て、ばらばらの長さで端末を入れ替えながら流す (全部表)。途中で 1 回、
 * 端末 0 と 1 の枠を入れ替えて (同じ大きさのとき。vt_con_rect + vt_con_visible) 描き直し、最後に元の枠に戻す。最後の VRAM を OUT に書く
 * (tests/mpcmp.py が、各枠の中 = そのフィクスチャだけを全面 (幅x高さ) で流した VRAM、枠の外 = 0 を確かめる)。
 * 毎回: いまの端末の VRAM = 論理画面の描き直し (vt_host_verify)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"

extern unsigned char vt_host_vram[4 * 0x20000];
extern unsigned short vt_host_pal[16];
extern unsigned char vt_host_cgrom[0xC0000];
int vt_host_verify(void);

#define NC 4
static void *st[NC], *scr[NC];
static int loaded, ncon;
static unsigned char *data[NC];
static long len[NC], pos[NC];
static int rx[NC], ry[NC], rw[NC], rh[NC];
static int fails;

static void use(int k)
{
  if (k == loaded) return;
  vt_con_save(st[loaded]);
  vt_con_load(st[k]);
  loaded = k;
}

int main(int argc, char **argv)
{
  const char *out = 0;
  int i, k, step = 0;
  unsigned seed = 12345;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-g") && i + 1 < argc) {
      FILE *g = fopen(argv[++i], "rb");
      if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cgrom\n"); return 2; }
      fclose(g);
    } else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-F")) vt_set_small(1);   /* 小さい字 (6x12)。枠の左端は 4 桁の倍数 */
    else if (ncon < NC) {
      char name[512]; FILE *f;
      if (sscanf(argv[i], "%511[^:]:%d,%d,%d,%d", name, &rx[ncon], &ry[ncon], &rw[ncon], &rh[ncon]) != 5) { fprintf(stderr, "spec %s\n", argv[i]); return 2; }
      f = fopen(name, "rb");
      if (!f) { perror(name); return 2; }
      fseek(f, 0, SEEK_END); len[ncon] = ftell(f); fseek(f, 0, SEEK_SET);
      data[ncon] = malloc(len[ncon] + 1);
      if (fread(data[ncon], 1, len[ncon], f) != (size_t)len[ncon]) return 2;
      fclose(f);
      ncon++;
    }
  }
  if (!ncon || !out) { fprintf(stderr, "usage: mptest [-g CGROM] -o OUT spec...\n"); return 2; }
  vt_init(0, 0);
  for (k = 0; k < ncon; k++) { st[k] = calloc(1, vt_con_size()); scr[k] = calloc(1, vt_con_scrsize()); }
  /* 端末 0 は vt_init のもの (置き場は内蔵)。1.. は vt_con_new。どれも自分の枠に置いて表に */
  vt_con_rect(rx[0], ry[0], rw[0], rh[0]); vt_con_visible(1);
  for (k = 1; k < ncon; k++) {
    vt_con_save(st[loaded]);
    vt_con_new(scr[k]); loaded = k;
    vt_con_rect(rx[k], ry[k], rw[k], rh[k]); vt_con_visible(1);
  }
  for (;;) {
    int left = 0;
    for (k = 0; k < ncon; k++) left |= pos[k] < len[k];
    if (!left) break;
    k = (seed = seed * 1103515245 + 12345) % ncon;
    if (pos[k] >= len[k]) continue;
    {
      long n = 1 + (seed >> 16) % 97;
      if (n > len[k] - pos[k]) n = len[k] - pos[k];
      use(k);
      vt_write(data[k] + pos[k], (int)n);
      pos[k] += n;
      if (!vt_host_verify()) { fprintf(stderr, "FAIL verify (con %d at %ld)\n", k, pos[k]); fails++; }
    }
    if (++step == 40 && ncon >= 2 && rw[0] == rw[1] && rh[0] == rh[1]) {
      /* 途中で枠を入れ替える (0 と 1 を交換して描き直し)。最後に戻す。重なっても後で全部描き直すので構わない */
      int t;
      use(0); vt_con_rect(rx[1], ry[1], rw[1], rh[1]);
      use(1); vt_con_rect(rx[0], ry[0], rw[0], rh[0]);
      for (t = 0; t < 2; t++) { use(t); vt_con_visible(1); }
    }
  }
  /* 元の枠に戻して全部描き直す (枠の外は 0 に) */
  memset(vt_host_vram, 0, sizeof vt_host_vram);
  for (k = 0; k < ncon; k++) { use(k); vt_con_rect(rx[k], ry[k], rw[k], rh[k]); vt_con_visible(1); }
  for (k = 0; k < ncon; k++) { use(k); if (!vt_host_verify()) { fprintf(stderr, "FAIL verify end %d\n", k); fails++; } }
  {
    FILE *f = fopen(out, "wb");
    int p, y;
    unsigned char line[32];
    for (p = 0; p < 4; p++) for (y = 0; y < 512; y++) fwrite(vt_host_vram + p * 0x20000 + y * 128, 1, 128, f);
    for (i = 0; i < 16; i++) { line[i * 2] = vt_host_pal[i] >> 8; line[i * 2 + 1] = vt_host_pal[i]; }
    fwrite(line, 1, 32, f);
    fclose(f);
  }
  printf(fails ? "mptest: %d FAIL\n" : "mptest: ok\n", fails);
  return fails ? 1 : 0;
}
