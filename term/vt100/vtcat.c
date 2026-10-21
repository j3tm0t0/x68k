/*
 * vtcat -- VT100 バイト列を記録したファイルを読み、エミュレータに食わせて
 *          結果の論理画面を UTF-8 で書き出すテストプログラム。
 *   X68000 では vtcat.x、Mac では -DVT_HOSTTEST で vthost としてビルドする。
 *
 * usage: vtcat [-c CHUNK] [-s COLSxROWS] [-a ATTRFILE] INFILE OUTFILE
 *   -c CHUNK   vt_write に渡す 1 回のバイト数 (既定 4096)。分割境界の頑健性試験に使う
 *   -s CxR     画面サイズ (既定 96x32)
 *   -a FILE    属性 (fg,bg,flags) も FILE に書く
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"

void vt_dump_text(FILE *f, int cols, int rows);
void vt_dump_attr(FILE *f, int cols, int rows);

int main(int argc, char **argv)
{
  int chunk = 4096, cols = 0, rows = 0, i, off, waitkey = 0;
  const char *attrfile = 0, *infile = 0, *outfile = 0;
  long fsz;
  unsigned char *buf;
  FILE *fp;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &cols, &rows); }
    else if (!strcmp(argv[i], "-a") && i + 1 < argc) attrfile = argv[++i];
    else if (!strcmp(argv[i], "-w")) waitkey = 1;   /* 描画後、キー入力まで画面を保持 (実機の目視・VRAM ダンプ用) */
    else if (!infile) infile = argv[i];
    else outfile = argv[i];
  }
  if (!infile || !outfile) { fprintf(stderr, "usage: vtcat [-c N] [-s CxR] [-a F] IN OUT\n"); return 1; }
  if (chunk < 1) chunk = 1;

  fp = fopen(infile, "rb");
  if (!fp) { fprintf(stderr, "cannot open %s\n", infile); return 1; }
  fseek(fp, 0, SEEK_END); fsz = ftell(fp); fseek(fp, 0, SEEK_SET);
  if (fsz < 0) fsz = 0;
  buf = (unsigned char *)malloc(fsz > 0 ? fsz : 1);
  if (!buf) { fprintf(stderr, "oom\n"); fclose(fp); return 1; }
  fsz = (long)fread(buf, 1, fsz, fp);
  fclose(fp);

  if (cols <= 0 || rows <= 0) vt_default_size(&cols, &rows);
  vt_init(cols, rows);
  for (off = 0; off < fsz; off += chunk) {
    int n = (off + chunk <= fsz) ? chunk : (int)(fsz - off);
    vt_write(buf + off, n);
  }
  vt_get_size(&cols, &rows);
  if (waitkey) {
    unsigned char kb[8];
    while (vt_key(kb, sizeof kb) == 0) { volatile long d; for (d = 0; d < 20000; d++) ; }
  }
  vt_fini();   /* 論理バッファは残るので fini 後に書き出す */

  fp = fopen(outfile, "wb");
  if (!fp) { fprintf(stderr, "cannot write %s\n", outfile); free(buf); return 1; }
  vt_dump_text(fp, cols, rows);
  fclose(fp);
  if (attrfile) {
    fp = fopen(attrfile, "wb");
    if (fp) { vt_dump_attr(fp, cols, rows); fclose(fp); }
  }
  free(buf);
  return 0;
}
