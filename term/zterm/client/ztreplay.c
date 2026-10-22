/*
 * ztreplay.c -- zterm のフレーム列 (zterm --replay ... --out FILE) を ztdraw.c で描き、最終画面を書き出す試験。
 * サーバの差分・スクロール・消去の符号化と Z 側の適用が、サーバの持つ画面と同じ結果になるかを見る。
 *   usage: ztreplay [-V] [-g CGROM] [-v VRAM] FRAMES OUT.txt
 *   -V  フレームごとに VRAM = 論理画面の描き直し か確かめる
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ztclient.h"
#include "vt100.h"

extern unsigned char vt_host_cgrom[];
int vt_host_verify(void);
void vt_dump_text(FILE *f, int cols, int rows);

int main(int argc, char **argv)
{
  int i, verify = 0, bad = 0, frames = 0, c, r;
  const char *in = 0, *out = 0;
  unsigned char *d;
  long n, o = 0;
  FILE *f;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-V")) verify = 1;
    else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
      FILE *g = fopen(argv[++i], "rb");
      if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cannot read CGROM\n"); return 1; }
      fclose(g);
    } else if (!in) in = argv[i];
    else out = argv[i];
  }
  if (!in || !out) { fprintf(stderr, "usage: ztreplay [-V] FRAMES OUT.txt\n"); return 2; }
  f = fopen(in, "rb");
  if (!f) { perror(in); return 1; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  d = malloc(n + 1);
  if (fread(d, 1, n, f) != (size_t)n) return 1;
  fclose(f);
  while (o + 3 <= n) {
    int len = (d[o + 1] << 8) | d[o + 2], rr;
    if (d[o] != 'Z' || o + 3 + len > n) { fprintf(stderr, "bad frame at %ld\n", o); return 1; }
    if (frames == 0 && d[o + 3] != 0x01) { fprintf(stderr, "no hello\n"); return 1; }
    rr = ztd_frame(d + o + 3, len);
    if (rr < 0) { fprintf(stderr, "bad op in frame %d\n", frames); return 1; }
    frames++;
    if (verify && !vt_host_verify()) { bad++; if (bad < 4) fprintf(stderr, "VERIFY FAILED frame %d\n", frames); }
    o += 3 + len;
  }
  vt_get_size(&c, &r);
  f = fopen(out, "w");
  vt_dump_text(f, c, r);
  fclose(f);
  printf("%s: %d frames, verify-fail %d\n", in, frames, bad);
  return bad ? 1 : 0;
}
