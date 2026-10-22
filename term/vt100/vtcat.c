/*
 * vtcat -- VT100 バイト列を記録したファイルを読み、エミュレータに食わせて
 *          結果の論理画面を UTF-8 で書き出すテストプログラム。
 *   X68000 では vtcat.x、Mac では -DVT_HOSTTEST で vthost としてビルドする。
 *
 * usage: vtcat [-c CHUNK] [-s COLSxROWS] [-a ATTRFILE] INFILE OUTFILE
 *   -c CHUNK   vt_write に渡す 1 回のバイト数 (既定 4096)。分割境界の頑健性試験に使う
 *   -s CxR     画面サイズ (既定 96x32)
 *   -a FILE    属性 (fg,bg,flags) も FILE に書く
 *   -v FILE    描画後のテキスト VRAM (4 プレーン x 512 ライン x 128 バイト) とパレット 16 ワード
 *              (ビッグエンディアン) を FILE に書く。tests/vram2png.py で PNG にする
 *   -r FILE    ホストへの応答 (DA/DSR など、vt_key が吐くもの) を FILE に書く
 *   -g FILE    (vthost のみ) CGROM イメージ (768KB、0xF00000-0xFBFFFF) を読み込んで実フォントで描く
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vt100.h"
#ifndef VT_HOSTTEST
#define __IOCS_INLINE__
#include <x68k/iocs.h>
#endif

void vt_dump_text(FILE *f, int cols, int rows);
void vt_dump_attr(FILE *f, int cols, int rows);

#ifdef VT_HOSTTEST
extern unsigned char vt_host_vram[], vt_host_cgrom[];
extern unsigned short vt_host_pal[];
#define VRAMP ((const unsigned char *)vt_host_vram)
#define PALP  ((const unsigned short *)vt_host_pal)
#else
#define __DOS_INLINE__
#include <x68k/dos.h>
#define VRAMP ((const volatile unsigned char *)0xE00000UL)
#define PALP  ((const volatile unsigned short *)0xE82200UL)
#endif

/* テキスト VRAM 4 プレーンの表示部 (512 ライン) とパレットを書き出す。実機はスーパーバイザで読む。 */
static void dump_vram(const char *name)
{
  FILE *f = fopen(name, "wb");
  static unsigned char line[128];
  int p, y, i, s;
  if (!f) { fprintf(stderr, "cannot write %s\n", name); return; }
  for (p = 0; p < 4; p++)
    for (y = 0; y < 512; y++) {
#ifndef VT_HOSTTEST
      s = _dos_super(0);
#endif
      for (i = 0; i < 128; i++) line[i] = VRAMP[(long)p * 0x20000 + (long)y * 128 + i];
#ifndef VT_HOSTTEST
      if (s >= 0) _dos_super(s);
#endif
      fwrite(line, 1, 128, f);
    }
#ifndef VT_HOSTTEST
  s = _dos_super(0);
#endif
  for (i = 0; i < 16; i++) { line[i * 2] = (unsigned char)(PALP[i] >> 8); line[i * 2 + 1] = (unsigned char)PALP[i]; }
#ifndef VT_HOSTTEST
  if (s >= 0) _dos_super(s);
#endif
  (void)s;
  fwrite(line, 1, 32, f);
  fclose(f);
}

int main(int argc, char **argv)
{
  int chunk = 4096, cols = 0, rows = 0, i, off, waitkey = 0, bench = 0;
  const char *attrfile = 0, *infile = 0, *outfile = 0, *vramfile = 0, *replyfile = 0;
  long fsz;
  unsigned char *buf;
  FILE *fp;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &cols, &rows); }
    else if (!strcmp(argv[i], "-a") && i + 1 < argc) attrfile = argv[++i];
    else if (!strcmp(argv[i], "-v") && i + 1 < argc) vramfile = argv[++i];
    else if (!strcmp(argv[i], "-r") && i + 1 < argc) replyfile = argv[++i];
#ifdef VT_HOSTTEST
    else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
      FILE *g = fopen(argv[++i], "rb");
      if (!g || fread(vt_host_cgrom, 1, 0xC0000, g) != 0xC0000) { fprintf(stderr, "cannot read CGROM %s\n", argv[i]); return 1; }
      fclose(g);
    }
#endif
    else if (!strcmp(argv[i], "-w")) waitkey = 1;   /* 描画後、キー入力まで画面を保持 (実機の目視・VRAM ダンプ用) */
    else if (!strcmp(argv[i], "-b") && i + 1 < argc) bench = atoi(argv[++i]);   /* 計測: 全体を bench 回流す */
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
  if (bench > 0) {
#ifndef VT_HOSTTEST
    struct iocs_time t0, t1;
    long total = (long)fsz * bench, cs;
    int rep;
    t0 = _iocs_ontime();
    for (rep = 0; rep < bench; rep++)
      for (off = 0; off < fsz; off += chunk) {
        int n = (off + chunk <= fsz) ? chunk : (int)(fsz - off);
        vt_write(buf + off, n);
      }
    t1 = _iocs_ontime();
    cs = (long)t1.sec - (long)t0.sec;           /* 1/100 秒単位 */
    if (cs < 0) cs += 24L * 60 * 60 * 100;
    vt_fini();
    {
      FILE *bf = fopen(outfile, "wb");
      long bps = cs > 0 ? total * 100 / cs : 0;
      if (bf) { fprintf(bf, "bench: %ld bytes in %ld.%02ld s = %ld bytes/s\n", total, cs / 100, cs % 100, bps); fclose(bf); }
      printf("bench: %ld bytes in %ld.%02ld s = %ld bytes/s\n", total, cs / 100, cs % 100, bps);
    }
    free(buf);
    return 0;
#endif
  }
  for (off = 0; off < fsz; off += chunk) {
    int n = (off + chunk <= fsz) ? chunk : (int)(fsz - off);
    vt_write(buf + off, n);
  }
  vt_get_size(&cols, &rows);
  if (vramfile) dump_vram(vramfile);   /* vt_fini はパレットを戻すので、その前に (カーソルも写る) */
  if (replyfile) {   /* 応答キューを吐かせる (vthost のキーボードは空なので応答だけが出る) */
    unsigned char rb[256];
    int k = vt_key(rb, sizeof rb);
    FILE *rf = fopen(replyfile, "wb");
    if (rf) { fwrite(rb, 1, k, rf); fclose(rf); }
  }
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
