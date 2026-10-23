/*
 * bootwait.c -- picosim の「Pico Z Loader」の部分: ID 0 (起動 HDS) の準備待ちと、ID 6 の netpipe 以外の帯
 *
 * x68kzremotedrv フォーク (yunkya2/x68kzremotedrv の j3tm0t0 版、branch bootwait 8f91624) の src/bootwait.c・
 * src/virtual_disk.c・src/vd_command.c と同じ決まりで答える。Z の上で動く IPL や設定画面の 68000 のバイナリ
 * (driver/bootwait1.bin・bootwait.bin・hdsboot.bin・settingui.bin) は持たず、-F のディレクトリから起動時に読む
 * (無いものはその機能を切る)。
 *
 * 状態: Pico の起動 (= picosim の起動か "powercycle") からの時間で sysstatus を進める (-T wifi,smb,hds 秒)。
 *   0 〜 wifi: 1 (Wi-Fi に接続中) / 〜 smb: 3 (SMB に接続中、Wi-Fi は済み) / 〜 hds: 4 (SMB 済み) / hds 〜: 5 で HDS が開く
 * 控え (フラッシュ 0x1E0000 の代わり、-B のファイル): 見出し 4KB ("X6Z BootSnap v1"、FNV-1a、HDS の大きさ、パス) +
 *   HDS の LBA 0-31 (16KB)。HDS が開いたら比べ、違えば書き直す。
 * ID 0 (HDS が開くまで): 控えがこの HDS のものなら LBA 0-31 を控えから (LBA 2 は待ち IPL の前半 bootwait1、3 は 0)。
 *   それ以外は NOT READY (02/04/01)。sysstatus が 5 なのに開けない (HDS が無い) なら MEDIUM ERROR (03/11/00)
 * ID 0 (開いたあと): HDS そのまま。ただし hdsboot があれば LBA 2 は hdsboot (F1 で設定画面)、LBA 0x20/0x21 は本物の 2/3
 * ID 6: LBA 0 = "X68SCSI1" + "X68000ZRemoteDrv"、LBA 4 = "X68K" (リモートドライブ 0 個)、0x80〜 = 待ち IPL の後半、
 *   0x100〜 = settingui、0x40000〜0xfffff = 待ち IPL の問い合わせ (bw_reply)、0x100000〜 = netpipe (netpipe.c)、
 *   それ以外の 0x200〜 = 設定画面との vdbuf ("X68Z" の書き込みに vd_command で答え、読み込みで返す)
 * 制御: -C のファイルに 1 行 1 命令を書くと実行して消す: ready / close / status N / powercycle / hds PATH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "picosim.h"

/* ---- driver/ と src/bootwait_proto.h の取り決め ---- */
#define BW_BAND         0x40000
#define BW_BAND_END     0x100000
#define BW_STRIDE       64
#define BW_WAIT         0
#define BW_READY        1
#define BW_CHANGED      2
#define BW_NOHDS        3
#define BW_STAGE2       0x80
#define BW_STAGE2_SECTS 32
#define BW_SNAP_SECTS   32
#define SNAP_MAGIC      "X6Z BootSnap v1"
#define SNAP_DATA       4096
#define SNAP_SIZE       (SNAP_DATA + BW_SNAP_SECTS * 512)
#define SETTINGUI_LBA   (0x20000 / 512)
#define SETTINGUI_END   (0x40000 / 512)

struct pz pz;                           /* 外 (picosim.c) から見る状態 */

/* B: (remote.c + x68kserremote の remoteserv.c)。リンクしていなければこの弱い定義 (B: 無し) が使われる */
__attribute__((weak)) int remote_units;
__attribute__((weak)) int remote_request(uint8_t *c, uint8_t *r) { (void)c; (void)r; return -1; }
__attribute__((weak)) void remote_add(const char *dir)
{
  fprintf(stderr, "picosim: built without the remote drive (make RS=.../x68kserremote); -R %s ignored\n", dir);
}
static int nunits(void) { return remote_units; }

static uint8_t *bw1, *bw2, *hdsboot, *setui, *scsirem;   /* 68000 のバイナリ (-F から) */
static long bw1_len, bw2_len, hdsboot_len, setui_len, scsirem_len;
static uint8_t snap[SNAP_SIZE];         /* 控え (ファイルの写し) */
static int snap_ok = -1;                /* 壊れていないか (-1: まだ) */
static int served, checked, match;
static uint32_t ipl_sum;
static uint64_t boot_us;                /* Pico の起動 */
static int forced = -1;                 /* 制御で決めた sysstatus (-1: 時間で) */

/* ---- 設定 (config.txt / フラッシュ 0x1F0000 の代わり。-K のファイルに struct config_data のまま) ---- */
static struct config_data {
  char wifi_ssid[32], wifi_passwd[16], smb2_user[16], smb2_passwd[16], smb2_workgroup[16], smb2_server[32];
  char remoteboot[4], remoteunit[4], remote[8][128], hds[4][128], tz[16], tadjust[4], fastconnect[4];
} config;
static const char *config_file;

static uint8_t *load(const char *dir, const char *name, long *len)
{
  char p[1024];
  snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*len + 512);
  memset(b, 0, *len + 512);
  if (fread(b, 1, *len, f) != (size_t)*len) {
    free(b);
    b = NULL;
  }
  fclose(f);
  return b;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, int n)
{
  while (n-- > 0)
    h = (h ^ *p++) * 16777619u;
  return h;
}

static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
/* 控えの見出し: magic[16] hash size_lo size_hi pad path[128] (Pico のリトルエンディアンのまま) */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void ple32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int usable(void)
{
  if (snap_ok < 0)
    snap_ok = memcmp(snap, SNAP_MAGIC, sizeof SNAP_MAGIC) == 0 &&
              le32(snap + 16) == fnv(2166136261u, snap + SNAP_DATA, BW_SNAP_SECTS * 512);
  return snap_ok && pz.hds_path && strncmp((const char *)snap + 32, pz.hds_path, 128) == 0;
}

int pz_sysstatus(void)
{
  if (forced >= 0)
    return forced;
  if (!pz.bootwait)
    return 5;
  double t = (picosim_now_us() - boot_us) / 1e6;
  return t < pz.t_wifi ? 1 : t < pz.t_smb ? 3 : t < pz.t_hds ? 4 : 5;
}

/* HDS を開く (connect.c の vd_mount + bw_check) */
static void hds_open(void)
{
  uint8_t buf[512];
  struct stat st;
  if (pz.hds_fd >= 0 || !pz.hds_path)
    return;
  pz.hds_fd = open(pz.hds_path, O_RDWR);
  if (pz.hds_fd < 0 || fstat(pz.hds_fd, &st) < 0) {
    picosim_log("pz: cannot open %s\n", pz.hds_path);
    pz.hds_fd = -1;
    return;
  }
  pz.hds_sects = (uint32_t)(st.st_size / 512);
  if (!pz.bootwait || !pz.snap_file) {
    picosim_log("pz: HDS %s open\n", pz.hds_path);
    return;
  }
  int same = usable() && le32(snap + 20) == (uint32_t)st.st_size && le32(snap + 24) == (uint32_t)((uint64_t)st.st_size >> 32);
  for (int i = 0; i < BW_SNAP_SECTS; i++) {
    if (pread(pz.hds_fd, buf, 512, (off_t)i * 512) != 512)
      memset(buf, 0, 512);
    if (same && memcmp(buf, snap + SNAP_DATA + i * 512, 512) != 0)
      same = 0;
  }
  match = same;
  checked = 1;
  if (same) {
    picosim_log("pz: HDS %s open, snapshot matches\n", pz.hds_path);
    return;
  }
  /* 控え直す (見出しを最後に: 途中で止まっても壊れた控えは使われない。ファイルは丸ごと書き直す) */
  uint32_t h = 2166136261u;
  memset(snap, 0xff, SNAP_DATA);
  for (int i = 0; i < BW_SNAP_SECTS; i++) {
    if (pread(pz.hds_fd, snap + SNAP_DATA + i * 512, 512, (off_t)i * 512) != 512)
      memset(snap + SNAP_DATA + i * 512, 0, 512);
    h = fnv(h, snap + SNAP_DATA + i * 512, 512);
  }
  memset(snap, 0, 256);
  memcpy(snap, SNAP_MAGIC, sizeof SNAP_MAGIC);
  ple32(snap + 16, h);
  ple32(snap + 20, (uint32_t)st.st_size);
  ple32(snap + 24, (uint32_t)((uint64_t)st.st_size >> 32));
  strncpy((char *)snap + 32, pz.hds_path, 127);
  snap_ok = -1;
  FILE *f = fopen(pz.snap_file, "wb");
  if (f) {
    fwrite(snap, 1, SNAP_SIZE, f);
    fclose(f);
  }
  picosim_log("pz: HDS %s open, snapshot %s written (%s)\n", pz.hds_path, pz.snap_file,
              served ? "CHANGED: the Z already has the old one" : "updated");
}

static void hds_close(void)
{
  if (pz.hds_fd >= 0)
    close(pz.hds_fd);
  pz.hds_fd = -1;
  checked = match = 0;
}

static void control(void)
{
  char line[256];
  FILE *f;
  if (!pz.ctl_file || !(f = fopen(pz.ctl_file, "r")))
    return;
  unlink(pz.ctl_file);
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    picosim_log("pz: control \"%s\"\n", line);
    if (!strcmp(line, "ready"))
      forced = 5;
    else if (!strcmp(line, "close")) {
      hds_close();
      forced = 4;
    } else if (!strncmp(line, "status ", 7))
      forced = atoi(line + 7);
    else if (!strcmp(line, "auto"))
      forced = -1;
    else if (!strcmp(line, "powercycle")) {
      /* Z と Pico の電源を入れ直した: Z のキャッシュは消え、Pico は最初から */
      hds_close();
      served = 0;
      forced = -1;
      boot_us = picosim_now_us();
      picosim_cache_clear();
    } else if (!strncmp(line, "hds ", 4)) {
      hds_close();
      pz.hds_path = strdup(line + 4);
      strncpy(config.hds[0], pz.hds_path, 127);
    }
  }
  fclose(f);
}

void pz_tick(void)
{
  control();
  if (pz_sysstatus() >= 5 && pz.hds_fd < 0)
    hds_open();
}

void pz_init(void)
{
  boot_us = picosim_now_us();
  pz.hds_fd = -1;
  if (pz.fw_dir) {
    bw1 = load(pz.fw_dir, "bootwait1.bin", &bw1_len);
    bw2 = load(pz.fw_dir, "bootwait.bin", &bw2_len);
    hdsboot = load(pz.fw_dir, "hdsboot.bin", &hdsboot_len);
    setui = load(pz.fw_dir, "settingui.bin", &setui_len);
    scsirem = load(pz.fw_dir, "scsiremote.bin", &scsirem_len);
    picosim_log("pz: firmware pieces from %s: bootwait1 %ld, bootwait %ld, hdsboot %ld, settingui %ld, scsiremote %ld bytes\n",
                pz.fw_dir, bw1 ? bw1_len : -1, bw2 ? bw2_len : -1, hdsboot ? hdsboot_len : -1, setui ? setui_len : -1,
                scsirem ? scsirem_len : -1);
    if (bw1_len > 512 || bw2_len > BW_STAGE2_SECTS * 512)
      bw1 = bw2 = NULL;
  }
  if (pz.snap_file) {
    FILE *f = fopen(pz.snap_file, "rb");
    if (f) {
      if (fread(snap, 1, SNAP_SIZE, f) != SNAP_SIZE)
        memset(snap, 0, SNAP_SIZE);
      fclose(f);
    }
  }
  if (pz.hds_path)
    strncpy(config.hds[0], pz.hds_path, 127);
  strcpy(config.wifi_ssid, "picosim");
  strcpy(config.smb2_server, "192.168.1.1");
  if (config_file) {
    FILE *f = fopen(config_file, "rb");
    if (f) {
      if (fread(&config, 1, sizeof config, f) != sizeof config)
        picosim_log("pz: short config %s\n", config_file);
      fclose(f);
    }
  }
  if (pz.bootwait)
    picosim_log("pz: bootwait: Wi-Fi %.1f s, SMB %.1f s, HDS %.1f s; snapshot %s (%s)\n", pz.t_wifi, pz.t_smb, pz.t_hds,
                pz.snap_file ? pz.snap_file : "-", usable() ? "usable" : "none");
  pz_tick();
}

void pz_set_config_file(const char *path) { config_file = path; }

/* ---- ID 0 ---- */
int pz_id0_ready(void)
{
  return pz.hds_fd >= 0 || (pz.bootwait && bw1 && usable());
}

int pz_id0_read(uint32_t lba, uint8_t *buf)
{
  if (pz.hds_fd < 0) {
    if (pz.bootwait && bw1 && lba < BW_SNAP_SECTS && usable()) {
      if (!served)
        picosim_log("pz: serving the snapshot (waiting IPL)\n");
      served = 1;
      if (lba == 2 || lba == 3) {
        memset(buf, 0, 512);
        if (lba == 2)
          memcpy(buf, bw1, bw1_len);
      } else
        memcpy(buf, snap + SNAP_DATA + lba * 512, 512);
      return PS_OK;
    }
    return pz_sysstatus() >= 5 ? PS_IOERR : PS_NOTREADY;
  }
  if (hdsboot && lba == 2) {
    memset(buf, 0, 512);
    memcpy(buf, hdsboot, hdsboot_len > 512 ? 512 : hdsboot_len);
    return PS_OK;
  }
  if (hdsboot && (lba == 0x20 || lba == 0x21))
    lba -= 0x20 - 2;
  if (pread(pz.hds_fd, buf, 512, (off_t)lba * 512) != 512)
    memset(buf, 0, 512);
  return PS_OK;
}

int pz_id0_write(uint32_t lba, const uint8_t *buf)
{
  if (pz.hds_fd < 0)
    return pz_sysstatus() >= 5 ? PS_IOERR : PS_NOTREADY;
  return pwrite(pz.hds_fd, buf, 512, (off_t)lba * 512) == 512 ? PS_OK : PS_IOERR;
}

/* ---- 待ち IPL の問い合わせ (bootwait.c の bw_reply) ---- */
static int ipl_sector(int k, uint8_t *buf)
{
  if (match) {
    memcpy(buf, snap + SNAP_DATA + (2 + k) * 512, 512);
    return 0;
  }
  return pz.hds_fd >= 0 && pread(pz.hds_fd, buf, 512, (off_t)(2 + k) * 512) == 512 ? 0 : -1;
}

static void bw_reply(uint32_t lba, uint8_t *buf)
{
  uint32_t j = (lba - BW_BAND) % BW_STRIDE;
  int sys = pz_sysstatus(), st;
  if (pz.hds_fd < 0)
    st = sys >= 5 ? BW_NOHDS : BW_WAIT;
  else if (!checked)
    st = BW_WAIT;
  else if (served && !match)
    st = BW_CHANGED;
  else
    st = BW_READY;
  memset(buf, 0, 512);
  if (j == 0) {
    if (st == BW_READY) {
      uint32_t sum = 0;
      for (int k = 0; k < 2; k++) {
        if (ipl_sector(k, buf) != 0) {
          st = BW_WAIT;
          break;
        }
        for (int i = 0; i < 512; i += 4)
          sum += g32(buf + i);
      }
      ipl_sum = sum;
      memset(buf, 0, 512);
    }
    memcpy(buf, "X6BW", 4);
    buf[4] = st;
    buf[5] = sys;
    p32(buf + 8, sys >= 2 ? 0xc0a80176 : 0);   /* Pico の IP (192.168.1.118 ということにする) */
    p32(buf + 12, (uint32_t)((picosim_now_us() - boot_us) / 1000));
    p32(buf + 16, lba);
    p32(buf + 20, st == BW_READY ? ipl_sum : 0);
    p32(buf + 24, sys < 5 ? 0 : 0xffffffff);
    if (picosim_verbose)
      picosim_log("pz: waiter asks (lba 0x%x): state %d sysstatus %d\n", lba, st, sys);
  } else if (j <= 2 && st == BW_READY) {
    if (ipl_sector(j - 1, buf) != 0)
      memset(buf, 0, 512);
  }
}

/* ---- 設定画面との vdbuf (virtual_disk.c と vd_command.c) ---- */
#define VD_PAGE 496
static uint8_t vd_hdr[16], vd_wr[VD_PAGE * 32], vd_rd[VD_PAGE * 32];
static int vd_rpages, vd_rcnt;
static int64_t vd_base = -1;            /* -P: この応答で最初に読まれた LBA の 8 の倍数 */

static int vd_command(const uint8_t *c, uint8_t *r)
{
  int cmd = c[0] << 8 | c[1];
  if (cmd >= 0xff00)
    picosim_log("pz: settings command 0x%04x\n", cmd);
  switch (cmd) {
  case 0xff00: {                        /* GETINFO: 時刻, unit, version, verstr */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    memset(r, 0, 24);
    r[0] = (tm->tm_year + 1900) >> 8;
    r[1] = tm->tm_year + 1900;
    r[2] = tm->tm_mon + 1;
    r[3] = tm->tm_mday;
    r[4] = tm->tm_hour;
    r[5] = tm->tm_min;
    r[6] = tm->tm_sec;
    r[7] = nunits();
    r[8] = 1;
    strcpy((char *)r + 9, "picosim");
    return 26;
  }
  case 0xff01:                          /* GETCONFIG */
    memcpy(r, &config, sizeof config);
    return sizeof config;
  case 0xff02:                          /* SETCONFIG: mode, config。つなぎ直し (時間で最初から) */
    memcpy(&config, c + 3, sizeof config);
    if (config.hds[0][0] && (!pz.hds_path || strcmp(config.hds[0], pz.hds_path))) {
      hds_close();
      pz.hds_path = strdup(config.hds[0]);
    }
    boot_us = picosim_now_us();
    r[0] = 0;
    return 1;
  case 0xff03:                          /* GETSTATUS */
    r[0] = pz_sysstatus();
    return 1;
  case 0xff07:                          /* FLASHCONFIG */
    if (config_file) {
      FILE *f = fopen(config_file, "wb");
      if (f) {
        fwrite(&config, 1, sizeof config, f);
        fclose(f);
      }
    }
    r[0] = 0;
    return 1;
  case 0xff08:                          /* FLASHCLEAR */
    memset(&config, 0, sizeof config);
    r[0] = 0;
    return 1;
  case 0xff09:                          /* REBOOT: Pico だけ再起動 (Z のキャッシュは残る) */
    hds_close();
    served = 0;
    forced = -1;
    boot_us = picosim_now_us();
    r[0] = 0;
    return 1;
  case 0xff04:                          /* WIFI_SCAN: status, n, ssid[16][32] */
    memset(r, 0, 2 + 16 * 32);
    r[1] = 1;
    strcpy((char *)r + 2, "picosim");
    return 2 + 16 * 32;
  case 0xff05:                          /* SMB2_ENUM: status, n, share[16][64] */
    memset(r, 0, 2 + 16 * 64);
    r[1] = 1;
    strcpy((char *)r + 2, "picosim");
    return 2 + 16 * 64;
  case 0xff06: {                        /* SMB2_LIST: status, list (名前を 0 で区切り、最後に 0) */
    memset(r, 0, 1 + 1024);
    const char *b = pz.hds_path ? strrchr(pz.hds_path, '/') : NULL;
    strncpy((char *)r + 1, b ? b + 1 : pz.hds_path ? pz.hds_path : "", 120);
    return 1 + 1024;
  }
  default:
    /* リモートドライブ (scsiremote.sys) の要求。ファームと同じく、接続が済むまでは答えない */
    if (nunits() && pz_sysstatus() >= 5)
      return remote_request((uint8_t *)c, r);
    return -1;
  }
}

static void vd_write(const uint8_t *b)
{
  int page = b[12], maxpage = b[13];
  if (g32(b) != 0x5836385a || page >= 32)  /* "X68Z" */
    return;
  memcpy(vd_hdr, b, 16);
  memcpy(vd_wr + page * VD_PAGE, b + 16, VD_PAGE);
  if (picosim_verbose >= 3)
    picosim_log("pz: vdbuf write session %08x seq %u page %d/%d cmd %02x%02x\n", g32(b + 4), g32(b + 8), page, maxpage,
                b[16], b[17]);
  if (page == maxpage) {
    int n = vd_command(vd_wr, vd_rd);
    vd_rpages = n < 0 ? 0 : (n - 1) / VD_PAGE;
    vd_rcnt = 0;
    vd_base = -1;
  }
}

/* ファームと同じ (既定): page = vdbuf_rcnt + lba % 8、lba % 8 == 7 を読むたびに vdbuf_rcnt += 8。
 * Z の Linux の先読みが、ドライバの読まない上の 8 セクタ (前の応答が 8 ページの倍数で終わると 1 組飛ばされる) を
 * 読むと vdbuf_rcnt が余分に進み、ページ 8 以降が 8 ずれる (B: の _EXEC で再配置表が壊れて落ちた原因)。
 * -P: ページを LBA から決める。ドライバは 8 ページごとに 8 セクタ下へ進むので、この応答で最初に読まれた
 *     LBA の組 (先読みはそれより上だけを読む) からの距離で決まる */
int pz_vd_page_by_lba;

static void vd_read(uint32_t lba, uint8_t *buf)
{
  int page = vd_rcnt + (lba % 8);
  if (pz_vd_page_by_lba) {
    if (vd_base < 0)
      vd_base = lba & ~7u;
    int64_t g = (vd_base - (int64_t)(lba & ~7u)) / 8;
    page = g < 0 ? 31 : (int)(g * 8 + (lba % 8));   /* 上 (先読み) は使われないので空のページ */
  }
  memcpy(buf, vd_hdr, 16);
  buf[13] = vd_rpages;
  buf[12] = page;
  if (page < 32)
    memcpy(buf + 16, vd_rd + page * VD_PAGE, VD_PAGE);
  else
    memset(buf + 16, 0, VD_PAGE);
  if (lba % 8 == 7)
    vd_rcnt += 8;
}

/* ---- ID 6 ---- */
int pz_id6_read(uint32_t lba, uint8_t *buf)
{
  memset(buf, 0, 512);
  if (lba == 0 && pz.signature) {
    memcpy(buf, "X68SCSI1", 8);
    memcpy(buf + 16, "X68000ZRemoteDrv", 16);
  } else if (lba == 4 && pz.signature) {
    memcpy(buf, "X68K", 4);             /* 分割表: リモートドライブの数 (-R) だけ "Human68k" */
    for (int i = 0; i < nunits(); i++)
      memcpy(buf + 16 + i * 16, "Human68k", 8);
  } else if (lba >= 6 && lba < 0x20) {
    /* SCSI のデバイスドライバ (scsiremote.sys)。Human68k が ID 6 をディスクとして組み込むときに読む */
    uint32_t off = (lba - 6) * 512;
    if (scsirem && nunits() && off < (uint32_t)scsirem_len)
      memcpy(buf, scsirem + off, scsirem_len - off < 512 ? scsirem_len - off : 512);
  } else if (lba < 0x20) {
  } else if (np_read(lba, buf) == 0) {
  } else if (lba >= BW_STAGE2 && lba < BW_STAGE2 + BW_STAGE2_SECTS) {
    uint32_t off = (lba - BW_STAGE2) * 512;
    if (bw2 && off < (uint32_t)bw2_len)
      memcpy(buf, bw2 + off, bw2_len - off < 512 ? bw2_len - off : 512);
  } else if (lba >= BW_BAND && lba < BW_BAND_END) {
    if (bw2)
      bw_reply(lba, buf);
  } else if (lba >= SETTINGUI_LBA && lba < SETTINGUI_END) {
    uint32_t off = (lba - SETTINGUI_LBA) * 512;
    if (setui && off < (uint32_t)setui_len)
      memcpy(buf, setui + off, setui_len - off < 512 ? setui_len - off : 512);
  } else if (lba >= SETTINGUI_END) {
    vd_read(lba, buf);
  }
  return PS_OK;
}

int pz_id6_write(uint32_t lba, const uint8_t *buf)
{
  if (np_write(lba, buf) == 0)
    return PS_OK;
  vd_write(buf);                        /* "X68Z" でなければ捨てる (ファームと同じく成功扱い) */
  return PS_OK;
}
