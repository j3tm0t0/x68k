/*
 * zrmt_host_test.c -- zrmt.sys の書き込みのキャッシュ (dcache) の試験を Mac / Linux で動かす
 *
 *   cc -O1 -Wall -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-function \
 *      -I test/stub -o /tmp/zrmt_host_test test/zrmt_host_test.c && /tmp/zrmt_host_test
 *   (net/zmux で。make -C net/zmux/x68k hosttest でも同じ)
 *
 * zrmt.c をそのまま取り込み、zmux.sys の入口 (call_entry) を、空きの大きさを決められる「にせの Pi」に替える。
 * にせの Pi はホストのバイト順で読み書きする (zrmt.c の構造体をそのまま見るので、68000 のバイト順でなくてよい)。
 *
 * 確かめること: 小さな WRITE は送る前に成功を返すので、後でキャッシュを送ったときの失敗 (ディスクフル) が
 * CLOSE / SEEK / READ / FILEDATE の結果として返ること。FILEDATE の前に書き込みが届くこと。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../x68k/zrmt.c"

char _end, __bss_start;
void dos_print(const char *s) { (void)s; }
void dos_putchar(int c) { (void)c; }

/* ---- にせの Pi: ファイルは 1 つ、空きは cap バイト */
static uint8_t disk[65536];
static long disk_len, cap;
static int log_ops[64], nlog;           /* 届いた命令の順番 */

static void logop(int op) { if (nlog < 64) log_ops[nlog++] = op; }

long call_entry(void *e, long cmd, long *a)
{
  uint8_t *w = (uint8_t *)a[0];
  uint8_t *rb = (uint8_t *)a[2];
  int op = (w[0] & 0x1f) | 0x40;
  (void)e; (void)cmd;
  logop(op);
  switch (op) {
  case 0x49: case 0x4a: case 0x4b:      /* create, open, close */
    if (op == 0x49) disk_len = 0;
    if (op == 0x4a) { struct res_open *ro = (void *)rb; ro->res = 0; ro->size = disk_len; return sizeof *ro; }
    rb[0] = 0;
    return 1;
  case 0x4d: {                          /* write: 空きを超えた分は書けない (remoteserv.c / rdrive.rs と同じくエラーを返す) */
    struct cmd_write *cw = (void *)w;
    struct res_write *rw = (void *)rb;
    if (cw->len == 0) { disk_len = cw->pos; rw->len = 0; return 2; }
    if (cw->pos + cw->len > cap) { rw->len = _DOSE_DISKFULL; return 2; }
    memcpy(disk + cw->pos, cw->data, cw->len);
    if (cw->pos + cw->len > disk_len) disk_len = cw->pos + cw->len;
    rw->len = cw->len;
    return 2;
  }
  case 0x4c: {                          /* read */
    struct cmd_read *cr = (void *)w;
    struct res_read *rr = (void *)rb;
    long n = cr->pos >= disk_len ? 0 : disk_len - cr->pos;
    if (n > cr->len) n = cr->len;
    memcpy(rr->data, disk + cr->pos, n);
    rr->len = n;
    return 2 + n;
  }
  case 0x4f: {                          /* filedate */
    struct res_filedate *rf = (void *)rb;
    rf->time = 0x1234; rf->date = 0x5678;
    return sizeof *rf;
  }
  }
  rb[0] = 0;
  return 1;
}

/* ---- Human68k の真似 */
static uint8_t fcb[96];
static uint8_t namebuf[NAMEBUF];
static int fails;

static long call(int cmd, void *addr, long status, int attr)
{
  struct req_header q;
  memset(&q, 0, sizeof q);
  q.command = cmd;
  q.addr = addr;
  q.status = status;
  q.attr = attr;
  q.fcb = fcb;
  rd_interrupt(&q);
  return (long)(int32_t)q.status;
}

static void check(const char *what, long got, long want)
{
  if (got != want) { printf("FAIL %s: %ld (want %ld)\n", what, got, want); fails++; }
  else printf("ok   %s: %ld\n", what, got);
}

static void reset(long capacity)
{
  memset(dcache, 0, sizeof dcache);
  memset(fcb, 0, sizeof fcb);
  disk_len = 0; cap = capacity; nlog = 0;
  call(0x09, namebuf, 0, 0x20);         /* create */
}

static long write_small(int n)
{
  static uint8_t buf[CONFIG_DATASIZE];
  memset(buf, 'a', n);
  return call(0x0d, buf, n, 0);
}

int main(void)
{
  uint8_t rbuf[16];
  int i;

  entry = (void *)1;

  /* 1. 空きが足りないまま CLOSE: 書き込みは成功に見えるが、CLOSE がディスクフルを返す */
  reset(512);
  check("write 1000 (cached)", write_small(1000), 1000);
  check("close on a full disk", call(0x0b, 0, 0, 0), _DOSE_DISKFULL);

  /* 2. 空きが十分なら CLOSE は 0、中身も届く */
  reset(sizeof disk);
  check("write 1000", write_small(1000), 1000);
  check("write 1000 more", write_small(1000), 1000);
  check("close", call(0x0b, 0, 0, 0), 0);
  check("bytes on the Pi", disk_len, 2000);

  /* 3. SEEK の前の送り出しの失敗 */
  reset(100);
  write_small(500);
  check("seek after a failed flush", call(0x0e, 0, 0, 0), _DOSE_DISKFULL);

  /* 4. READ の前の送り出しの失敗 */
  reset(100);
  write_small(500);
  check("read after a failed flush", call(0x0c, rbuf, sizeof rbuf, 0), _DOSE_DISKFULL);

  /* 5. 続かない位置への WRITE で前のキャッシュを送り出すときの失敗 */
  reset(100);
  write_small(500);
  fcb_fpos(fcb) = 2000;
  check("write elsewhere after a failed flush", write_small(10), _DOSE_DISKFULL);

  /* 6. FILEDATE の前に書き込みが Pi に届く */
  reset(sizeof disk);
  write_small(300);
  call(0x0f, 0, 0x56781234, 0);
  for (i = 0; i < nlog && log_ops[i] != 0x4d; i++)
    ;
  check("write reaches the Pi before filedate", i < nlog && (i + 1 < nlog && log_ops[i + 1] == 0x4f), 1);
  check("close after filedate", call(0x0b, 0, 0, 0), 0);

  printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
  return fails != 0;
}
