/*
 * bsrmt_host_test.c -- bsrmt.sys のキャッシュと応答の確かめを Mac / Linux で動かす (make -C net/bsrmt hosttest)
 *
 * bsrmt.c をそのまま取り込み、IOCS の SCSI 呼び出し (_S_WRITEEXT / _S_READEXT) を、x68rd_proto.h の形で答える
 * 「にせの BlueSCSI」(ファイル 1 つ) に替える。わざと壊れた応答を返す切り替えもある。
 *
 * 確かめること:
 *   1. READ でキャッシュに入った範囲を大きな WRITE (直接送る) で書き換えた後や、切り詰めた後の READ が新しい中身を返す
 *   2. 頼んだより長い READ の応答はバッファの外に写さず、-15 で失敗する
 *   3. FILES の件数が頼んだ数・X68RD_NFILES・応答の長さを超えても、その数までしか受け取らない。0 件の成功は -18
 *   4. 小さな WRITE の後の CLOSE で中身が届く
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* FILBUF のアドレスは 32 ビットで運ぶので、64 ビットのホストでは番号から本物のバッファを引く */
static unsigned char filbuf_mem[4][64];
#define FILBUF(filep) (filbuf_mem[(filep) & 3])

#include "../bsrmt.c"

char _end, __bss_start;
void dos_print(const char *s) { (void)s; }
void dos_putchar(int c) { (void)c; }

/* ---- にせの BlueSCSI */
static uint8_t disk[65536];
static long disk_len;
static uint8_t answer[X68RD_BYTES];
static long read_extra;                 /* READ の応答の長さに足す */
static int files_num = -1;              /* >= 0: FILES / NFILES の応答の件数をこれにする */
static int files_short;                 /* 1: 応答の長さ (見出しの len) を 1 件分にする */
static int nfiles_left;                 /* ディレクトリの残りの件数 */

static uint16_t g16_(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t g32_(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int files_answer(uint8_t *r, int asked)
{
  int n = asked < nfiles_left ? asked : nfiles_left, i, len;
  nfiles_left -= n;
  r[0] = n ? 0 : (uint8_t)-18;
  for (i = 0; 2 + (i + 1) * X68RD_FILESINFO <= X68RD_DATA_OFF - X68RD_HDR; i++) {
    memset(r + 2 + i * X68RD_FILESINFO, 0, X68RD_FILESINFO);
    r[2 + i * X68RD_FILESINFO] = 0x20;
    snprintf((char *)r + 2 + i * X68RD_FILESINFO + 9, 23, "F%02d.TXT", i);
  }
  if (files_num >= 0) { n = files_num; r[0] = 0; }
  r[1] = n;
  len = 2 + n * X68RD_FILESINFO;
  if (files_short) len = 2 + X68RD_FILESINFO;
  return len;
}

int _iocs_s_writeext(int lba, int cnt, int id, int sz, const void *data)
{
  const uint8_t *q = data, *c = q + X68RD_HDR;
  uint8_t *r = answer + X68RD_HDR;
  int op = (c[0] & 0x1f) | 0x40, len = 1;
  (void)id; (void)sz; (void)cnt;
  if (lba != X68RD_WLBA) return 0;
  memset(answer, 0, sizeof answer);
  switch (op) {
  case X68RD_CREATE: disk_len = 0; r[0] = 0; break;
  case X68RD_OPEN: r[0] = 0; r[1] = disk_len >> 24; r[2] = disk_len >> 16; r[3] = disk_len >> 8; r[4] = disk_len; len = 5; break;
  case X68RD_READ: {
    long pos = g32_(c + 5), n = g16_(c + 9);
    if (pos > disk_len) pos = disk_len;
    if (n > disk_len - pos) n = disk_len - pos;
    memcpy(answer + X68RD_DATA_OFF, disk + pos, n);
    n += read_extra;
    r[0] = n >> 8; r[1] = n;
    len = 2;
    break;
  }
  case X68RD_WRITE: {
    long pos = g32_(c + 5), n = g16_(c + 9);
    if (n == 0) disk_len = pos;
    memcpy(disk + pos, q + X68RD_DATA_OFF, n);
    if (pos + n > disk_len) disk_len = pos + n;
    r[0] = n >> 8; r[1] = n;
    len = 2;
    break;
  }
  case X68RD_FILES: len = files_answer(r, c[2]); break;
  case X68RD_NFILES_CMD: len = files_answer(r, c[1]); break;
  default: r[0] = 0; break;
  }
  memcpy(answer, "X6RA", 4);
  memcpy(answer + 4, q + 4, 4);
  answer[8] = len >> 8; answer[9] = len;
  return 0;
}

int _iocs_s_readext(int lba, int cnt, int id, int sz, void *buf)
{
  (void)id; (void)sz;
  if (lba == X68RD_RLBA) memcpy(buf, answer, cnt * 512);
  return 0;
}
int _iocs_s_request(int n, int id, void *buf) { (void)n; (void)id; (void)buf; return 0; }
int _iocs_s_inquiry(int n, int id, struct iocs_inquiry *b) { (void)n; (void)id; (void)b; return 0; }
int _iocs_timeget(void) { return 0x123456; }
int _iocs_bindateget(void) { return 0x07ea0b0e; }
struct iocs_time _iocs_ontime(void) { struct iocs_time t = { 0, 0 }; return t; }

/* ---- Human68k の真似 */
static uint8_t fcb[96];
static uint8_t nb[X68RD_NAMEBUF];
static struct dcache test_dcache[CONFIG_NDCACHE];
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

static void reset(void)
{
  dcache = test_dcache;
  ndcache = CONFIG_NDCACHE;
  datasize = CONFIG_DATASIZE;
  nfiles = X68RD_NFILES;
  memset(test_dcache, 0, sizeof test_dcache);
  memset(fcache, 0, sizeof fcache);
  memset(fcb, 0, sizeof fcb);
  read_extra = 0; files_num = -1; files_short = 0; nfiles_left = 0;
}

int main(void)
{
  uint8_t rbuf[16];
  static uint8_t big[CONFIG_DATASIZE + 16];
  int n;

  /* 1. 読み込みのキャッシュと直接の WRITE */
  reset();
  memset(disk, 'o', 30000);
  disk_len = 30000;
  check("open", call(0x0a, nb, 0, 0), 0);
  check("small read fills the cache", call(0x0c, rbuf, sizeof rbuf, 0), sizeof rbuf);
  fcb_fpos(fcb) = 0;
  memset(big, 'n', CONFIG_DATASIZE);
  check("large write goes straight to BlueSCSI", call(0x0d, big, CONFIG_DATASIZE, 0), CONFIG_DATASIZE);
  fcb_fpos(fcb) = 0;
  memset(rbuf, 0, sizeof rbuf);
  check("read after it", call(0x0c, rbuf, sizeof rbuf, 0), sizeof rbuf);
  check("gets the new bytes, not the old cache", rbuf[0] == 'n' && rbuf[15] == 'n', 1);
  /* 0 バイトの WRITE (切り詰め) の後も古いキャッシュを返さない */
  fcb_fpos(fcb) = 0;
  call(0x0c, rbuf, sizeof rbuf, 0);
  fcb_fpos(fcb) = 4;
  check("truncate to 4", call(0x0d, big, 0, 0), 0);
  fcb_fpos(fcb) = 0;
  check("read after truncating", call(0x0c, rbuf, sizeof rbuf, 0), 4);

  /* 2. 長すぎる READ の応答 */
  reset();
  memset(disk, 'o', 100);
  disk_len = 100;
  read_extra = 20000;
  big[CONFIG_DATASIZE] = 0x5a;
  check("read with a too long answer", call(0x0c, big, CONFIG_DATASIZE, 0), -15);
  check("nothing written past the buffer", big[CONFIG_DATASIZE], 0x5a);
  read_extra = 3;                       /* 小さな READ (キャッシュに読む) でも */
  check("small read with a too long answer", call(0x0c, rbuf, sizeof rbuf, 0), -15);

  /* 3. FILES / NFILES の件数 */
  reset();
  nfiles_left = 40;
  check("files", call(0x07, nb, 1, 0x20), 0);
  for (n = 1; n < 100 && call(0x08, nb, 1, 0) == 0; n++)
    ;
  check("files + nfiles see all 40", n, 40);
  reset();
  files_num = 200;                      /* 頼んだ 32 件より多いと答える */
  check("files that claims 200", call(0x07, nb, 1, 0x20), 0);
  check("cached no more than 32", fcache[0].num, X68RD_NFILES);
  reset();
  files_short = 1;                      /* 32 件と答えるが、応答の長さは 1 件分 */
  nfiles_left = 40;
  check("files whose answer is short", call(0x07, nb, 1, 0x20), 0);
  check("nothing cached beyond the answer", fcache[0].filep, 0);
  reset();
  files_num = 0;                        /* 成功で 0 件 */
  check("files with 0 entries", call(0x07, nb, 1, 0x20), _DOSE_NOMORE);

  /* 4. 小さな WRITE は CLOSE で届く */
  reset();
  check("create", call(0x09, nb, 1, 0x20), 0);
  memset(big, 'w', 100);
  check("small write (cached)", call(0x0d, big, 100, 0), 100);
  check("close", call(0x0b, 0, 0, 0), 0);
  check("bytes on BlueSCSI", disk_len, 100);

  printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
  return fails != 0;
}
