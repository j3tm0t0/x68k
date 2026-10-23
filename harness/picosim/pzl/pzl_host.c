/*
 * pzl_host.c -- Pico Z Loader (x68k-zos の branch pzl、pico/) のホスト用の下回り。picosim --pzl から使う
 *
 * PZL のファームのうち、SCSI の割り当て (src/scsi/virtual_disk.c)・記憶装置の層 (src/storage/storage.c, stor_ram.c)・
 * 設定 (src/config/config_file.c) は PZL のソースをそのままコンパイルする (pzl/Makefile が pzl の作業木から写し出す)。
 * ここはそれ以外の、Pico でしか動かない所の代わり:
 *   - main.c の大域変数 (sysstatus, boottime, log_txt)
 *   - net.c (Wi-Fi・SMB の接続の状態機械) の代わりに、時間で sysstatus を進め、net.c と同じ順に HDS を開く
 *   - stor_smb.c (libsmb2) の代わりに、"share/path/file.hds" を手元のディレクトリ (pzl_init の smb_root) の下の
 *     ファイルとして読み書きする hds_smb
 *   - フラッシュ・watchdog (pzl/shim/hardware/*.h)
 * picosim からは pzl_* だけが見える (Makefile が ld -r で 1 個の .o にまとめ、名前を隠す)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "main.h"
#include "virtual_disk.h"
#include "config_file.h"
#include "storage.h"

/* ---- main.c / net.c の大域変数 ---- */
struct diskinfo diskinfo[7];
char log_txt[LOGSIZE];
uint64_t boottime;
volatile int sysstatus;

/* ---- フラッシュ・watchdog ---- */
static int rebooted;
void pzl_host_reboot(void) { rebooted = 1; }
void pzl_host_flash_erase(uint32_t off, size_t len) { (void)off; (void)len; }
void pzl_host_flash_program(uint32_t off, const uint8_t *data, size_t len) { (void)off; (void)data; (void)len; }

/* ---- hds_smb の代わり: 手元のディレクトリ ---- */
static const char *smb_root = ".";
static int fds[HDS_MAX] = { -1, -1, -1, -1, -1, -1 };

static int f_open(int id, const char *path, uint64_t *size)
{
  char p[1024];
  struct stat st;
  snprintf(p, sizeof p, "%s/%s", smb_root, path);
  fds[id] = open(p, O_RDWR);
  if (fds[id] < 0 || fstat(fds[id], &st) < 0)
    return -1;
  *size = (uint64_t)st.st_size;
  return 0;
}

static int f_read(int id, uint32_t lba, uint8_t *buf)
{
  return pread(fds[id], buf, 512, (off_t)lba * 512) == 512 ? 0 : -1;
}

static int f_write(int id, uint32_t lba, const uint8_t *buf)
{
  return pwrite(fds[id], buf, 512, (off_t)lba * 512) == 512 ? 0 : -1;
}

static int f_write_n(int id, uint32_t lba, uint32_t n, const uint8_t *buf)
{
  return pwrite(fds[id], buf, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)n * 512 ? 0 : -1;
}

static void f_close(int id)
{
  if (fds[id] >= 0)
    close(fds[id]);
  fds[id] = -1;
}

const struct hds_backend hds_smb = { "host-dir (picosim)", f_open, f_read, f_write, f_write_n, f_close };

/* ---- net.c の代わり ---- */
static double t_wifi, t_smb, t_hds;
static int opened;

static void open_hds(void)
{
  /* net.c の smb_mount の後半と同じ: REMOTE_BOOT なら ID 1 から、HDS0〜3 を開いて大きさを diskinfo に */
  int id = atoi(config.remoteboot) ? 1 : 0;
  for (int i = 0; i < (int)countof(config.hds); i++, id++) {
    if (strlen(config.hds[i]) == 0 || hds_isopen(id))
      continue;
    if (hds_open(id, config.hds[i]) == 0)
      diskinfo[id].size = hds_size(id);
  }
  for (int i = 0; i < 7; i++)
    diskinfo[i].sects = (diskinfo[i].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  sysstatus = STAT_CONFIGURED;
  vd_mounted();
}

void pzl_poll(double t)
{
  if (opened)
    return;
  if (t < t_wifi)
    sysstatus = STAT_WIFI_CONNECTING;
  else if (t < t_smb)
    sysstatus = STAT_SMB2_CONNECTING;
  else if (t < t_hds)
    sysstatus = STAT_SMB2_CONNECTED;
  else {
    opened = 1;
    open_hds();
  }
}

/* config.txt の形 (PZL の config_parse に渡す)。tw/ts/th: Wi-Fi・SMB・HDS が済む時刻 (秒) */
int pzl_init(const char *config_txt, const char *root, double tw, double ts, double th)
{
  static char buf[4096];
  FILE *f = fopen(config_txt, "r");
  size_t n;
  if (!f) {
    perror(config_txt);
    return -1;
  }
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  config_parse((uint8_t *)buf);
  smb_root = root;
  t_wifi = tw;
  t_smb = ts;
  t_hds = th;
  vd_init();
  pzl_poll(0);
  return 0;
}

/* ---- picosim からの SCSI (Z の Linux が pscsi.ini 越しにするのと同じ番地) ---- */
#define PZL_OK        0
#define PZL_NOTREADY  1
#define PZL_IOERR     2

static int rc(int r)
{
  return r == VD_NOTREADY ? PZL_NOTREADY : r == VD_IOERR ? PZL_IOERR : PZL_OK;   /* -1 は成功扱い (msc_disk.c) */
}

int pzl_read(int id, uint32_t lba, uint8_t *buf)
{
  return rc(vd_read_block(0x00803fa0 + (uint32_t)id * 0x800000 + lba, buf));
}

int pzl_write(int id, uint32_t lba, const uint8_t *buf)
{
  uint8_t b[512];
  memcpy(b, buf, 512);
  return rc(vd_write_block(0x00803fa0 + (uint32_t)id * 0x800000 + lba, b));
}

int pzl_present(int id) { return id >= 0 && id < 7 && diskinfo[id].type != DTYPE_NOTUSED; }
uint32_t pzl_sects(int id) { return pzl_present(id) ? (uint32_t)diskinfo[id].sects : 0; }
int pzl_sysstatus(void) { return sysstatus; }
int pzl_rebooted(void) { int r = rebooted; rebooted = 0; return r; }

/* TEST UNIT READY: HDS が開くまで NOT READY (vd_read_block の HDS と同じ決まり) */
int pzl_ready(int id)
{
  uint8_t b[512];
  if (!pzl_present(id))
    return PZL_IOERR;
  return diskinfo[id].type == DTYPE_HDS ? rc(vd_read_block(0x00803fa0 + (uint32_t)id * 0x800000, b)) : PZL_OK;
}
