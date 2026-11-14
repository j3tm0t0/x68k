/*
 * picosim -- X68000 Z に挿した Pico W (x68kzremotedrv の netpipe ファーム) の代わりを Mac の上でする。
 *            XEiJ (harness/xeij/agent/PicoBridge.java) が SCSI の命令を TCP で渡してくる
 *
 *   picosim [-l 127.0.0.1:6868] [-n 192.168.1.0/24] [-g 192.168.1.1] [-D DNS] [-L [u]PORT:ZPORT]...
 *           [-0 disk.hds] [-H ID:データの HDS]... [-G ID:ゲームの DIR] [-T Wi-Fi,SMB,HDS の秒] [-N 秒] [-B 控え] [-F ファームの driver/] [-C 制御] [-K 設定] [-R B: の中身]... [-X BlueSCSI のリモートドライブの中身]... [-c MB] [-r KB] [-d µs] [-a 倍率] [-s 秒] [-p lan.pcap] [-v]
 *
 * 受け持つ SCSI ID:
 *   ID 6 = netpipe (netpipe.c)。LBA 0x100000 の要求と 0x110000〜0x1ffff7 の応答。ほかの LBA は 0 を返し、書き込みは捨てる
 *          (ファームの ID 6 にある scsiremote.sys / 設定画面 / 起動待ち IPL は真似ない。LBA 0 が 0 なので Human68k は
 *          ディスクとして扱わない)
 *   ID 0 = -0 の HDS ファイル (書き込みも反映)。-T で Pico Z Loader の起動待ち (bootwait.c)。-N n は -T 0,0,n
 * Z の Linux の読み込みキャッシュ (-c MB、既定 64、0 で無し) と先読み (-r KB、既定 16 = 4KB ページ 4 枚) も真似る:
 *   読んだセクタは覚えておき、同じ (ID, LBA) は Pico に聞かずに前の中身を返す。npd.x が応答帯を毎回別の所で読むのはこのため
 * 形式 (PicoBridge.java と同じ、ビッグエンディアン):
 *   要求 32 + n: "PSQ1" seq(4) id path cdblen 0 out_len(4) cdb(16) data
 *   応答 16 + n: "PSA1" seq(4) status key asc ascq in_len(4) data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "picosim.h"

extern double np_accel_scale;
extern FILE *lan_pcap;
extern int pz_vd_page_by_lba;
extern int pz_face_id;
int pz_face_read(uint32_t lba, uint8_t *buf);
void remote_add(const char *dir);
/* x68rd.c (BlueSCSI のリモートドライブ、-X) は remoteserv.c と一緒にだけリンクする。無いときはこの弱い定義 */
__attribute__((weak)) int x68rd_read(uint32_t lba, uint8_t *buf) { (void)lba; (void)buf; return -1; }
__attribute__((weak)) int x68rd_write(uint32_t lba, const uint8_t *buf) { (void)lba; (void)buf; return -1; }
__attribute__((weak)) int x68rd_range(uint32_t lba) { (void)lba; return 0; }
int x68rd_on;
int picosim_verbose;
static uint64_t t0_us;

uint64_t picosim_now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - t0_us;
}

void picosim_log(const char *fmt, ...)
{
  va_list ap;
  uint64_t t = picosim_now_us();
  fprintf(stderr, "%5llu.%03llu ", (unsigned long long)(t / 1000000), (unsigned long long)(t / 1000 % 1000));
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* ---- SCSI の対象 ---- */
struct target {
  int present;
  const char *name;
  uint32_t sects;
  uint8_t key, asc, ascq;               /* 次の REQUEST SENSE */
  uint32_t reads, writes;
};
static struct target tg[8];
static uint32_t cmd_delay_us;

/* ---- --pzl: Pico Z Loader のファーム (pzl/、x68k-zos の branch pzl) の SCSI の層をそのまま使う ----
 * pzl/build/pzl_all.o をリンクしたとき (build/picosim-pzl) だけ。ID 0〜6 の読み書きはすべて PZL の vd_read_block /
 * vd_write_block に (Z の Linux が pscsi.ini 越しにするのと同じ番地)。bootwait.c と netpipe.c は使わない */
static int use_pzl;
__attribute__((weak)) int pzlh_init(const char *c, const char *r, const char *f, double a, double b, double d, uint32_t ip, uint32_t gw)
{
  (void)c; (void)r; (void)f; (void)a; (void)b; (void)d; (void)ip; (void)gw;
  fprintf(stderr, "picosim: built without PZL (make build/picosim-pzl)\n");
  return -1;
}
__attribute__((weak)) void pzlh_poll(double t) { (void)t; }
__attribute__((weak)) int pzlh_read(int id, uint32_t lba, uint8_t *buf) { (void)id; (void)lba; (void)buf; return PS_IOERR; }
__attribute__((weak)) int pzlh_write(int id, uint32_t lba, const uint8_t *buf) { (void)id; (void)lba; (void)buf; return PS_IOERR; }
__attribute__((weak)) int pzlh_present(int id) { (void)id; return 0; }
__attribute__((weak)) uint32_t pzlh_sects(int id) { (void)id; return 0; }
__attribute__((weak)) int pzlh_ready(int id) { (void)id; return PS_IOERR; }
__attribute__((weak)) int pzlh_sysstatus(void) { return 0; }
__attribute__((weak)) int pzlh_rebooted(void) { return 0; }

/* 1 セクタ。戻り値 PS_OK / PS_NOTREADY / PS_IOERR (ID 0 と ID 6 の中身は bootwait.c が決める) */
static int t_read(int id, uint32_t lba, uint8_t *buf)
{
  tg[id].reads++;
  if (use_pzl)
    return pzlh_read(id, lba, buf);
  if (id == pz_face_id && id != 6)
    return pz_face_read(lba, buf);
  if (id == 6 && x68rd_read(lba, buf) == 0)
    return PS_OK;
  return id == 6 ? pz_id6_read(lba, buf) : id == 0 ? pz_id0_read(lba, buf) : pz_hds_read(id, lba, buf);
}

static int t_write(int id, uint32_t lba, const uint8_t *buf)
{
  tg[id].writes++;
  if (use_pzl)
    return pzlh_write(id, lba, buf);
  if (id == 6 && x68rd_write(lba, buf) == 0)
    return PS_OK;
  return id == 6 ? pz_id6_write(lba, buf) : id == 0 ? pz_id0_write(lba, buf) : pz_hds_write(id, lba, buf);
}

/* ---- Z の Linux の読み込みキャッシュ ---- */
static uint32_t c_cap, c_hand, c_ra_pages = 4, *c_bucket, *c_next, c_nbucket;
static uint64_t *c_key;
static uint8_t *c_data;
static uint32_t st_hit, st_miss;
#define C_NONE 0xffffffffu

static uint32_t c_hash(uint64_t k) { return (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 40) & (c_nbucket - 1); }

static void cache_init(uint32_t mb)
{
  c_cap = mb * 2048;
  if (!c_cap)
    return;
  for (c_nbucket = 1; c_nbucket < c_cap; c_nbucket <<= 1)
    ;
  c_bucket = malloc(c_nbucket * sizeof *c_bucket);
  c_next = malloc(c_cap * sizeof *c_next);
  c_key = malloc(c_cap * sizeof *c_key);
  c_data = malloc((size_t)c_cap * 512);
  if (!c_bucket || !c_next || !c_key || !c_data) {
    fprintf(stderr, "picosim: no memory for a %u MB cache\n", mb);
    exit(1);
  }
  memset(c_bucket, 0xff, c_nbucket * sizeof *c_bucket);
  memset(c_key, 0xff, c_cap * sizeof *c_key);
}

static uint8_t *cache_find(uint64_t k)
{
  for (uint32_t s = c_bucket[c_hash(k)]; s != C_NONE; s = c_next[s])
    if (c_key[s] == k)
      return c_data + (size_t)s * 512;
  return NULL;
}

static void cache_put(uint64_t k, const uint8_t *buf)
{
  uint8_t *d = cache_find(k);
  if (!d) {
    uint32_t s = c_hand++ % c_cap;
    if (c_key[s] != ~0ull) {            /* 古いものから追い出す */
      for (uint32_t *p = &c_bucket[c_hash(c_key[s])]; *p != C_NONE; p = &c_next[*p])
        if (*p == s) {
          *p = c_next[s];
          break;
        }
    }
    c_key[s] = k;
    uint32_t h = c_hash(k);
    c_next[s] = c_bucket[h];
    c_bucket[h] = s;
    d = c_data + (size_t)s * 512;
  }
  memcpy(d, buf, 512);
}

void picosim_cache_clear(void)
{
  if (!c_cap)
    return;
  memset(c_bucket, 0xff, c_nbucket * sizeof *c_bucket);
  memset(c_key, 0xff, c_cap * sizeof *c_key);
  picosim_log("Z read cache cleared\n");
}

/* 失敗 (NOT READY など) はキャッシュに残らない (Z の Linux も失敗した読み込みは覚えない) */
static int z_read(int id, uint32_t lba, uint8_t *buf)
{
  uint64_t k = (uint64_t)id << 32 | lba;
  uint8_t *d;
  int rc = PS_OK;
  if (!c_cap || (id == 6 && x68rd_range(lba)))   /* BlueSCSI のリモートドライブ (-X) はキャッシュを通さない */
    return t_read(id, lba, buf);
  if ((d = cache_find(k)) != NULL) {
    st_hit++;
    memcpy(buf, d, 512);
    if (picosim_verbose >= 3 && id == 6 && lba >= 0x200000)
      picosim_log("vdbuf: Z reads 0x%x from its cache: seq %u page %d/%d\n", lba, g32(buf + 8), buf[12], buf[13]);
    return PS_OK;
  }
  st_miss++;
  /* 先読み: 4KB のページ単位で、外れたページから c_ra_pages 枚を Pico から読んで覚える */
  uint32_t first = lba & ~7u, end = first + (c_ra_pages ? c_ra_pages : 1) * 8;
  if (end > tg[id].sects)
    end = tg[id].sects;
  for (uint32_t l = first; l < end; l++) {
    uint64_t kk = (uint64_t)id << 32 | l;
    if (l != lba && cache_find(kk))
      continue;
    uint8_t tmp[512];
    int r = t_read(id, l, tmp);
    if (r == PS_OK)
      cache_put(kk, tmp);
    if (picosim_verbose >= 3 && id == 6 && l >= 0x200000)
      picosim_log("vdbuf: %s 0x%x from the Pico: seq %u page %d/%d\n", l == lba ? "Z reads" : "readahead", l, g32(tmp + 8),
                  tmp[12], tmp[13]);
    if (l == lba) {
      memcpy(buf, tmp, 512);
      rc = r;
      if (r != PS_OK)
        break;
    }
  }
  return rc;
}

static int z_write(int id, uint32_t lba, const uint8_t *buf)
{
  int r = t_write(id, lba, buf);
  if (r != PS_OK)
    return r;
  if (c_cap && id != 6)
    cache_put((uint64_t)id << 32 | lba, buf);
  else if (c_cap && cache_find((uint64_t)id << 32 | lba))
    cache_put((uint64_t)id << 32 | lba, buf);
  return r;
}

/* ---- 起動の制御 (netpipe のレコード 0xA8。x68k-zos #172 の起動メニューが ID 0 の IPL から送る) ----
 *   要求 [0xA8][op][引数]  応答 [0xA8][op][status][値]
 *   op 0 PING        -> [版 1][隠している ID のビット]
 *   op 3 HIDE [ビット] -> [隠している ID のビット]。ビット n の ID は、次の SCSI バスリセットまで選択に応答しない
 *                        (ID 6 自身は隠さない)。BlueSCSI の x68k-bootsel のファーム (BlueSCSI_x68np.cpp) と同じ決まり。
 *   op 1 LIST [ID][番号]   -> [数][今の番号][長さ][表示名]。ゲーム用の ID (-G ID:DIR) に見せられるイメージの番号番目
 *   op 2 SELECT [ID][番号] -> [番号]。その ID のイメージを番号番目のファイルに替える (下の「ゲーム用の ID」)
 * XEiJ はバスリセット (SPC の RST 出力) をまねないので、橋 (PicoBridge) が XEiJ のリセットのたびに path 3 で知らせる */
#define BOOT_MARK 0xa8
static uint8_t hidden_ids;

/* ---- ゲーム用の ID (-G ID:DIR。x68k-zos の起動メニューの「イメージを選ぶ」項目) ----
 * DIR の *.hds (と *.hda) を名前順 (大文字小文字は区別しない) に並べたものが一覧。'.' で始まる名前と、39 バイトを
 * 超える名前は入れない。64 個まで。表示名は、同じ名前の .txt があればその 1 行目 (40 バイトまで)、無ければ拡張子を
 * 除いたファイル名。選んだファイル名は DIR/lastgame.txt に書き、次に起動したときはそのイメージから始める。
 * LIST の番号 0 で DIR を読み直す。SELECT の番号は、最後に読み直したときの一覧のもの。
 * status: 0 = 済んだ、2 = 要求が短い、3 = 知らない op、4 = その ID はゲーム用でない、5 = 番号が一覧の外、
 *         6 = 開けない (前のイメージのまま) */
#define GAME_MAX   64
#define GAME_NAME  40
#define GAME_LABEL 40
static int game_id = -1, game_n, game_cur = -1;
static const char *game_dir;
static char game_name[GAME_MAX][GAME_NAME], game_now[GAME_NAME];

static void game_scan(void)
{
  DIR *d = opendir(game_dir);
  struct dirent *e;
  game_n = 0;
  game_cur = -1;
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {    /* 名前順に差し込む。64 個を超えたら、名前順で後ろのものを落とす */
    size_t n = strlen(e->d_name);
    int pos = game_n;
    if (e->d_name[0] == '.' || n < 5 || n >= GAME_NAME ||
        (strcasecmp(e->d_name + n - 4, ".hds") && strcasecmp(e->d_name + n - 4, ".hda")))
      continue;
    while (pos > 0 && strcasecmp(game_name[pos - 1], e->d_name) > 0)
      pos--;
    if (pos >= GAME_MAX)
      continue;
    if (game_n < GAME_MAX)
      game_n++;
    memmove(game_name[pos + 1], game_name[pos], (size_t)(game_n - 1 - pos) * GAME_NAME);
    strcpy(game_name[pos], e->d_name);
  }
  closedir(d);
  for (int i = 0; i < game_n; i++)
    if (!strcasecmp(game_name[i], game_now))
      game_cur = i;
}

static int game_label(int i, uint8_t *out)
{
  char path[1200], line[GAME_LABEL + 1];
  int n = (int)strlen(game_name[i]) - 4;
  FILE *f;
  snprintf(path, sizeof path, "%s/%.*s.txt", game_dir, n, game_name[i]);
  if ((f = fopen(path, "rb")) != NULL) {
    size_t k = fread(line, 1, GAME_LABEL, f);
    fclose(f);
    line[k] = 0;
    k = strcspn(line, "\r\n\x1a");
    if (k) {
      memcpy(out, line, k);
      return (int)k;
    }
  }
  memcpy(out, game_name[i], n);
  return n;
}

static int game_open(int i, int first)
{
  char path[1200];
  struct stat st;
  snprintf(path, sizeof path, "%s/%s", game_dir, game_name[i]);
  if (stat(path, &st) < 0 || st.st_size < 512)
    return -1;
  if (first)
    pz_hds_add(game_id, strdup(path));
  else if (pz_hds_switch(game_id, path) < 0)
    return -1;
  tg[game_id].present = 1;
  tg[game_id].name = "game";
  tg[game_id].sects = (uint32_t)(st.st_size / 512);
  strcpy(game_now, game_name[i]);
  game_cur = i;
  return 0;
}

/* -G: 前に選んだもの (lastgame.txt) があればそれ、無ければ一覧の先頭を開く */
static void game_init(int id, const char *dir)
{
  char path[1200], last[GAME_NAME + 2] = "";
  FILE *f;
  int i, pick = 0;
  game_id = id;
  game_dir = dir;
  game_scan();
  snprintf(path, sizeof path, "%s/lastgame.txt", dir);
  if ((f = fopen(path, "rb")) != NULL) {
    if (fgets(last, sizeof last, f))
      last[strcspn(last, "\r\n")] = 0;
    fclose(f);
  }
  for (i = 0; i < game_n; i++)
    if (!strcasecmp(game_name[i], last))
      pick = i;
  if (game_n == 0 || game_open(pick, 1) < 0) {
    picosim_log("games: no image in %s, id%d is not a game slot\n", dir, id);
    game_id = -1;
    return;
  }
  picosim_log("games: id%d = %s/%s (%d images, %u sectors)\n", id, dir, game_now, game_n, tg[id].sects);
}

static int game_exchange(const uint8_t *req, int reqlen, uint8_t *res)
{
  int i;
  if (reqlen < 4) {
    res[2] = 2;
    return 3;
  }
  if (game_id < 0 || req[2] != game_id) {
    res[2] = 4;
    return 3;
  }
  i = req[3];
  if (req[1] == 1) {                    /* LIST */
    if (i == 0)
      game_scan();
    res[3] = game_n;
    res[4] = game_cur < 0 ? 0xff : game_cur;
    res[5] = 0;
    if (i >= game_n) {
      res[2] = 5;
      return 6;
    }
    res[5] = game_label(i, res + 6);
    return 6 + res[5];
  }
  res[3] = i;                           /* SELECT */
  if (i >= game_n) {
    res[2] = 5;
    return 4;
  }
  if (i != game_cur) {
    if (game_open(i, 0) < 0) {
      picosim_log("games: cannot open %s, id%d stays on %s\n", game_name[i], game_id, game_now);
      res[2] = 6;
      return 4;
    }
    picosim_cache_clear();
  }
  {                                     /* 今のイメージを選び直したときも書く (lastgame.txt がまだ無いことがある) */
    char path[1200];
    FILE *f;
    snprintf(path, sizeof path, "%s/lastgame.txt", game_dir);
    if ((f = fopen(path, "wb")) != NULL) {
      fprintf(f, "%s\r\n", game_now);
      fclose(f);
    }
  }
  picosim_log("games: id%d = %s (%u sectors)\n", game_id, game_now, tg[game_id].sects);
  return 4;
}

static int bootctl_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  if (reqlen < 2 || resmax < 5)
    return 0;
  res[0] = BOOT_MARK;
  res[1] = req[1];
  res[2] = 0;
  switch (req[1]) {
  case 0:
    res[3] = 1;
    res[4] = hidden_ids;
    return 5;
  case 1: case 2:
    if (resmax < 6 + GAME_LABEL)
      return 0;
    return game_exchange(req, reqlen, res);
  case 3:
    if (reqlen < 3) {
      res[2] = 2;
      return 3;
    }
    hidden_ids = req[2] & ~(1 << 6);
    picosim_log("bootctl: SCSI IDs hidden until the next bus reset, mask 0x%02x\n", hidden_ids);
    res[3] = hidden_ids;
    if (hidden_ids && getenv("PICOSIM_HIDE_BADREPLY")) {   /* 試験用: 隠したのに、応答は失敗にする (取り消しの試験) */
      res[2] = 1;
      return 3;
    }
    return 4;
  }
  res[2] = 3;
  return 3;
}

static const struct picosim_svc bootctl_svc = { "bootctl", BOOT_MARK, NULL, bootctl_exchange, NULL };
__attribute__((constructor)) static void bootctl_reg(void) { picosim_register_svc(&bootctl_svc); }

/* ---- 命令 ---- */
static void check(struct ps_result *r, struct target *t, int key, int asc, int ascq)
{
  r->status = 2;
  r->key = t->key = key;
  r->asc = t->asc = asc;
  r->ascq = t->ascq = ascq;
  r->len = 0;
}

static void scsi(int id, const uint8_t *cdb, const uint8_t *out, uint32_t outlen, struct ps_result *r, uint8_t *in)
{
  struct target *t = &tg[id & 7];
  int op = cdb[0], lun = cdb[1] >> 5;
  uint32_t lba = 0, n = 0, alloc;
  memset(r, 0, sizeof *r);
  if (use_pzl)
    t->present = pzlh_present(id & 7);
  if (!t->present || (hidden_ids >> (id & 7) & 1)) {
    /* 対象の無い ID・隠している ID: 新しい橋は先に path 2 で確かめて選択のタイムアウトにする。古い橋のために LUN が無いことにする */
    static struct target none;
    check(r, &none, 5, 0x25, 0);
    return;
  }
  if (lun && op != 0x03 && op != 0x12) {
    check(r, t, 5, 0x25, 0);
    return;
  }
  int notready;
  if (use_pzl) {
    pzlh_poll((double)picosim_now_us() / 1e6);
    t->present = pzlh_present(id);
    t->sects = pzlh_sects(id);
    notready = t->present && pzlh_ready(id) == PS_NOTREADY;
  } else {
    pz_tick();
    if (id == 0 && pz.hds_fd >= 0)
      t->sects = pz.hds_sects;
    notready = id == 0 ? !pz_id0_ready() : id != 6 && id != pz_face_id && !pz_hds_ready(id);
  }
  switch (op) {
  case 0x00:                            /* TEST UNIT READY */
    if (notready)
      check(r, t, 2, 0x04, 0x01);
    return;
  case 0x03:                            /* REQUEST SENSE */
    alloc = cdb[4] ? cdb[4] : 4;
    memset(in, 0, 18);
    in[0] = 0x70;
    in[2] = t->key;
    in[7] = 10;
    in[12] = t->asc;
    in[13] = t->ascq;
    t->key = t->asc = t->ascq = 0;
    r->len = alloc < 18 ? alloc : 18;
    return;
  case 0x12:                            /* INQUIRY */
    alloc = cdb[4];
    memset(in, 0, 36);
    in[0] = lun ? 0x7f : 0x00;
    in[2] = 2;
    in[3] = 2;
    in[4] = 31;
    memcpy(in + 8, "PICOSIM ", 8);
    /* ID 6 の製品名は "X68NP" で始める。起動メニュー (x68k-zos boot/bootmenu) が、要求を書く前に netpipe の装置かを
     * これで見分ける。BlueSCSI の x68k-bootsel のファームも同じ製品名を返す */
    memcpy(in + 16, id == 6 ? "X68NP netpipe   " : "HDS image       ", 16);
    if (id == 6 && getenv("PICOSIM_ID6_PRODUCT")) {   /* 試験用: 目印の無い古いファームのまね */
      size_t n = strlen(getenv("PICOSIM_ID6_PRODUCT"));
      memset(in + 16, ' ', 16);
      memcpy(in + 16, getenv("PICOSIM_ID6_PRODUCT"), n < 16 ? n : 16);
    }
    memcpy(in + 32, "1.0 ", 4);
    r->len = alloc < 36 ? alloc : 36;
    return;
  case 0x25:                            /* READ CAPACITY */
    if (notready) {
      check(r, t, 2, 0x04, 0x01);
      return;
    }
    p32(in, t->sects - 1);
    p32(in + 4, 512);
    r->len = 8;
    return;
  case 0x1a:                            /* MODE SENSE(6) */
    alloc = cdb[4];
    memset(in, 0, 12);
    in[0] = 11;
    in[3] = 8;
    p32(in + 4, t->sects);
    p32(in + 8, 512);
    r->len = alloc < 12 ? alloc : 12;
    return;
  case 0x08: case 0x0a:                 /* READ(6) / WRITE(6) */
    lba = (cdb[1] & 31) << 16 | cdb[2] << 8 | cdb[3];
    n = cdb[4] ? cdb[4] : 256;
    break;
  case 0x28: case 0x2a: case 0x2e:      /* READ(10) / WRITE(10) / WRITE AND VERIFY(10) */
    lba = g32(cdb + 2);
    n = cdb[7] << 8 | cdb[8];
    break;
  case 0xa8: case 0xaa:                 /* READ(12) / WRITE(12) */
    lba = g32(cdb + 2);
    n = g32(cdb + 6);
    break;
  case 0x01: case 0x0b: case 0x2b: case 0x15: case 0x1b: case 0x1e: case 0x35:
    return;                             /* REZERO / SEEK / MODE SELECT / START STOP / PREVENT / SYNC CACHE: GOOD */
  default:
    check(r, t, 5, 0x20, 0);
    return;
  }
  if ((uint64_t)lba + n > t->sects || n * 512 > PS_DATA_MAX) {
    check(r, t, 5, 0x21, 0);
    return;
  }
  if (op == 0x08 || op == 0x28 || op == 0xa8) {
    for (uint32_t i = 0; i < n; i++) {
      int rc = z_read(id, lba + i, in + i * 512);
      if (rc != PS_OK) {
        if (rc == PS_NOTREADY)
          check(r, t, 2, 0x04, 0x01);
        else
          check(r, t, 3, 0x11, 0);
        return;
      }
    }
    r->len = n * 512;
  } else {
    if (outlen < n * 512) {
      check(r, t, 5, 0x24, 0);
      return;
    }
    for (uint32_t i = 0; i < n; i++) {
      int rc = z_write(id, lba + i, out + i * 512);
      if (rc == PS_NOTREADY) {
        check(r, t, 2, 0x04, 0x01);
        return;
      } else if (rc != PS_OK) {
        check(r, t, 3, 0x0c, 0);
        return;
      }
    }
  }
}

/* ---- 橋 (XEiJ) ---- */
#define MAXCLI 4
static int lfd, cli[MAXCLI];
static uint8_t *ibuf, *obuf;
static uint32_t st_cmds, st_spc, st_mbx;

static int readn(int fd, void *p, size_t n)
{
  uint8_t *b = p;
  while (n) {
    ssize_t k = read(fd, b, n);
    if (k <= 0) {
      if (k < 0 && errno == EINTR)
        continue;
      return -1;
    }
    b += k;
    n -= k;
  }
  return 0;
}

static int writen(int fd, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n) {
    ssize_t k = write(fd, b, n);
    if (k <= 0) {
      if (k < 0 && errno == EINTR)
        continue;
      return -1;
    }
    b += k;
    n -= k;
  }
  return 0;
}

static int serve_one(int fd)
{
  uint8_t h[32], rh[16];
  struct ps_result r;
  if (readn(fd, h, 32) < 0 || g32(h) != 0x50535131)   /* "PSQ1" */
    return -1;
  uint32_t seq = g32(h + 4), outlen = g32(h + 12);
  int id = h[8] & 7, path = h[9];
  if (outlen > PS_DATA_MAX || readn(fd, ibuf, outlen) < 0)
    return -1;
  if (path == 2 || path == 3) {
    /* path 2: この ID は選択に応答するか (status 0 = する、0xff = しない)。path 3: SCSI のバスリセット */
    memset(&r, 0, sizeof r);
    if (path == 3) {
      if (hidden_ids)
        picosim_log("bootctl: bus reset, hidden IDs 0x%02x are back\n", hidden_ids);
      else if (picosim_verbose)
        picosim_log("bootctl: bus reset\n");
      hidden_ids = 0;
    } else {
      if (use_pzl)
        tg[id].present = pzlh_present(id);
      if (!tg[id].present || (hidden_ids >> id & 1))
        r.status = 0xff;
    }
  } else
    scsi(id, h + 16, ibuf, outlen, &r, obuf);
  st_cmds++;
  if (path == 1)
    st_mbx++;
  else
    st_spc++;
  if (picosim_verbose >= 2) {
    uint32_t lba = h[16] == 0x08 || h[16] == 0x0a ? (h[17] & 31) << 16 | h[18] << 8 | h[19] : g32(h + 18);
    picosim_log("id%d %s %02x lba=0x%x out=%u -> st=%d %x/%02x/%02x in=%u\n", id, path ? "mbx" : "spc", h[16], lba,
                outlen, r.status, r.key, r.asc, r.ascq, r.len);
  }
  if (cmd_delay_us)
    usleep(cmd_delay_us);
  p32(rh, 0x50534131);                  /* "PSA1" */
  p32(rh + 4, seq);
  rh[8] = r.status;
  rh[9] = r.key;
  rh[10] = r.asc;
  rh[11] = r.ascq;
  p32(rh + 12, r.len);
  if (writen(fd, rh, 16) < 0 || (r.len && writen(fd, obuf, r.len) < 0))
    return -1;
  return 0;
}

static int listen_on(const char *spec)
{
  char host[64] = "127.0.0.1";
  int port = 6868, one = 1;
  const char *c = strrchr(spec, ':');
  if (c) {
    snprintf(host, sizeof host, "%.*s", (int)(c - spec), spec);
    port = atoi(c + 1);
  } else
    port = atoi(spec);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
  inet_pton(AF_INET, host, &a.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
    perror("picosim: listen");
    exit(1);
  }
  picosim_log("bridge: listening on %s:%d\n", host, port);
  return fd;
}

static volatile sig_atomic_t want_stats, want_quit;
static void on_sig(int s) { if (s == SIGUSR1) want_stats = 1; else want_quit = 1; }

static void stats(void)
{
  fprintf(stderr, "picosim: %u commands (%u SPC, %u mailbox)  Z cache hit %u miss %u\n", st_cmds, st_spc, st_mbx, st_hit, st_miss);
  np_stats();
  lan_stats();
}

static uint32_t ip_arg(const char *s)
{
  struct in_addr a;
  if (inet_pton(AF_INET, s, &a) != 1) {
    fprintf(stderr, "picosim: bad address %s\n", s);
    exit(2);
  }
  return a.s_addr;
}

static void usage(void)
{
  fprintf(stderr,
          "usage: picosim [--pzl[=config.txt] [--pzl-root=DIR] [--pzl-flash=F] [--pzl-ip=A] [--pzl-zip=A]] [-l 127.0.0.1:6868] [-n 192.168.1.0/24] [-g GW] [-D DNS] [-L [u]PORT:ZPORT|none]...\n"
          "               [-0 disk.hds] [-H ID:data.hds]... [-G ID:gamedir] [-T wifi,smb,hds] [-N sec] [-B snap] [-F fwdir] [-C ctlfile] [-K config] [-R dir]... [-X dir]... [-c MB] [-r KB] [-d us] [-a scale] [-s sec] [-p lan.pcap] [-v]\n");
  exit(2);
}

int main(int argc, char **argv)
{
  const char *pzl_cfg = NULL, *pzl_root = ".", *pzl_flash = NULL, *pzl_ip = "192.168.1.118", *pzl_zip = "192.168.1.68";
  int pzl_on = 0;
  /* --pzl[=config.txt] [--pzl-root=DIR] [--pzl-flash=FILE] [--pzl-ip=A] [--pzl-zip=A] (getopt の前に取り除く)。
     config.txt が無ければフラッシュ (--pzl-flash) の設定 */
  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--pzl", 5)) {
      const char *a = argv[i];
      if (!strcmp(a, "--pzl"))
        pzl_on = 1;
      else if (!strncmp(a, "--pzl=", 6))
        pzl_on = 1, pzl_cfg = a + 6;
      else if (!strncmp(a, "--pzl-root=", 11))
        pzl_root = a + 11;
      else if (!strncmp(a, "--pzl-flash=", 12))
        pzl_flash = a + 12;
      else if (!strncmp(a, "--pzl-ip=", 9))
        pzl_ip = a + 9;
      else if (!strncmp(a, "--pzl-zip=", 10))
        pzl_zip = a + 10;
      else
        usage();
      memmove(argv + i, argv + i + 1, (argc - i) * sizeof *argv);
      argc--;
      i--;
    }
  const char *listen_spec = "127.0.0.1:6868", *hds0 = NULL;
  int o, nr_sec = 0, cache_mb = 64, ra_kb = 16, stat_sec = 0, nofwd = 0, prefix = 24;
  t0_us = 0;
  t0_us = picosim_now_us();
  lan_cfg.net = ip_arg("192.168.1.0");
  lan_cfg.gw = ip_arg("192.168.1.1");
  lan_cfg.dns = 0;
  while ((o = getopt(argc, argv, "l:n:g:D:L:0:H:G:Q:N:T:B:F:C:K:R:X:Pc:r:d:a:s:p:vh")) != -1) {
    switch (o) {
    case 'l': listen_spec = optarg; break;
    case 'n': {
      char b[32], *sl;
      snprintf(b, sizeof b, "%s", optarg);
      if ((sl = strchr(b, '/')) != NULL) {
        *sl = 0;
        prefix = atoi(sl + 1);
      }
      lan_cfg.net = ip_arg(b);
      break;
    }
    case 'g': lan_cfg.gw = ip_arg(optarg); break;
    case 'D': lan_cfg.dns = ip_arg(optarg); break;
    case 'L':
      if (!strcmp(optarg, "none")) {
        nofwd = 1;
        break;
      }
      if (lan_cfg.nfwd < 16) {
        const char *s = optarg;
        int udp = *s == 'u';
        if (udp) s++;
        int hp = atoi(s), zp = strchr(s, ':') ? atoi(strchr(s, ':') + 1) : hp;
        lan_cfg.fwd[lan_cfg.nfwd].udp = udp;
        lan_cfg.fwd[lan_cfg.nfwd].hport = hp;
        lan_cfg.fwd[lan_cfg.nfwd].zport = zp;
        lan_cfg.nfwd++;
      }
      break;
    case '0': hds0 = optarg; break;
    case 'H': {                         /* -H ID:file.hds (ID 1〜5): データの HDS (ファームの HDS1〜。SD のデータディスクの真似) */
      int id = atoi(optarg);
      const char *c = strchr(optarg, ':');
      struct stat st;
      if (id < 1 || id > 5 || !c) {
        fprintf(stderr, "picosim: -H ID:file.hds[:hold|:after-remote] (ID 1-5): %s\n", optarg);
        exit(2);
      }
      pz_hds_add(id, c + 1);
      if (stat(pz_hds_path(id), &st) < 0) {
        perror(pz_hds_path(id));
        exit(2);
      }
      tg[id].present = 1;
      tg[id].name = pz_hds_path(id);
      tg[id].sects = (uint32_t)(st.st_size / 512);
      picosim_log("id%d: %s (%u sectors)\n", id, c + 1, tg[id].sects);
      break;
    }
    case 'G': {                         /* -G ID:DIR (ID 1〜5): ゲーム用の ID。DIR の *.hds から 0xA8 LIST / SELECT で選ぶ */
      int id = atoi(optarg);
      const char *c = strchr(optarg, ':');
      if (id < 1 || id > 5 || !c) {
        fprintf(stderr, "picosim: -G ID:DIR (ID 1-5): %s\n", optarg);
        exit(2);
      }
      game_init(id, c + 1);
      break;
    }
    case 'N': nr_sec = atoi(optarg); break;
    case 'T':
      pz.bootwait = 1;
      if (sscanf(optarg, "%lf,%lf,%lf", &pz.t_wifi, &pz.t_smb, &pz.t_hds) != 3)
        usage();
      break;
    case 'B': pz.snap_file = optarg; break;
    case 'F': pz.fw_dir = optarg; pz.signature = 1; break;
    case 'C': pz.ctl_file = optarg; break;
    case 'K': pz_set_config_file(optarg); break;
    case 'P': pz_vd_page_by_lba = 1; break;
    case 'Q':                           /* -Q N: リモートドライブの顔を ID N に (bootwait.c の pz_face_id) */
      pz_face_id = atoi(optarg);
      if (pz_face_id < 1 || pz_face_id > 6) usage();
      tg[pz_face_id].present = 1;
      tg[pz_face_id].name = "remote drive face";
      tg[pz_face_id].sects = 0x80000000u / 512;
      break;
    case 'R':
      remote_add(optarg);
      break;
    case 'X':                           /* BlueSCSI のリモートドライブ (x68rd.c、bsrmt.sys の相手) */
      remote_add(optarg);
      x68rd_on = 1;
      break;
    case 'c': cache_mb = atoi(optarg); break;
    case 'r': ra_kb = atoi(optarg); break;
    case 'd': cmd_delay_us = atoi(optarg); break;
    case 'a': np_accel_scale = atof(optarg); break;
    case 's': stat_sec = atoi(optarg); break;
    case 'p':
      if (!(lan_pcap = fopen(optarg, "wb"))) {
        perror(optarg);
        exit(1);
      }
      break;
    case 'v': picosim_verbose++; break;
    default: usage();
    }
  }
  lan_cfg.mask = htonl(prefix ? 0xffffffffu << (32 - prefix) : 0);
  if (!lan_cfg.dns)                     /* DNS の口 (.3)。ゲートウェイ (.1) の 53 番も同じ (lan_slirp.c の dns_answer) */
    lan_cfg.dns = htonl((ntohl(lan_cfg.net) & ntohl(lan_cfg.mask)) | 3);
  if (!nofwd && lan_cfg.nfwd == 0) {
    lan_cfg.fwd[0].hport = lan_cfg.fwd[0].zport = 2323;   /* rcmd.x */
    lan_cfg.nfwd = 1;
  }
  if (nofwd)
    lan_cfg.nfwd = 0;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGUSR1, on_sig);
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  ibuf = malloc(PS_DATA_MAX);
  obuf = malloc(PS_DATA_MAX);

  tg[6].present = 1;
  tg[6].name = "netpipe";
  tg[6].sects = 0x80000000u / 512;      /* ファームの diskinfo[6].size */
  if (nr_sec && !pz.bootwait) {         /* -N n = -T 0,0,n */
    pz.bootwait = 1;
    pz.t_hds = nr_sec;
  }
  if (hds0) {
    struct stat st;
    if (stat(hds0, &st) < 0) {
      perror(hds0);
      exit(1);
    }
    tg[0].present = 1;
    tg[0].name = hds0;
    tg[0].sects = (uint32_t)(st.st_size / 512);
    pz.hds_path = hds0;
    if (pz.bootwait && !pz.snap_file) {
      static char sf[1024];
      snprintf(sf, sizeof sf, "%s.bootsnap", hds0);
      pz.snap_file = sf;
    }
    picosim_log("id0: %s (%u sectors)\n", hds0, tg[0].sects);
  }
  c_ra_pages = ra_kb / 4;
  cache_init(cache_mb);
  picosim_log("Z read cache: %d MB, readahead %d KB\n", cache_mb, cache_mb ? ra_kb : 0);
  if (lan_init() < 0) {
    fprintf(stderr, "picosim: slirp_new failed\n");
    return 1;
  }
  np_init();
  if (pzl_on) {
    if (pzlh_init(pzl_cfg, pzl_root, pzl_flash, pz.t_wifi, pz.t_smb, pz.t_hds, ip_arg(pzl_ip), lan_cfg.gw) < 0)
      return 1;
    use_pzl = 1;
    lan_forward_to(ip_arg(pzl_zip));    /* -L の転送 (PZL の netpipe が Z の IP を名乗る。picosim は代理 ARP しない) */
    for (int id = 0; id < 7; id++)
      if (pzlh_present(id))
        picosim_log("pzl: SCSI ID %d (%u sectors so far)\n", id, pzlh_sects(id));
  } else
    pz_init();
  lfd = listen_on(listen_spec);
  for (int i = 0; i < MAXCLI; i++)
    cli[i] = -1;

  uint64_t next_stat = stat_sec ? picosim_now_us() + (uint64_t)stat_sec * 1000000 : 0;
  while (!want_quit) {
    struct pollfd pfd[256];
    int n = 0, base;
    uint32_t timeout = 50;
    pfd[n].fd = lfd;
    pfd[n++].events = POLLIN;
    for (int i = 0; i < MAXCLI; i++)
      if (cli[i] >= 0) {
        pfd[n].fd = cli[i];
        pfd[n++].events = POLLIN;
      }
    base = n;
    n += lan_pollfds(pfd + base, 256 - base, &timeout);
    if (poll(pfd, n, (int)timeout) < 0 && errno != EINTR)
      break;
    lan_pollfds_done(pfd + base, n - base);
    if (pfd[0].revents & POLLIN) {
      int fd = accept(lfd, NULL, NULL), one = 1, k;
      if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        for (k = 0; k < MAXCLI && cli[k] >= 0; k++)
          ;
        if (k < MAXCLI) {
          cli[k] = fd;
          picosim_log("bridge: XEiJ connected\n");
        } else
          close(fd);
      }
    }
    for (int j = 1; j < base; j++)
      if (pfd[j].revents & (POLLIN | POLLHUP | POLLERR))
        for (int i = 0; i < MAXCLI; i++)
          if (cli[i] == pfd[j].fd && serve_one(cli[i]) < 0) {
            picosim_log("bridge: XEiJ disconnected\n");
            close(cli[i]);
            cli[i] = -1;
          }
    np_poll();
    if (use_pzl)
      pzlh_poll((double)picosim_now_us() / 1e6);
    else
      pz_tick();
    if (want_stats || (next_stat && picosim_now_us() >= next_stat)) {
      want_stats = 0;
      if (next_stat)
        next_stat += (uint64_t)stat_sec * 1000000;
      stats();
    }
  }
  stats();
  return 0;
}
