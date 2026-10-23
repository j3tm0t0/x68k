/*
 * pzl_host.c -- Pico Z Loader (x68k-zos の branch pzl、pico/) のホスト用の下回り。picosim --pzl から使う
 *
 * PZL のファームのうち、SCSI の割り当て (src/scsi)・記憶装置 (src/storage)・設定 (src/config)・設定画面のコマンド
 * (src/remote/vd_command.c)・リモートドライブ (x68kserremote の remoteserv.c)・netpipe (src/netpipe)・
 * 暗号アクセラレータ (src/accel)・起動待ち (src/bootwait) は PZL のソースをそのままコンパイルする
 * (pzl/Makefile がコミットから写し出す)。ここはそれ以外の、Pico でしか動かない所の代わり:
 *   - main.c / net.c の大域変数と、net.c の接続の状態機械 (時間で sysstatus を進め、net.c と同じ順にリモートドライブの
 *     根と HDS を開く)
 *   - SMB: HDS (stor_smb.c の hds_smb) とリモートドライブ (remoteserv.c の fileop) は、"share/path" を手元の
 *     ディレクトリ (pzlh_init の root) の下として。リモートドライブは x68kserremote の POSIX の fileop.h を使う
 *     (Pico の src/remote/fileop.h は libsmb2)。設定画面の SMB の一覧 (SMB2_ENUM / LIST) は失敗を返す
 *   - Wi-Fi (cyw43): lwIP の netif 1 つを picosim の LAN (libslirp) の 1 台にする (IP は既定 192.168.1.118)。
 *     netpipe.c はこれを Wi-Fi の netif として横取りする (Z の IP の代理 ARP とブリッジ)
 *   - core 1 (accel.c の計算): スレッド。フラッシュ (設定 0x1F0000、起動待ちの控え 0x1E0000): 2MB の配列
 *     (pzlh_init の flash ファイルがあれば読み、書いたら書き戻す)
 * picosim からは pzlh_* だけが見える (Makefile が ld -r で 1 個の .o にまとめ、名前を隠す。lwIP と libslirp の
 * 名前がぶつからない)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/dns.h"
#include "netif/ethernet.h"
#include "lwip/sockets.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "libsmb2-dcerpc-srvsvc.h"
#include "main.h"
#include "virtual_disk.h"
#include "config_file.h"
#include "storage.h"
#include "smb2connect.h"
#include "bootwait.h"
#include "accel.h"

/* picosim (外) のもの */
uint64_t picosim_now_us(void);
void picosim_log(const char *fmt, ...);
typedef void (*lan_rx_fn)(const uint8_t *frame, int len, void *arg);
int lan_attach(const uint8_t mac[6], uint32_t ip_be, lan_rx_fn rx, void *arg);
void lan_send(const uint8_t *frame, int len);

/* ---- main.c / net.c の大域変数 ---- */
char log_txt[LOGSIZE];
volatile int sysstatus;
uint64_t boottime;
const char *rootpath[8];

/* ---- 時計・core 1 ---- */
uint64_t pzlh_now_us(void) { return picosim_now_us(); }
void sleep_ms(uint32_t ms) { usleep(ms * 1000); }

static void *core1(void *arg)
{
  ((void (*)(void))arg)();
  return NULL;
}

void multicore_launch_core1(void (*entry)(void))
{
  pthread_t t;
  pthread_create(&t, NULL, core1, (void *)entry);
  pthread_detach(t);
}

/* ---- フラッシュ (2MB) ---- */
uint8_t pzlh_flash[2 * 1024 * 1024];
static const char *flash_file;

static void flash_save(void)
{
  FILE *f;
  if (flash_file && (f = fopen(flash_file, "wb"))) {
    fwrite(pzlh_flash, 1, sizeof pzlh_flash, f);
    fclose(f);
  }
}

void pzl_host_flash_erase(uint32_t off, size_t len)
{
  if (off + len <= sizeof pzlh_flash)
    memset(pzlh_flash + off, 0xff, len);
  flash_save();
}

void pzl_host_flash_program(uint32_t off, const uint8_t *data, size_t len)
{
  if (off + len <= sizeof pzlh_flash)
    for (size_t i = 0; i < len; i++)
      pzlh_flash[off + i] &= data[i];
  flash_save();
}

/* bootwait_pico.c の代わり */
void bw_flash_erase(uint32_t off, uint32_t len) { pzl_host_flash_erase(BW_FLASH_OFFSET + off, len); }
void bw_flash_program(uint32_t off, const uint8_t *data, uint32_t len) { pzl_host_flash_program(BW_FLASH_OFFSET + off, data, len); }
const uint8_t *bw_flash_ptr(void) { return pzlh_flash + BW_FLASH_OFFSET; }

static int rebooted;
void pzl_host_reboot(void) { rebooted = 1; }

/* ---- SMB の代わり ---- */
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

/* smb2connect.c と、vd_command.c が使う libsmb2 / ソケット (設定画面の SMB の一覧) は、ホストでは失敗 */
struct smb2_context *connect_smb2(const char *share) { (void)share; return NULL; }
void disconnect_smb2(struct smb2_context *smb2) { (void)smb2; }
struct smb2_context *connect_smb2_path(const char *path, const char **shpath) { (void)path; (void)shpath; return NULL; }
void disconnect_smb2_all(void) {}
int pzl_poll(struct pollfd *fds_, int nfds, int timeout) { (void)fds_; (void)nfds; (void)timeout; return -1; }
const char *smb2_get_error(struct smb2_context *smb2) { (void)smb2; return "no SMB on the host (picosim)"; }
int smb2_get_fd(struct smb2_context *smb2) { (void)smb2; return -1; }
int smb2_which_events(struct smb2_context *smb2) { (void)smb2; return 0; }
int smb2_service(struct smb2_context *smb2, int revents) { (void)smb2; (void)revents; return -1; }
void smb2_free_data(struct smb2_context *smb2, void *ptr) { (void)smb2; (void)ptr; }
int smb2_share_enum_async(struct smb2_context *smb2, enum SHARE_INFO_enum level, smb2_command_cb cb, void *cb_data)
{ (void)smb2; (void)level; (void)cb; (void)cb_data; return -1; }
struct smb2dir *smb2_opendir(struct smb2_context *smb2, const char *path) { (void)smb2; (void)path; return NULL; }
struct smb2dirent *smb2_readdir(struct smb2_context *smb2, struct smb2dir *dir) { (void)smb2; (void)dir; return NULL; }
void smb2_closedir(struct smb2_context *smb2, struct smb2dir *dir) { (void)smb2; (void)dir; }
/* REMOTE_BOOT の HUMAN.SYS (virtual_disk.c の human_read) も SMB なので、ホストでは読めない (0 を返す) */
struct smb2_context *path2smb2(const char *path, const char **shpath) { (void)path; (void)shpath; return NULL; }
struct smb2fh *smb2_open(struct smb2_context *smb2, const char *path, int flags) { (void)smb2; (void)path; (void)flags; return NULL; }
int smb2_close(struct smb2_context *smb2, struct smb2fh *fh) { (void)smb2; (void)fh; return 0; }
int smb2_read(struct smb2_context *smb2, struct smb2fh *fh, uint8_t *buf, uint32_t count)
{ (void)smb2; (void)fh; (void)buf; (void)count; return -1; }
int64_t smb2_lseek(struct smb2_context *smb2, struct smb2fh *fh, int64_t offset, int whence, uint64_t *cur)
{ (void)smb2; (void)fh; (void)offset; (void)whence; (void)cur; return -1; }

/* ---- Wi-Fi の代わり: lwIP の netif を picosim の LAN に ---- */
cyw43_t cyw43_state;
static struct netif *wifi = &cyw43_state.netif[CYW43_ITF_STA];
static const uint8_t pico_mac[6] = { 0x28, 0xcd, 0xc1, 0x00, 0x01, 0x18 };
static uint32_t pico_ip, gw_ip;
static int lan_up;
/* LAN から届いたフレームは溜めておき、poll で lwIP に渡す (lwIP の送信の途中で受信が入れ子にならないように) */
#define RXQ 64
static struct { int len; uint8_t f[1514]; } rxq[RXQ];
static int rxh, rxt;

u32_t sys_now(void) { return (u32_t)(picosim_now_us() / 1000); }
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t p) { (void)p; }
u32_t lwip_port_rand(void) { return (u32_t)random(); }

static void lan_rx(const uint8_t *f, int len, void *arg)
{
  (void)arg;
  if (len > 1514 || rxh - rxt >= RXQ)
    return;
  rxq[rxh % RXQ].len = len;
  memcpy(rxq[rxh % RXQ].f, f, len);
  rxh++;
}

static err_t linkout(struct netif *nif, struct pbuf *p)
{
  uint8_t f[1600];
  (void)nif;
  if (p->tot_len > sizeof f)
    return ERR_IF;
  pbuf_copy_partial(p, f, p->tot_len, 0);
  lan_send(f, p->tot_len);
  return ERR_OK;
}

static err_t nif_init(struct netif *nif)
{
  nif->name[0] = 'w';
  nif->name[1] = '0';
  nif->mtu = 1500;
  nif->hwaddr_len = 6;
  memcpy(nif->hwaddr, pico_mac, 6);
  nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
  nif->output = etharp_output;
  nif->linkoutput = linkout;
  return ERR_OK;
}

static void wifi_up(void)
{
  ip4_addr_t ip, mask, gw;
  ip.addr = pico_ip;
  gw.addr = gw_ip;
  IP4_ADDR(&mask, 255, 255, 255, 0);
  netif_add(wifi, &ip, &mask, &gw, NULL, nif_init, ethernet_input);
  netif_set_default(wifi);
  netif_set_up(wifi);
  netif_set_link_up(wifi);
  /* DHCP の代わり: DNS はゲートウェイ (picosim が答える) */
  ip_addr_t d;
  ip_addr_copy_from_ip4(d, gw);
  dns_setserver(0, &d);
  lan_attach(pico_mac, pico_ip, lan_rx, NULL);
  lan_up = 1;
  picosim_log("pzl: Wi-Fi (lwIP) %s on the LAN\n", ip4addr_ntoa(&ip));
}

void pzlh_net_poll(void)
{
  while (rxt < rxh) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, rxq[rxt % RXQ].len, PBUF_POOL);
    if (p) {
      pbuf_take(p, rxq[rxt % RXQ].f, rxq[rxt % RXQ].len);
      if (wifi->input(p, wifi) != ERR_OK)
        pbuf_free(p);
    }
    rxt++;
  }
  sys_check_timeouts();
}

/* ---- net.c の代わり ---- */
static double t_wifi, t_smb, t_hds;
static int opened;

static int bw_read0(void *ctx, uint32_t lba, uint8_t *buf)
{
  (void)ctx;
  return hds_read(0, lba, buf) < 0 ? -1 : 0;
}

uint32_t net_retry_ms(void) { return opened ? 0xffffffffu : 0; }
/* net.c の: HDS を開こうとしている最中か / SMB の読み書きの失敗 (繋ぎ直し) / SMB を使っている最中か。ホストでは SMB が
   切れないので、開く前は pending、失敗は記録だけ */
volatile bool smb_busy;
bool net_hds_pending(void) { return !opened; }
void net_io_failed(void) { picosim_log("pzl: net_io_failed (the Pico would reconnect SMB)\n"); }
const char *net_hostname(void) { return "pico-host"; }
void net_request(int mode) { (void)mode; }

/* net.c の net_open_storage と同じ順 (リモートドライブの根 REMOTE0〜 → HDS0〜、HDS0 は起動待ちの控えも → diskinfo)。
   net_open_storage そのものは net.c (cyw43 と libsmb2 の状態機械と同じファイル) にあるので、ホストでは同じ手順を写している */
static void open_storage(void)
{
  static char paths[8][1024];
  int remoteunit = atoi(config.remoteunit);
  for (int i = 0; i < remoteunit && i < (int)countof(config.remote); i++) {
    struct stat st;
    snprintf(paths[i], sizeof paths[i], "%s/%s", smb_root, config.remote[i]);
    if (rootpath[i] || stat(paths[i], &st) < 0 || !S_ISDIR(st.st_mode)) {
      if (!rootpath[i])
        printf("%s is not directory.\n", config.remote[i]);
      continue;
    }
    rootpath[i] = paths[i];
    printf("REMOTE%u: %s\n", i, config.remote[i]);
  }
  int id = atoi(config.remoteboot) ? 1 : 0;
  for (int i = 0; i < (int)countof(config.hds); i++, id++) {
    if (strlen(config.hds[i]) == 0 || hds_isopen(id))
      continue;
    if (hds_open(id, config.hds[i]) == 0) {
      diskinfo[id].size = hds_size(id);
      if (id == 0)
        bw_check(config.hds[i], hds_size(0), bw_read0, NULL);
    }
  }
  for (int i = 0; i < 7; i++)
    diskinfo[i].sects = (diskinfo[i].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  sysstatus = STAT_CONFIGURED;
  vd_mounted();
}

void pzlh_poll(double t)
{
  pzlh_net_poll();
  accel_poll();
  if (opened)
    return;
  if (t < t_wifi) {
    sysstatus = STAT_WIFI_CONNECTING;
  } else if (t < t_smb) {
    if (!lan_up)
      wifi_up();
    sysstatus = STAT_SMB2_CONNECTING;
  } else if (t < t_hds) {
    if (!lan_up)
      wifi_up();
    sysstatus = STAT_SMB2_CONNECTED;
  } else {
    if (!lan_up)
      wifi_up();
    opened = 1;
    /* SMB のサーバの時計の代わりに Mac の時計 (GETINFO の時刻と、リモートドライブの数を返すかどうかが boottime で決まる) */
    boottime = (uint64_t)time(NULL) * 1000000 - picosim_now_us();
    open_storage();
  }
}

/* cfg: config.txt の形 (PZL の config_parse。NULL ならフラッシュの設定)。root: SMB の共有の代わりのディレクトリ。
   flash: フラッシュを残すファイル (NULL なら残さない)。tw/ts/th: Wi-Fi・SMB・HDS が済む時刻 (秒)。
   ip / gw: Pico の IP と既定の経路 (ネットワークバイトオーダー) */
int pzlh_init(const char *cfg, const char *root, const char *flash, double tw, double ts, double th, uint32_t ip, uint32_t gw)
{
  static char buf[4096];
  FILE *f;
  size_t n;
  memset(pzlh_flash, 0xff, sizeof pzlh_flash);
  flash_file = flash;
  if (flash && (f = fopen(flash, "rb"))) {
    if (fread(pzlh_flash, 1, sizeof pzlh_flash, f) != sizeof pzlh_flash)
      memset(pzlh_flash, 0xff, sizeof pzlh_flash);
    fclose(f);
  }
  config_read();                        /* フラッシュの設定 (無ければ既定値: TZ・TADJUST など。Pico と同じ) */
  if (cfg) {
    /* config.txt の形で上書きする (Mac から config.txt を書いたのと同じ) */
    if (!(f = fopen(cfg, "r"))) {
      perror(cfg);
      return -1;
    }
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    config_parse((uint8_t *)buf);
  }
  smb_root = root;
  t_wifi = tw;
  t_smb = ts;
  t_hds = th;
  pico_ip = ip;
  gw_ip = gw;
  lwip_init();
  vd_init();
  accel_init();
  pzlh_poll(0);
  return 0;
}

/* ---- picosim からの SCSI (Z の Linux が pscsi.ini 越しにするのと同じ番地) ---- */
#define PZLH_OK        0
#define PZLH_NOTREADY  1
#define PZLH_IOERR     2

static int rc(int r)
{
  return r == VD_NOTREADY ? PZLH_NOTREADY : r == VD_IOERR ? PZLH_IOERR : PZLH_OK;   /* -1 は成功扱い (msc_disk.c) */
}

int pzlh_read(int id, uint32_t lba, uint8_t *buf)
{
  return rc(vd_read_block(0x00803fa0 + (uint32_t)id * 0x800000 + lba, buf));
}

int pzlh_write(int id, uint32_t lba, const uint8_t *buf)
{
  uint8_t b[512];
  memcpy(b, buf, 512);
  return rc(vd_write_block(0x00803fa0 + (uint32_t)id * 0x800000 + lba, b));
}

int pzlh_present(int id) { return id >= 0 && id < 7 && diskinfo[id].type != DTYPE_NOTUSED; }
uint32_t pzlh_sects(int id) { return pzlh_present(id) ? (uint32_t)diskinfo[id].sects : 0; }
int pzlh_sysstatus(void) { return sysstatus; }
int pzlh_rebooted(void) { int r = rebooted; rebooted = 0; return r; }

/* TEST UNIT READY: HDS が開くまで NOT READY (vd_read_block の HDS と同じ決まり。控えを返す間は読める) */
int pzlh_ready(int id)
{
  uint8_t b[512];
  if (!pzlh_present(id))
    return PZLH_IOERR;
  return diskinfo[id].type == DTYPE_HDS ? rc(vd_read_block(0x00803fa0 + (uint32_t)id * 0x800000, b)) : PZLH_OK;
}
