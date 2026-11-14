/*
 * x68rd.c -- picosim の ID 6 で BlueSCSI のリモートドライブ (x68k-zos #188、net/bsrmt/x68rd_proto.h) のまね
 *
 *   picosim -X DIR [-X DIR2] ...   bsrmt.sys の要求 (LBA 0x200000 に書く) を受け、応答を LBA 0x210000 から読ませる。
 *                                  中身は remote.c と同じ remoteserv.c (x68kserremote) で、-X のディレクトリは -R と
 *                                  同じユニットの並びに入る。-X を付けると ID 6 の 0x200000〜0x21ffff はこちらの物になり、
 *                                  Z の読み込みキャッシュも通さない (BlueSCSI の X68000 にはキャッシュが無い)
 *
 * 形の違い (remoteserv.c の x68kremote.h、picosim では CONFIG_NFILEINFO 10 / CONFIG_DATASIZE 15360) はここで埋める:
 *   INIT は版と大きさを付けて返す、FILES / NFILES は 1 回 10 件まで、READ / WRITE は 15360 バイトずつに分けて渡し、
 *   データは要求・応答の +512 に置く。BlueSCSI のファーム (BlueSCSI-v2 の lib/X68kRemote) は別の実装なので、
 *   ここはドライバ (bsrmt.sys) の試験用。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "picosim.h"
#include "../../net/bsrmt/x68rd_proto.h"

#define RS_DATASIZE 15360               /* remote/config.h の CONFIG_DATASIZE */
#define RS_NFILES   10

int remote_request(uint8_t *c, uint8_t *r);
extern int remote_units;
int x68rd_on;

static uint8_t req[X68RD_BYTES], ans[X68RD_BYTES];
static uint32_t last_seq, st_req, st_repeat;
static int have_req;

static uint16_t g16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t g32_(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* 1 命令。戻り値は RES の長さ (READ は 2、データは ans + 512) */
static int command(const uint8_t *cmd, int cmdlen, uint8_t *res)
{
  static uint8_t c[RS_DATASIZE + 64], r[RS_DATASIZE + 64];
  int op = (cmd[0] & 0x1f) | 0x40, n;
  (void)cmdlen;
  switch (op) {
  case X68RD_INIT:
    c[0] = cmd[0];
    remote_request(c, r);
    res[0] = 0;
    res[1] = X68RD_VERSION;
    res[2] = RS_NFILES;
    res[3] = 0;
    p16(res + 4, X68RD_DATASIZE);
    res[6] = remote_units;
    return 7;
  case X68RD_FILES: case X68RD_NFILES_CMD: {
    int at = op == X68RD_FILES ? 2 : 1;
    int len = op == X68RD_FILES ? 7 + X68RD_NAMEBUF : 6;
    memcpy(c, cmd, len);
    if (c[at] > RS_NFILES) c[at] = RS_NFILES;
    if (c[at] == 0) c[at] = 1;
    memset(r, 0, 2 + RS_NFILES * X68RD_FILESINFO);
    remote_request(c, r);
    memcpy(res, r, 2 + r[1] * X68RD_FILESINFO);
    return 2 + r[1] * X68RD_FILESINFO;
  }
  case X68RD_READ: {
    uint32_t fcb = g32_(cmd + 1), pos = g32_(cmd + 5);
    int len = g16(cmd + 9), total = 0;
    if (len > X68RD_DATASIZE) len = X68RD_DATASIZE;
    while (len > 0) {
      int size = len > RS_DATASIZE ? RS_DATASIZE : len;
      c[0] = cmd[0];
      p32(c + 1, fcb); p32(c + 5, pos); p16(c + 9, size);
      remote_request(c, r);
      n = (int16_t)g16(r);
      if (n < 0) {
        if (total == 0) total = n;
        break;
      }
      memcpy(ans + X68RD_DATA_OFF + total, r + 2, n);
      total += n; pos += n; len -= n;
      if (n < size) break;
    }
    p16(res, total);
    return 2;
  }
  case X68RD_WRITE: {
    uint32_t fcb = g32_(cmd + 1), pos = g32_(cmd + 5);
    int len = g16(cmd + 9), total = 0;
    const uint8_t *data = req + X68RD_DATA_OFF;
    if (len > X68RD_DATASIZE) len = X68RD_DATASIZE;
    do {
      int size = len > RS_DATASIZE ? RS_DATASIZE : len;
      c[0] = cmd[0];
      p32(c + 1, fcb); p32(c + 5, pos); p16(c + 9, size);
      memcpy(c + 11, data + total, size);
      remote_request(c, r);
      n = (int16_t)g16(r);
      if (n < 0) {
        if (total == 0) total = n;
        break;
      }
      total += n; pos += n; len -= n;
      if (n < size) break;
    } while (len > 0);
    p16(res, total);
    return 2;
  }
  default:
    memcpy(c, cmd, cmdlen < (int)sizeof c ? cmdlen : (int)sizeof c);
    n = remote_request(c, r);
    if (n < 0) {
      res[0] = (uint8_t)-1;
      return 1;
    }
    memcpy(res, r, n);
    return n;
  }
}

static void answer(void)
{
  uint32_t seq = g32_(req + 4);
  int len = g16(req + 8), flags = g16(req + 10);
  if (g32_(req) != X68RD_REQ_MAGIC || len > X68RD_DATA_OFF - X68RD_HDR) {
    memset(ans, 0, X68RD_HDR);
    p32(ans, X68RD_ANS_MAGIC);
    p32(ans + 4, seq);
    p16(ans + 10, X68RD_ST_BADHDR);
    return;
  }
  st_req++;
  if (have_req && seq == last_seq && (flags & X68RD_F_RETRY)) {
    st_repeat++;
    picosim_log("x68rd: seq %u again, the last answer\n", seq);
    return;
  }
  memset(ans, 0, X68RD_DATA_OFF);
  int n = command(req + X68RD_HDR, len, ans + X68RD_HDR);
  p32(ans, X68RD_ANS_MAGIC);
  p32(ans + 4, seq);
  p16(ans + 8, n);
  p16(ans + 10, X68RD_ST_OK);
  last_seq = seq;
  have_req = 1;
}

/* ID 6 の 1 セクタ。0 = こちらで扱った、-1 = ほかへ */
int x68rd_write(uint32_t lba, const uint8_t *buf)
{
  if (!x68rd_on || lba < X68RD_WLBA || lba >= X68RD_WLBA + X68RD_SECTORS)
    return -1;
  memcpy(req + (lba - X68RD_WLBA) * 512, buf, 512);
  /* 要求は 1 回の WRITE(10) で来る。最初のセクタの長さから最後のセクタが分かる */
  uint32_t last;
  int len = g16(req + 8);
  if (g32_(req) == X68RD_REQ_MAGIC) {
    int op = (req[X68RD_HDR] & 0x1f) | 0x40;
    int bytes = op == X68RD_WRITE ? X68RD_DATA_OFF + g16(req + X68RD_HDR + 9) : X68RD_HDR + len;
    last = X68RD_WLBA + (bytes + 511) / 512 - 1;
    if (lba == last || lba == X68RD_WLBA + X68RD_SECTORS - 1)
      answer();
  }
  return 0;
}

int x68rd_read(uint32_t lba, uint8_t *buf)
{
  if (!x68rd_on || lba < X68RD_WLBA || lba >= X68RD_RLBA + X68RD_SECTORS)
    return -1;
  if (lba < X68RD_RLBA) {
    memset(buf, 0, 512);
    return 0;
  }
  if (!have_req && lba == X68RD_RLBA) {
    memset(buf, 0, 512);
    p32(buf, X68RD_ANS_MAGIC);
    p16(buf + 10, X68RD_ST_NOREQ);
    return 0;
  }
  memcpy(buf, ans + (lba - X68RD_RLBA) * 512, 512);
  return 0;
}

int x68rd_range(uint32_t lba)
{
  return x68rd_on && lba >= X68RD_WLBA && lba < X68RD_RLBA + X68RD_SECTORS;
}
