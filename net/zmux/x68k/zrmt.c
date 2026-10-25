/*
 * zrmt.sys -- zmux のチャネル 2 で Pi のディレクトリを Human68k のドライブにする (net/zmux/DESIGN.md)
 *
 *   CONFIG.SYS:  DEVICE = \SYS\RSDRV.SYS
 *                DEVICE = \SYS\ZMUX.SYS
 *                DEVICE = \SYS\ZRMT.SYS [/uN]     (ZMUX.SYS の後ろ。/uN でユニット (ドライブ) の数、既定 1)
 *   Pi:          zmux daemon ... --drive DIR [--drive DIR2]
 *
 * Human68k のリモートドライブ (ファイル単位の要求: CHDIR, FILES, OPEN, READ, WRITE ...) を受け、要求 1 つを
 * zmux.sys の入口 (命令 6) でチャネル 2 の 1 ブロックとして Pi に送り、応答 1 つを受け取る。
 * 要求と応答の形は yunkya2 氏の x68kserremote (MIT) の x68kremote.h と同じで、この処理も driver/remotedrv.c を
 * 元にしている (setjmp の代わりに戻り値で失敗を返す。Pi 側は net/zmux/src/rdrive.rs)。
 * READ / WRITE のデータは 1 回 CONFIG_DATASIZE (4084) バイト = フレーム 4 つ分。小さな読み書きは dcache で 1 回にまとめる。
 * 線が UP でないとき (zmuxd が居ない、素の文字のとき) と時間切れ (5 秒) は「ドライブの準備ができていません」(0x1002) で失敗する。
 */
#define __DOS_INLINE__
#define __IOCS_INLINE__
#include <x68k/dos.h>
#include <x68k/iocs.h>
#include <stdint.h>
#include <stddef.h>

#define CONFIG_DATASIZE 4084
#define CONFIG_NDCACHE  2
#define TIMEOUT         500             /* 1/100 秒 */
#define ZMUX_RD_XCHG    6               /* zmux.sys の入口の命令 */

/* ---- Human68k のエラー */
#define _DOSE_CANTSEEK  -25

struct req_header {
  uint8_t magic, unit, command, errl, errh, reserved[8], attr;
  uint8_t *addr;
  uint32_t status;
  void *fcb;
} __attribute__((packed, aligned(2)));

#define fcb_mode(fcb)  (((uint8_t *)(fcb))[14])
#define fcb_fpos(fcb)  (*(uint32_t *)(&((uint8_t *)(fcb))[6]))
#define fcb_size(fcb)  (*(uint32_t *)(&((uint8_t *)(fcb))[64]))

/* ---- x68kremote.h の形 (packed、ネットワークバイトオーダー = 68000 のまま) */
#define FILBUF_ATR 21                   /* dos_filbuf の atr の位置 (searchatr 1, driveno 1, dirsec 4, dirlft 2, dirpos 2, filename 8, ext 3) */
#define NAMEBUF 88                      /* dos_namebuf: flag 1, drive 1, path 65, name1 8, ext 3, name2 10 */
struct filesinfo { uint8_t dummy, atr; uint16_t time, date; uint32_t filelen; char name[23]; } __attribute__((packed));

struct cmd_dirop { uint8_t command; uint8_t path[NAMEBUF]; } __attribute__((packed));
struct cmd_rename { uint8_t command; uint8_t path_old[NAMEBUF], path_new[NAMEBUF]; } __attribute__((packed));
struct cmd_chmod { uint8_t command, attr; uint8_t path[NAMEBUF]; } __attribute__((packed));
struct cmd_files { uint8_t command, attr; uint32_t filep; uint8_t path[NAMEBUF]; } __attribute__((packed));
struct res_files { int8_t res; struct filesinfo file; } __attribute__((packed));
struct cmd_nfiles { uint8_t command; uint32_t filep; } __attribute__((packed));
struct cmd_create { uint8_t command, attr, mode; uint32_t fcb; uint8_t path[NAMEBUF]; } __attribute__((packed));
struct cmd_open { uint8_t command, mode; uint32_t fcb; uint8_t path[NAMEBUF]; } __attribute__((packed));
struct res_open { int8_t res; uint32_t size; } __attribute__((packed));
struct cmd_close { uint8_t command; uint32_t fcb; } __attribute__((packed));
struct cmd_read { uint8_t command; uint32_t fcb, pos; uint16_t len; } __attribute__((packed));
struct res_read { int16_t len; uint8_t data[CONFIG_DATASIZE]; } __attribute__((packed));
struct cmd_write { uint8_t command; uint32_t fcb, pos; uint16_t len; uint8_t data[CONFIG_DATASIZE]; } __attribute__((packed));
struct res_write { int16_t len; } __attribute__((packed));
struct cmd_filedate { uint8_t command; uint32_t fcb; uint16_t time, date; } __attribute__((packed));
struct res_filedate { uint16_t time, date; } __attribute__((packed));
struct res_dskfre { int32_t res; uint16_t freeclu, totalclu, clusect, sectsize; } __attribute__((packed));

static union {
  struct cmd_dirop cmd_dirop; struct cmd_rename cmd_rename; struct cmd_chmod cmd_chmod;
  struct cmd_files cmd_files; struct cmd_nfiles cmd_nfiles; struct cmd_create cmd_create; struct cmd_open cmd_open;
  struct cmd_close cmd_close; struct cmd_read cmd_read; struct cmd_write cmd_write; struct cmd_filedate cmd_filedate;
  uint8_t raw[16];
} c;
static union {
  int8_t res; struct res_files res_files; struct res_open res_open; struct res_read res_read; struct res_write res_write;
  struct res_filedate res_filedate; struct res_dskfre res_dskfre; uint8_t raw[16];
} r;

extern long call_entry(void *entry, long cmd, long *args);
extern void dos_print(const char *);
extern void dos_putchar(int);

static void *entry;                     /* zmux.sys の入口 */
static int unit_base;
static int failed;                      /* この要求の中で Pi との交換に失敗した */

/* 動作確認用 (メモリダンプで "ZRMTSTAT" の後ろ) */
static struct { char tag[8]; uint32_t xchg, fail, rdbytes, wrbytes, cachehit; } st;

static void memcpy_(void *d, const void *s, int n) { uint8_t *x = d; const uint8_t *y = s; while (n--) *x++ = *y++; }
static void memset_(void *d, int v, int n) { uint8_t *x = d; while (n--) *x++ = v; }

/* デバイスチェインから "AUX     " + "ZMUX" (zmux.sys) を探す。入口はヘッダ +0x1a (zmux.x の find_zmux と同じ) */
static void *find_zmux(void)
{
  uint16_t *p;
  for (p = (uint16_t *)0x6800; p < (uint16_t *)0x68000; p++)
    if (p[0] == 0x4e55 && p[1] == 0x4c20 && p[2] == 0x2020 && p[3] == 0x2020) {   /* "NUL     " */
      uint8_t *d = (uint8_t *)p - 14;
      while (d != (uint8_t *)0xffffffff) {
        const uint8_t *n = d + 14;
        if (n[0] == 'A' && n[1] == 'U' && n[2] == 'X' && n[3] == ' ' && n[8] == 'Z' && n[9] == 'M' && n[10] == 'U' && n[11] == 'X')
          return d + 26;
        d = *(uint8_t **)d;
      }
      return 0;
    }
  return 0;
}

/* 要求 1 つを送って応答 1 つを受ける。0 = 成功、-1 = 失敗 (failed も立てる)。応答が rsize に足りなければ残りは 0 */
static int cmdres(void *wbuf, int wsize, void *rbuf, int rsize)
{
  long a[5];
  long n;
  int tries;
  if (failed)
    return -1;
  a[0] = (long)wbuf; a[1] = wsize; a[2] = (long)rbuf; a[3] = rsize; a[4] = TIMEOUT;
  for (tries = 0; tries < 2; tries++) {
    n = call_entry(entry, ZMUX_RD_XCHG, a);
    st.xchg++;
    if (n >= 0) {
      if (n < rsize)
        memset_((uint8_t *)rbuf + n, 0, rsize - n);
      return 0;
    }
    if (n != -3)                        /* 時間切れだけもう 1 回 */
      break;
  }
  st.fail++;
  failed = 1;
  return -1;
}

/* ---- read / write (CONFIG_DATASIZE ずつ) と dcache (remotedrv.c と同じ) */
static long send_read(uint32_t fcb, uint8_t *buf, uint32_t pos, long len)
{
  long total = 0;
  while (len > 0) {
    long size = len > CONFIG_DATASIZE ? CONFIG_DATASIZE : len;
    c.cmd_read.command = 0x4c;
    c.cmd_read.fcb = fcb;
    c.cmd_read.pos = pos;
    c.cmd_read.len = size;
    if (cmdres(&c.cmd_read, sizeof c.cmd_read, &r.res_read, sizeof r.res_read) < 0)
      return -1;
    if (r.res_read.len < 0)
      return r.res_read.len;
    if (r.res_read.len == 0)
      break;
    memcpy_(buf, r.res_read.data, r.res_read.len);
    buf += r.res_read.len;
    total += r.res_read.len;
    pos += r.res_read.len;
    len -= r.res_read.len;
    st.rdbytes += r.res_read.len;
  }
  return total;
}

static long send_write(uint32_t fcb, const uint8_t *buf, uint32_t pos, long len)
{
  long total = 0;
  do {
    long size = len > CONFIG_DATASIZE ? CONFIG_DATASIZE : len;
    c.cmd_write.command = 0x4d;
    c.cmd_write.fcb = fcb;
    c.cmd_write.pos = pos;
    c.cmd_write.len = size;
    memcpy_(c.cmd_write.data, buf, size);
    if (cmdres(&c.cmd_write, offsetof(struct cmd_write, data) + size, &r.res_write, sizeof r.res_write) < 0)
      return -1;
    if (r.res_write.len < 0)
      return r.res_write.len;
    buf += r.res_write.len;
    total += r.res_write.len;
    pos += r.res_write.len;
    len -= r.res_write.len;
    st.wrbytes += r.res_write.len;
  } while (len > 0);
  return total;
}

struct dcache { uint32_t fcb, pos; int16_t len; int dirty; uint8_t cache[CONFIG_DATASIZE]; };
static struct dcache dcache[CONFIG_NDCACHE];

static struct dcache *dcache_alloc(uint32_t fcb)
{
  int i;
  for (i = 0; i < CONFIG_NDCACHE; i++)
    if (dcache[i].fcb == fcb)
      return &dcache[i];
  for (i = 0; i < CONFIG_NDCACHE; i++)
    if (dcache[i].fcb == 0)
      return &dcache[i];
  return 0;
}

static int dcache_flush(uint32_t fcb, int clean)
{
  int i, res = 0;
  for (i = 0; i < CONFIG_NDCACHE; i++) {
    if (dcache[i].fcb == fcb) {
      if (dcache[i].dirty) {
        if (send_write(dcache[i].fcb, dcache[i].cache, dcache[i].pos, dcache[i].len) < 0)
          res = -1;
        dcache[i].dirty = 0;
      }
      if (clean)
        dcache[i].fcb = 0;
    }
  }
  return res;
}

static int atoi_(const char *p) { int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0'; return v; }

static long rd_init(struct req_header *req)
{
  extern char _end, __bss_start;
  char *b;
  const char *p;
  int units = 1;
  for (b = &__bss_start; b < &_end; b++)   /* CONFIG.SYS から組み込むドライバの BSS は 0 にならない */
    *b = 0;
  memcpy_(st.tag, "ZRMTSTAT", 8);
  /* 引数: DEVICE = \SYS\ZRMT.SYS /u2 (req->status はファイル名の次からの文字列の列) */
  p = (const char *)req->status;
  while (*p) p++;
  p++;
  while (*p) {
    if ((*p == '/' || *p == '-') && (p[1] | 0x20) == 'u') {
      units = atoi_(p + 2);
      if (units < 1 || units > 7) units = 1;
    }
    while (*p) p++;
    p++;
  }
  entry = find_zmux();
  if (!entry) {
    dos_print("zrmt.sys: zmux.sys (AUX) が組み込まれていないので組み込みません\r\n");
    return 0x1002;
  }
  dos_print("zrmt.sys 0.1: ドライブ ");
  dos_putchar('A' + *(uint8_t *)&req->fcb);
  if (units > 1) {
    dos_print(":-");
    dos_putchar('A' + *(uint8_t *)&req->fcb + units - 1);
  }
  dos_print(": は zmux (RS-232C) の先の Pi のディレクトリです\r\n");
  req->attr = units;
  req->addr = (uint8_t *)&_end;
  return 0;
}

long rd_interrupt(struct req_header *req)
{
  int op;
  failed = 0;
  if ((req->command & 0x1f) == 0)
    return rd_init(req);
  req->command = (req->command & 0x1f) | (((req->unit + unit_base) & 7) << 5);
  op = (req->command & 0x1f) | 0x40;
  switch (op) {
  case 0x41: case 0x42: case 0x43: case 0x45:   /* chdir, mkdir, rmdir, delete */
    c.cmd_dirop.command = req->command;
    memcpy_(c.cmd_dirop.path, req->addr, NAMEBUF);
    cmdres(&c.cmd_dirop, sizeof c.cmd_dirop, &r.res, 1);
    req->status = r.res;
    break;
  case 0x44:                            /* rename */
    c.cmd_rename.command = req->command;
    memcpy_(c.cmd_rename.path_old, req->addr, NAMEBUF);
    memcpy_(c.cmd_rename.path_new, (void *)req->status, NAMEBUF);
    cmdres(&c.cmd_rename, sizeof c.cmd_rename, &r.res, 1);
    req->status = r.res;
    break;
  case 0x46:                            /* chmod */
    c.cmd_chmod.command = req->command;
    c.cmd_chmod.attr = req->attr;
    memcpy_(c.cmd_chmod.path, req->addr, NAMEBUF);
    cmdres(&c.cmd_chmod, sizeof c.cmd_chmod, &r.res, 1);
    req->status = r.res;
    break;
  case 0x47:                            /* files */
    c.cmd_files.command = req->command;
    c.cmd_files.attr = req->attr;
    c.cmd_files.filep = req->status;
    memcpy_(c.cmd_files.path, req->addr, NAMEBUF);
    cmdres(&c.cmd_files, sizeof c.cmd_files, &r.res_files, sizeof r.res_files);
    if (!failed && r.res_files.res == 0)
      memcpy_((uint8_t *)req->status + FILBUF_ATR, &r.res_files.file.atr, sizeof r.res_files.file - 1);   /* FILBUF の atr から */
    req->status = r.res_files.res;
    break;
  case 0x48:                            /* nfiles */
    c.cmd_nfiles.command = req->command;
    c.cmd_nfiles.filep = req->status;
    cmdres(&c.cmd_nfiles, sizeof c.cmd_nfiles, &r.res_files, sizeof r.res_files);
    if (!failed && r.res_files.res == 0)
      memcpy_((uint8_t *)req->status + FILBUF_ATR, &r.res_files.file.atr, sizeof r.res_files.file - 1);
    req->status = r.res_files.res;
    break;
  case 0x49:                            /* create */
    c.cmd_create.command = req->command;
    c.cmd_create.attr = req->attr;
    c.cmd_create.mode = req->status;
    c.cmd_create.fcb = (uint32_t)req->fcb;
    memcpy_(c.cmd_create.path, req->addr, NAMEBUF);
    cmdres(&c.cmd_create, sizeof c.cmd_create, &r.res, 1);
    fcb_size(req->fcb) = 0;
    req->status = r.res;
    break;
  case 0x4a:                            /* open */
    c.cmd_open.command = req->command;
    c.cmd_open.mode = fcb_mode(req->fcb);
    c.cmd_open.fcb = (uint32_t)req->fcb;
    memcpy_(c.cmd_open.path, req->addr, NAMEBUF);
    cmdres(&c.cmd_open, sizeof c.cmd_open, &r.res_open, sizeof r.res_open);
    fcb_size(req->fcb) = r.res_open.size;
    req->status = r.res_open.res;
    break;
  case 0x4b:                            /* close */
    dcache_flush((uint32_t)req->fcb, 1);
    c.cmd_close.command = req->command;
    c.cmd_close.fcb = (uint32_t)req->fcb;
    cmdres(&c.cmd_close, sizeof c.cmd_close, &r.res, 1);
    req->status = r.res;
    break;
  case 0x4c: {                          /* read */
    uint32_t *pp = &fcb_fpos(req->fcb);
    uint8_t *buf = req->addr;
    long len = req->status, size = 0, rlen;
    struct dcache *d;
    dcache_flush((uint32_t)req->fcb, 0);
    if ((d = dcache_alloc((uint32_t)req->fcb))) {
      do {
        if (d->fcb == (uint32_t)req->fcb && *pp >= d->pos && *pp < d->pos + d->len) {
          long clen = d->pos + d->len - *pp;
          if (clen > len) clen = len;
          memcpy_(buf, d->cache + (*pp - d->pos), clen);
          buf += clen; len -= clen; size += clen; *pp += clen;
          st.cachehit++;
        }
        if (len == 0 || len >= (long)sizeof d->cache)
          break;
        dcache_flush((uint32_t)req->fcb, 1);
        d->len = send_read((uint32_t)req->fcb, d->cache, *pp, sizeof d->cache);
        if (d->len < 0) { size = -1; goto out_read; }
        d->fcb = (uint32_t)req->fcb;
        d->pos = *pp;
        d->dirty = 0;
      } while (d->len > 0);
    }
    if (len > 0) {
      rlen = send_read((uint32_t)req->fcb, buf, *pp, len);
      if (rlen < 0) { size = -1; goto out_read; }
      size += rlen;
      *pp += rlen;
    }
  out_read:
    req->status = size;
    break;
  }
  case 0x4d: {                          /* write */
    uint32_t *pp = &fcb_fpos(req->fcb);
    uint32_t *sp = &fcb_size(req->fcb);
    long len = req->status;
    struct dcache *d;
    if (len > 0 && len < (long)sizeof dcache[0].cache) {
      if ((d = dcache_alloc((uint32_t)req->fcb))) {
        if (d->fcb == (uint32_t)req->fcb) {
          if (*pp == d->pos + d->len && *pp + len <= d->pos + sizeof d->cache) {
            memcpy_(d->cache + d->len, req->addr, len);
            d->len += len;
            d->dirty = 1;
            goto ok_write;
          }
          dcache_flush((uint32_t)req->fcb, 1);
        }
        d->fcb = (uint32_t)req->fcb;
        d->pos = *pp;
        memcpy_(d->cache, req->addr, len);
        d->len = len;
        d->dirty = 1;
        goto ok_write;
      }
    }
    dcache_flush((uint32_t)req->fcb, 0);
    len = send_write((uint32_t)req->fcb, req->addr, *pp, req->status);
    if (len == 0)
      *sp = *pp;                        /* 0 バイトの書き込みは切り詰め */
  ok_write:
    if (len > 0) {
      *pp += len;
      if (*pp > *sp) *sp = *pp;
    }
    req->status = len;
    break;
  }
  case 0x4e: {                          /* seek (Pi には聞かない) */
    int whence = req->attr;
    int32_t off = req->status;
    uint32_t pos = fcb_fpos(req->fcb), size = fcb_size(req->fcb);
    dcache_flush((uint32_t)req->fcb, 0);
    pos = (whence == 0 ? 0 : whence == 1 ? pos : size) + off;
    if (pos > size)
      req->status = _DOSE_CANTSEEK;
    else
      req->status = fcb_fpos(req->fcb) = pos;
    break;
  }
  case 0x4f:                            /* filedate */
    c.cmd_filedate.command = req->command;
    c.cmd_filedate.fcb = (uint32_t)req->fcb;
    c.cmd_filedate.time = req->status & 0xffff;
    c.cmd_filedate.date = req->status >> 16;
    cmdres(&c.cmd_filedate, sizeof c.cmd_filedate, &r.res_filedate, sizeof r.res_filedate);
    req->status = r.res_filedate.time + ((uint32_t)r.res_filedate.date << 16);
    break;
  case 0x50: {                          /* dskfre */
    uint16_t *p = (uint16_t *)req->addr;
    c.raw[0] = req->command;
    cmdres(c.raw, 1, &r.res_dskfre, sizeof r.res_dskfre);
    p[0] = r.res_dskfre.freeclu; p[1] = r.res_dskfre.totalclu; p[2] = r.res_dskfre.clusect; p[3] = r.res_dskfre.sectsize;
    req->status = r.res_dskfre.res;
    break;
  }
  case 0x51:                            /* drvctrl */
    req->attr = 2;
    req->status = 0;
    break;
  case 0x52: {                          /* getdpb: 仮のセクタ長 */
    uint8_t *p = req->addr;
    memset_(p, 0, 16);
    *(uint16_t *)&p[0] = 512;
    p[2] = 1;
    req->status = 0;
    break;
  }
  default:                              /* diskred, diskwrt, ioctl, abort, mediacheck, lock */
    req->status = 0;
    break;
  }
  if (failed) {
    /* zmuxd が居ない / 線が UP でない / 時間切れ。デバイスのエラー (0x1002) にすると Human68k の「ディスクが入っていません 中止/再実行/無視」の
       箱が出てキー待ちになり、Z-OS の画面ではペインを無視して出る。箱の出ない DOS エラー (-15 ドライブ指定が無効) にして呼んだ側に返す */
    req->status = -15;
    return 0;
  }
  return 0;
}
