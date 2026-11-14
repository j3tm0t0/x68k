/*
 * x68rd.c -- picosim の ID 6 で BlueSCSI のリモートドライブ (x68k-zos #188、net/bsrmt/x68rd_proto.h) のまね
 *
 *   picosim -X DIR [-X DIR2] ...   bsrmt.sys の要求 (LBA 0x200000 に書く) を受け、応答を LBA 0x210000 から読ませる。
 *                                  中身は remote.c と同じ remoteserv.c (x68kserremote) で、-X のディレクトリは -R と
 *                                  同じユニットの並びに入る。-X を付けると ID 6 の 0x200000〜0x21ffff はこちらの物になり、
 *                                  Z の読み込みキャッシュも通さない (BlueSCSI の X68000 にはキャッシュが無い)。
 *                                  remoteserv.c (RS=) が無いビルドには入らない (picosim.c の弱い定義で -X は何もしない)
 *
 * 形の違い (remoteserv.c の x68kremote.h、picosim では CONFIG_ALIGNED / CONFIG_NFILEINFO 10 / CONFIG_DATASIZE 15360) はここで埋める:
 *   INIT は版と大きさを付けて返す、FILES / NFILES は 1 回 10 件まで、READ / WRITE は 15360 バイトずつに分けて渡し、
 *   データは要求・応答の +512 に置く。BlueSCSI のファーム (BlueSCSI-v2 の lib/X68kRemote) は別の実装なので、
 *   ここはドライバ (bsrmt.sys) の試験用。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <config.h>
#include <x68kremote.h>
#include "picosim.h"
#include "../../net/bsrmt/x68rd_proto.h"

#define RS_DATASIZE CONFIG_DATASIZE     /* remote/config.h: 15360 */
#define RS_NFILES   CONFIG_NFILEINFO    /* 10 */

int remote_request(uint8_t *c, uint8_t *r);
extern int remote_units;
extern int x68rd_on;

static uint8_t req[X68RD_BYTES], ans[X68RD_BYTES];
static uint32_t last_seq, st_req, st_repeat;
static int have_req;

static uint16_t g16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t g32_(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* x68rd_proto.h の形 (詰めた並び) と remoteserv.c の形 (CONFIG_ALIGNED: 32 ビットは 4 の倍数の位置、FILES の 1 件は
   34 バイト) を、struct のメンバーで写し替える。値はどちらもビッグエンディアンのまま運ぶ */
#define CP(dst, src) memcpy(&(dst), (src), sizeof(dst))

/* 1 命令。戻り値は RES の長さ (READ は 2、データは ans + 512) */
static int command(const uint8_t *cmd, int cmdlen, uint8_t *res)
{
  static union { uint8_t b[RS_DATASIZE + 64]; struct cmd_dirop dirop; struct cmd_rename ren; struct cmd_chmod chmod;
                 struct cmd_files files; struct cmd_nfiles nfiles; struct cmd_create create; struct cmd_open open;
                 struct cmd_close close; struct cmd_read read; struct cmd_write write; struct cmd_filedate fdate; } c;
  static union { uint8_t b[RS_DATASIZE + 64]; struct res_files files; struct res_open open; struct res_read read;
                 struct res_write write; struct res_filedate fdate; struct res_dskfre dskfre; } r;
  int op = (cmd[0] & 0x1f) | 0x40, n, i;
  (void)cmdlen;
  memset(c.b, 0, 128);
  memset(r.b, 0, sizeof r.b);
  c.b[0] = cmd[0];
  switch (op) {
  case X68RD_INIT:
    remote_request(c.b, r.b);
    res[0] = 0;
    res[1] = X68RD_VERSION;
    res[2] = RS_NFILES;
    res[3] = 0;
    p16(res + 4, X68RD_DATASIZE);
    res[6] = remote_units;
    return 7;
  case X68RD_CHDIR: case X68RD_MKDIR: case X68RD_RMDIR: case X68RD_DELETE:
    CP(c.dirop.path, cmd + 1);
    remote_request(c.b, r.b);
    res[0] = r.b[0];
    return 1;
  case X68RD_RENAME:
    CP(c.ren.path_old, cmd + 1);
    CP(c.ren.path_new, cmd + 1 + X68RD_NAMEBUF);
    remote_request(c.b, r.b);
    res[0] = r.b[0];
    return 1;
  case X68RD_CHMOD:
    c.chmod.attr = cmd[1];
    CP(c.chmod.path, cmd + 2);
    remote_request(c.b, r.b);
    res[0] = r.b[0];
    return 1;
  case X68RD_FILES: case X68RD_NFILES_CMD:
    if (op == X68RD_FILES) {
      c.files.attr = cmd[1];
      c.files.num = cmd[2] > RS_NFILES ? RS_NFILES : cmd[2] ? cmd[2] : 1;
      CP(c.files.filep, cmd + 3);
      CP(c.files.path, cmd + 7);
    } else {
      c.nfiles.num = cmd[1] > RS_NFILES ? RS_NFILES : cmd[1] ? cmd[1] : 1;
      CP(c.nfiles.filep, cmd + 2);
    }
    remote_request(c.b, r.b);
    res[0] = r.files.res;
    res[1] = r.files.res == 0 ? r.files.num : 0;
    for (i = 0; i < res[1]; i++)       /* 34 バイトの 1 件の atr から名前まで = 32 バイト */
      memcpy(res + 2 + i * X68RD_FILESINFO, &r.files.file[i].atr, X68RD_FILESINFO);
    return 2 + res[1] * X68RD_FILESINFO;
  case X68RD_CREATE:
    c.create.attr = cmd[1];
    c.create.mode = cmd[2];
    CP(c.create.fcb, cmd + 3);
    CP(c.create.path, cmd + 7);
    remote_request(c.b, r.b);
    res[0] = r.b[0];
    return 1;
  case X68RD_OPEN:
    c.open.mode = cmd[1];
    CP(c.open.fcb, cmd + 2);
    CP(c.open.path, cmd + 6);
    remote_request(c.b, r.b);
    res[0] = r.open.res;
    memcpy(res + 1, &r.open.size, 4);
    return 5;
  case X68RD_CLOSE:
    CP(c.close.fcb, cmd + 1);
    remote_request(c.b, r.b);
    res[0] = r.b[0];
    return 1;
  case X68RD_READ: {
    uint32_t pos = g32_(cmd + 5);
    int len = g16(cmd + 9), total = 0;
    if (len > X68RD_DATASIZE) len = X68RD_DATASIZE;
    while (len > 0) {
      int size = len > RS_DATASIZE ? RS_DATASIZE : len;
      c.b[0] = cmd[0];
      CP(c.read.fcb, cmd + 1);
      p32((uint8_t *)&c.read.pos, pos);
      p16((uint8_t *)&c.read.len, size);
      remote_request(c.b, r.b);
      n = (int16_t)g16((uint8_t *)&r.read.len);
      if (n < 0) {
        if (total == 0) total = n;
        break;
      }
      memcpy(ans + X68RD_DATA_OFF + total, r.read.data, n);
      total += n; pos += n; len -= n;
      if (n < size) break;
    }
    p16(res, total);
    return 2;
  }
  case X68RD_WRITE: {
    uint32_t pos = g32_(cmd + 5);
    int len = g16(cmd + 9), total = 0;
    const uint8_t *data = req + X68RD_DATA_OFF;
    if (len > X68RD_DATASIZE) len = X68RD_DATASIZE;
    do {
      int size = len > RS_DATASIZE ? RS_DATASIZE : len;
      c.b[0] = cmd[0];
      CP(c.write.fcb, cmd + 1);
      p32((uint8_t *)&c.write.pos, pos);
      p16((uint8_t *)&c.write.len, size);
      memcpy(c.write.data, data + total, size);
      remote_request(c.b, r.b);
      n = (int16_t)g16((uint8_t *)&r.write.len);
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
  case X68RD_FILEDATE:
    CP(c.fdate.fcb, cmd + 1);
    CP(c.fdate.time, cmd + 5);
    CP(c.fdate.date, cmd + 7);
    remote_request(c.b, r.b);
    memcpy(res, &r.fdate.time, 2);
    memcpy(res + 2, &r.fdate.date, 2);
    return 4;
  case X68RD_DSKFRE:
    remote_request(c.b, r.b);
    memcpy(res, &r.dskfre.res, 4);
    memcpy(res + 4, &r.dskfre.freeclu, 2);
    memcpy(res + 6, &r.dskfre.totalclu, 2);
    memcpy(res + 8, &r.dskfre.clusect, 2);
    memcpy(res + 10, &r.dskfre.sectsize, 2);
    return 12;
  default:
    res[0] = (uint8_t)-1;
    return 1;
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
  } else if (lba == X68RD_WLBA) {
    answer();                           /* 見出しが違う: status 2 */
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
