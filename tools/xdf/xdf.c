/*
 * xdfwrite.x / xdfread.x -- 2HD フロッピーと XDF イメージ (1,261,568 バイト) の間を IOCS で直接読み書きする。
 *
 *   xdfwrite [-v] [-f] [-c CYL] [--force] IMAGE.XDF B:
 *                                                イメージを B: のフロッピーに書き、全トラックを読み返して比べる
 *                                                (比べるのは書いたときに取ったセクタごとの CRC32。イメージは 1 度だけ読む)
 *                                                -f は先に _B_FORMAT で 2HD (1024 バイト x 8 セクタ) に初期化する。
 *                                                -f が無くても、最初と最後のトラックが 2HD として読めなければ初期化する
 *                                                (DOS/V の 1.44MB や未初期化のディスク)
 *                                                -c CYL は CYL 番のシリンダから書く (途中で止まったときの続き。初期化しない)
 *                                                A: (起動中のディスク) は --force が無いと断る
 *   xdfread  [-v] B: OUT.XDF                     B: のフロッピーを丸ごと読んでファイルにする (A: も読める)。
 *                                                OUT.XDF が既にあれば断る
 *   xdfwrite -s B: / xdfread -s B:               ドライブの状態 (_B_DRVCHK と _B_DRVSNS) を出すだけ
 *
 * 2HD は 77 シリンダ x 2 ヘッド x 8 セクタ x 1024 バイト。XDF はこの順 (シリンダ、ヘッド、セクタ) に並べたもの。
 * IOCS の _B_READ / _B_WRITE / _B_FORMAT を 1 トラック (8KB) ずつ呼ぶ。失敗したら 1 秒待って 5 回までやり直す。
 * イメージはメモリに入るだけ (最大 40 シリンダ) まとめて読んでから続けて書く。リモートドライブからの読み込みが遅く、
 * 1 シリンダずつ読むとその間にドライブのモーターが止まり、次の書き込みで not ready になることがあったため
 * (実機の XVI で C26 の書き込みが 691a001a で失敗した)。DOS を通さないので、
 * ディスクが無いときや書き込み禁止のときに Human68k のエラーの窓 (中止/再実行/無視) は出ない。
 * 先に _B_DRVCHK でディスクの有無と書き込み禁止を確かめる。
 *
 * ドライブ名はフロッピーの装置番号に直に対応させる (A: = 0、B: = 1)。フロッピーから起動した機械の並びで、
 * ハードディスクから起動した機械では違うことがある。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#define __IOCS_INLINE__
#include <x68k/iocs.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define CYLS    77
#define HEADS   2
#define SECS    8
#define SECSIZE 1024
#define TRACK   (SECS * SECSIZE)
#define CYLSIZE (HEADS * TRACK)
#define XDFSIZE ((long)CYLS * CYLSIZE)
#define TRIES   6

/* _B_DRVCHK (d2 = 0、状態を見るだけ) の戻り値。XEiJ で確かめた: 入っている 02、書き込み禁止 0a、無い 0c */
#define CHK_INSERTED  0x02   /* ディスクが入っている */
#define CHK_PROTECTED 0x08   /* 書き込み禁止 (ディスクが無いときも 1) */

static int verbose;
static unsigned char dbuf[TRACK];
#ifndef XDFREAD
static unsigned long crcs[CYLS * HEADS * SECS];   /* 書いたセクタの CRC32。読み返しはこれと比べる (C: を 2 度読まない) */
static unsigned long crctab[256];

static unsigned long crc32(const unsigned char *p, int n)
{
  unsigned long c = 0xffffffff;
  while (n--) c = crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffff;
}
#endif

static int pda(int unit) { return 0x9070 | (unit << 8); }   /* PDA 9x (2HD) + MFM、再試行あり */
static int pos(int c, int h, int r) { return 0x03000000 | (c << 16) | (h << 8) | r; }   /* N=3 (1024 バイト) */

/* 戻り値は FDC の ST0、ST1、ST2、シリンダ番号の 4 バイト。ST0 の bit7-6 が 00 なら正常に終わった。
 * XEiJ で確かめた値: 正常 0100001a (装置 1、C26)、書き込み禁止 4102001a、ディスク無し 69000000、セクタが無い ffffffff。
 * ST0 の bit5 (SE、シーク終わり) が立つのはシークの段階で失敗したときで、2 バイト目は ST1 ではない
 * (実機の 691a001a は「C26 へのシークでドライブが not ready」。2 バイト目もシリンダ番号) */
static int failed(int st) { return (st & 0xc0000000) != 0; }

static void explain(int st)
{
  fflush(stdout);
  int st0 = (st >> 24) & 0xff, st1 = (st >> 16) & 0xff, st2 = (st >> 8) & 0xff;
  fprintf(stderr, " (status %08x:", st);
  if (st == -1) { fprintf(stderr, " sector not found)\n"); return; }
  if (st0 & 0x08) fprintf(stderr, " drive not ready");
  if (st0 & 0x10) fprintf(stderr, " equipment check");
  if (st0 & 0x20) { fprintf(stderr, " at seek)\n"); return; }
  if (st1 & 0x02) fprintf(stderr, " write protected");
  if (st1 & 0x01) fprintf(stderr, " no address mark");
  if (st1 & 0x04) fprintf(stderr, " no data");
  if (st1 & 0x20) fprintf(stderr, " CRC error");
  if (st1 & 0x10) fprintf(stderr, " overrun");
  if (st1 & 0x80) fprintf(stderr, " end of cylinder");
  if (st2 & 0x40) fprintf(stderr, " deleted data");
  fprintf(stderr, ")\n");
}

static int drive(const char *s)
{
  if (!s[0] || s[1] != ':' || s[2]) return -1;
  int d = (s[0] | 0x20) - 'a';
  return d >= 0 && d < 4 ? d : -1;
}

/* ディスクの有無と書き込み禁止を確かめる。駄目なら理由を出して 0 を返す */
static int check(int unit, int write)
{
  int s = _iocs_b_drvchk(pda(unit) & 0xff00, 0);
  if (verbose) printf("drive %c: _B_DRVCHK = %08x\n", 'A' + unit, s);
  if (!(s & CHK_INSERTED)) {
    fprintf(stderr, "no disk in drive %c:\n", 'A' + unit);
    return 0;
  }
  if (write && (s & CHK_PROTECTED)) {
    fprintf(stderr, "the disk in drive %c: is write protected\n", 'A' + unit);
    return 0;
  }
  return 1;
}

static void wait_cs(int cs)   /* 1/100 秒単位で待つ */
{
  int t0 = _iocs_ontime().sec;
  while ((unsigned)(_iocs_ontime().sec - t0) < (unsigned)cs)
    ;
}

/* 1 トラックの読み・書き・初期化。IOCS の再試行に加えて、失敗したら知らせて 1 秒待ち、tries 回までやり直す */
enum { RD, WR, FMT };
static int track(int op, int unit, int c, int h, void *buf, int tries)
{
  unsigned char id[SECS * 4];
  int st = 0;
  for (int i = 1; ; i++) {
    switch (op) {
    case RD: st = _iocs_b_read(pda(unit), pos(c, h, 1), TRACK, buf); break;
    case WR: st = _iocs_b_write(pda(unit), pos(c, h, 1), TRACK, buf); break;
    default:
      for (int r = 0; r < SECS; r++) {
        id[r*4] = c; id[r*4+1] = h; id[r*4+2] = r + 1; id[r*4+3] = 3;
      }
      st = _iocs_b_format(pda(unit), pos(c, h, 1), sizeof id, id);
    }
    if (!failed(st)) return 0;
    if (i >= tries) return st;
    printf("\n%s C%d H%d failed, retrying (%d/%d)", op == RD ? "read" : op == WR ? "write" : "format", c, h, i, tries - 1);
    explain(st);
    wait_cs(100);
  }
}

static int rdtrack(int unit, int c, int h, void *buf) { return track(RD, unit, c, h, buf, TRIES); }

#ifndef XDFREAD
/* 2HD (1024 バイト x 8 セクタ) に初期化してあるかを、最初と最後のトラックを読んで見る。
 * DOS/V の 1.44MB (512 バイト x 18 セクタ) や未初期化のディスクは読めない */
static int is_2hd(int unit)
{
  int st;
  if ((st = track(RD, unit, 0, 0, dbuf, 1)) || (st = track(RD, unit, CYLS - 1, HEADS - 1, dbuf, 1))) {
    if (verbose) { printf("track check"); explain(st); }
    return 0;
  }
  return 1;
}
#endif

static void progress(const char *what, int c)
{
  printf("\r%s cylinder %2d/%d", what, c + 1, CYLS);
  fflush(stdout);
}

static int status_only(const char *d)
{
  int unit = drive(d);
  if (unit < 0) { fprintf(stderr, "bad drive %s\n", d); return 1; }
  int s = _iocs_b_drvchk(pda(unit) & 0xff00, 0);
  printf("drive %c: _B_DRVCHK = %08x (%s%s), _B_DRVSNS = %08x\n", 'A' + unit, s,
         s & CHK_INSERTED ? "inserted" : "no disk", (s & (CHK_INSERTED | CHK_PROTECTED)) == (CHK_INSERTED | CHK_PROTECTED) ?
         ", write protected" : "", _iocs_b_drvsns(pda(unit) & 0xff00));
  return 0;
}

#ifdef XDFREAD
static int run(int argc, char **argv)
{
  int unit;
  if (argc != 2 || (unit = drive(argv[0])) < 0) {
    fprintf(stderr, "usage: xdfread [-v] B: OUT.XDF   (read a whole 2HD disk into an XDF file)\n"
                    "       xdfread -s B:             (show the drive status)\n");
    return 1;
  }
  if (!check(unit, 0)) return 1;
  int fd = open(argv[1], O_RDONLY | O_BINARY);
  if (fd >= 0) { close(fd); fprintf(stderr, "%s already exists\n", argv[1]); return 1; }
  fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
  if (fd < 0) { fprintf(stderr, "cannot create %s\n", argv[1]); return 1; }
  int bad = 0;
  for (int c = 0; c < CYLS; c++) {
    progress("read", c);
    for (int h = 0; h < HEADS; h++) {
      int st = rdtrack(unit, c, h, dbuf);
      if (st) {   /* 1 セクタずつ読み直し、読めないセクタは 0 で埋める */
        for (int r = 0; r < SECS; r++) {
          unsigned char *p = dbuf + r * SECSIZE;
          int st1 = _iocs_b_read(pda(unit), pos(c, h, r + 1), SECSIZE, p);
          if (failed(st1)) {
            memset(p, 0, SECSIZE);
            printf("\nbad sector C%d H%d R%d", c, h, r + 1);
            explain(st1);
            bad++;
          }
        }
      }
      if (write(fd, dbuf, TRACK) != TRACK) {
        fprintf(stderr, "\nwrite error on %s (disk full?)\n", argv[1]);
        close(fd);
        return 1;
      }
    }
  }
  close(fd);
  if (bad) { printf("\nNG: %d unreadable sectors (written as zeros)\n", bad); return 1; }
  printf("\nOK: %ld bytes\n", XDFSIZE);
  return 0;
}
#else
static int run(int argc, char **argv)
{
  int format = 0, force = 0, start = 0, unit;
  for (; argc && argv[0][0] == '-'; argc--, argv++) {
    if (!strcmp(argv[0], "-f")) format = 1;
    else if (!strcmp(argv[0], "-c") && argc > 1) {
      start = atoi(argv[1]); argc--, argv++;
      if (start < 0 || start >= CYLS) argc = -1;
    }
    else if (!strcmp(argv[0], "-v")) verbose = 1;
    else if (!strcmp(argv[0], "--force")) force = 1;
    else argc = -1;
  }
  if (argc != 2 || (unit = drive(argv[1])) < 0) {
    fprintf(stderr, "usage: xdfwrite [-v] [-f] [-c CYL] [--force] IMAGE.XDF B:\n"
                    "  writes a 2HD XDF image to the disk and reads it back to compare\n"
                    "  -f       format the disk first (2HD, 1024 bytes x 8 sectors)\n"
                    "  -c CYL   start writing at cylinder CYL (0-76; resume after a failure, no format)\n"
                    "  --force  allow drive A: (the boot disk)\n"
                    "       xdfwrite -s B:   (show the drive status)\n");
    return 1;
  }
  if (unit == 0 && !force) {
    fprintf(stderr, "drive A: is the boot disk; use --force to overwrite it\n");
    return 1;
  }
  int fd = open(argv[0], O_RDONLY | O_BINARY);
  if (fd < 0) { fprintf(stderr, "cannot open %s\n", argv[0]); return 1; }
  long size = lseek(fd, 0, SEEK_END);
  if (size != XDFSIZE) {
    fprintf(stderr, "%s is %ld bytes; a 2HD XDF is %ld bytes\n", argv[0], size, XDFSIZE);
    return 1;
  }
  if (!check(unit, 1)) return 1;
  for (int i = 0; i < 256; i++) {
    unsigned long c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
    crctab[i] = c;
  }

  if (start) format = 0;
  else if (!format && !is_2hd(unit)) {
    printf("drive %c: is not X68000 2HD (1024 bytes x 8 sectors); formatting it first\n", 'A' + unit);
    format = 1;
  }
  if (format) {
    for (int c = 0; c < CYLS; c++) {
      progress("format", c);
      for (int h = 0; h < HEADS; h++) {
        int st = track(FMT, unit, c, h, 0, TRIES);
        if (st) { printf("\nNG: format failed at C%d H%d", c, h); explain(st); return 1; }
      }
    }
    printf("\n");
  }

  /* イメージを入るだけ (最大 40 シリンダ) メモリに読み、まとめて書く */
  int n = 40;
  unsigned char *buf;
  while (!(buf = malloc((long)n * CYLSIZE)))
    if ((n /= 2) == 0) { fprintf(stderr, "out of memory\n"); return 1; }
  if (verbose) printf("buffer: %d cylinders\n", n);
  lseek(fd, 0, SEEK_SET);
  for (int c0 = 0; c0 < CYLS; c0 += n) {
    int k = c0 + n < CYLS ? n : CYLS - c0;
    for (int i = 0; i < k; i++) {
      unsigned char *p = buf + (long)i * CYLSIZE;
      progress("load ", c0 + i);
      if (read(fd, p, CYLSIZE) != CYLSIZE) { fprintf(stderr, "\nread error on %s\n", argv[0]); return 1; }
      for (int s = 0; s < HEADS * SECS; s++)
        crcs[(c0 + i) * HEADS * SECS + s] = crc32(p + s * SECSIZE, SECSIZE);
    }
    for (int i = 0; i < k; i++) {
      int c = c0 + i;
      if (c < start) continue;
      progress("write", c);
      for (int h = 0; h < HEADS; h++) {
        int st = track(WR, unit, c, h, buf + (long)i * CYLSIZE + h * TRACK, TRIES);
        if (st) {
          printf("\nNG: write failed at C%d H%d", c, h);
          explain(st);
          if (!format && !(st & 0x08000000)) printf("the disk may not be formatted as X68000 2HD; try again with -f\n");
          printf("to resume: xdfwrite -c %d %s %s\n", c, argv[0], argv[1]);
          return 1;
        }
      }
    }
  }
  close(fd);
  free(buf);
  printf("\n");

  for (int c = 0; c < CYLS; c++) {
    progress("verify", c);
    for (int h = 0; h < HEADS; h++) {
      int st = rdtrack(unit, c, h, dbuf);
      if (st) { printf("\nNG: read back failed at C%d H%d", c, h); explain(st); return 1; }
      for (int r = 0; r < SECS; r++)
        if (crc32(dbuf + r * SECSIZE, SECSIZE) != crcs[(c * HEADS + h) * SECS + r]) {
          printf("\nNG: sector C%d H%d R%d (sector %d of the image) differs\n",
                 c, h, r + 1, (c * HEADS + h) * SECS + r);
          return 1;
        }
    }
  }
  printf("\nOK: %ld bytes written and verified. eject and reinsert the disk before using it from DOS\n", XDFSIZE);
  return 0;
}
#endif

int main(int argc, char **argv)
{
  argc--, argv++;
  for (; argc && argv[0][0] == '-'; argc--, argv++) {
    if (!strcmp(argv[0], "-v")) verbose = 1;
    else if (!strcmp(argv[0], "-s") && argc == 2) return status_only(argv[1]);
    else break;
  }
  return run(argc, argv);
}
