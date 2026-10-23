/*
 * remote.c -- picosim の B: (リモートドライブ)。Z の scsiremote.sys の要求を x68kserremote の remoteserv.c
 *             (フォークの x68kserremote/service、ビルド時にそこからコンパイルする。ファイル操作は POSIX の fileop.h) に渡す
 *
 * ファームでは vd_command が知らない命令を remote_serv に回す (virtual_disk.c)。ここでも同じ。-R DIR で B: (と続く
 * ドライブ) の中身にする Mac のディレクトリを決める。-vv で要求ごとに記録する (OPEN の名前、READ の fcb・位置・長さ・
 * 返した長さ)。_EXEC の読み込みを追うための記録
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <config.h>
#include <x68kremote.h>
#include "picosim.h"

int remote_serv(uint8_t *wbuf, uint8_t *rbuf);
const char *rootpath[8];
int remote_units;
static uint32_t st_req;

void DPRINTF(int level, char *fmt, ...)
{
  va_list ap;
  if (picosim_verbose < level + 2)
    return;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void remote_add(const char *dir)
{
  if (remote_units < 8)
    rootpath[remote_units++] = dir;
}

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

/* 要求 1 個 (vdbuf の中身)。戻り値は応答の長さ (-1 = 知らない命令) */
int remote_request(uint8_t *c, uint8_t *r)
{
  /* 上位 3 ビットはユニット。ただし remotedrv.c の send_read / send_write は 0x4c / 0x4d をそのまま入れる
     (ユニット 2 に見えるが、remoteserv.c は fcb で探すので困らない) */
  int op = (c[0] & 0x1f) | 0x40, unit = c[0] >> 5;
  int n = remote_serv(c, r);
  st_req++;
  if (picosim_verbose >= 2) {
    static const char *names[] = { "INIT", "CHDIR", "MKDIR", "RMDIR", "RENAME", "DELETE", "CHMOD", "FILES", "NFILES",
                                   "CREATE", "OPEN", "CLOSE", "READ", "WRITE", "?4e", "FILEDATE", "DSKFRE" };
    const char *nm = op - 0x40 < (int)(sizeof names / sizeof names[0]) ? names[op - 0x40] : "?";
    if (op == 0x4c) {
      struct cmd_read *q = (struct cmd_read *)c;
      struct res_read *a = (struct res_read *)r;
      picosim_log("B: READ   fcb=%08x pos=%u len=%u -> %d (reply %d bytes)\n", be32(q->fcb), be32(q->pos),
                  be16(q->len), (int16_t)be16(a->len), n);
    } else if (op == 0x4d) {
      struct cmd_write *q = (struct cmd_write *)c;
      picosim_log("B: WRITE  fcb=%08x pos=%u len=%u -> reply %d bytes\n", be32(q->fcb), be32(q->pos),
                  be16(q->len), n);
    } else if (op == 0x4a || op == 0x49) {
      struct cmd_open *q = (struct cmd_open *)c;
      struct res_open *a = (struct res_open *)r;
      picosim_log("B%d: %-6s fcb=%08x mode=%d %.8s.%.3s -> %d size=%u\n", unit, nm, be32(q->fcb), q->mode,
                  q->path.name1, q->path.ext, a->res, be32(a->size));
    } else if (op == 0x4b)
      picosim_log("B%d: CLOSE  fcb=%08x\n", unit, be32(((struct cmd_close *)c)->fcb));
    else
      picosim_log("B%d: %-6s (op 0x%02x) -> reply %d bytes\n", unit, nm, op, n);
  }
  return n;
}
