/*
 * picosim.h -- Mac の上で X68000 Z の Pico W (x68kzremotedrv の netpipe ファーム) の代わりをする picosim の内部の口と、
 *              外から足すサービス (tistack の Pico 側ソケット窓口など) のための口
 *
 * 全体は 1 本のスレッド (poll のループ) で動く。サービスの関数もそのスレッドから呼ばれるので、ロックは要らない。
 */
#ifndef PICOSIM_H
#define PICOSIM_H
#include <stdint.h>

/* ---- 時計 ---- */
uint64_t picosim_now_us(void);          /* 起動からの µs (単調) */

/* ---- 記録 ---- */
extern int picosim_verbose;
void picosim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- SCSI の対象 (picosim.c) ---- */
/* 1 命令の結果。data は呼び出し側の領域 (最大 PS_DATA_MAX) */
#define PS_DATA_MAX     (1 << 20)
struct ps_result {
  uint8_t status;                       /* SCSI の状態 (0 = GOOD, 2 = CHECK CONDITION) */
  uint8_t key, asc, ascq;               /* CHECK CONDITION のときのセンス (次の REQUEST SENSE で返す) */
  uint32_t len;                         /* データインのバイト数 */
};

/* 1 セクタの結果 */
#define PS_OK           0
#define PS_NOTREADY     1               /* CHECK CONDITION 02/04/01 */
#define PS_IOERR        2               /* CHECK CONDITION 03/11/00 */
void picosim_cache_clear(void);         /* Z の読み込みキャッシュを捨てる (Z の電源断) */

/* ---- Pico Z Loader (bootwait.c): ID 0 の準備待ちと ID 6 の netpipe 以外の帯 ---- */
struct pz {
  const char *hds_path;                 /* -0 (config の HDS0) */
  int hds_fd;                           /* 開いていなければ -1 */
  uint32_t hds_sects;
  int bootwait;                         /* -T: 時間で sysstatus を進め、HDS が開くまで待たせる */
  double t_wifi, t_smb, t_hds;
  const char *snap_file, *fw_dir, *ctl_file;
  int signature;                        /* ID 6 の LBA 0 に "X68SCSI1"/"X68000ZRemoteDrv" (設定画面が要る) */
};
extern struct pz pz;
void pz_init(void);
void pz_tick(void);
void pz_set_config_file(const char *path);
int pz_sysstatus(void);
int pz_id0_ready(void);
int pz_id0_read(uint32_t lba, uint8_t *buf);
int pz_id0_write(uint32_t lba, const uint8_t *buf);
int pz_id6_read(uint32_t lba, uint8_t *buf);
void pz_hds_add(int id, const char *path);   /* ID 1〜5 のデータの HDS (-H、末尾 :hold / :after-remote) */
const char *pz_hds_path(int id);
void pz_remote_init_seen(void);
int pz_hds_ready(int id);
int pz_hds_read(int id, uint32_t lba, uint8_t *buf);
int pz_hds_write(int id, uint32_t lba, const uint8_t *buf);
int pz_id6_write(uint32_t lba, const uint8_t *buf);

/* ---- netpipe (netpipe.c): SCSI ID 6 の LBA を受ける ---- */
#define NP_WLBA         0x100000        /* 要求を書く (8 セクタまで) */
#define NP_RLBA         0x110000        /* 応答を読む (0x110000〜0x1ffff7 のどこでも) */
#define NP_REND         0x200000
#define NP_SECTS        8
#define NP_DATAMAX      (NP_SECTS * 512 - 16)
void np_init(void);
int np_write(uint32_t lba, const uint8_t *buf);   /* 1 セクタ。0 = 受けた、-1 = 対象外 */
int np_read(uint32_t lba, uint8_t *buf);
void np_poll(void);                     /* ループの 1 周ごと */
void np_stats(void);

/* ---- LAN (lan_slirp.c): Pico の Wi-Fi の代わり。libslirp の仮想 LAN (既定 192.168.1.0/24、ゲートウェイ .1 = Mac) ---- */
struct lan_config {
  uint32_t net, mask, gw, dns;          /* ネットワークバイトオーダー */
  int nfwd;
  struct { int udp; uint16_t hport, zport; } fwd[16];   /* Mac の 127.0.0.1:hport -> Z の zport */
};
extern struct lan_config lan_cfg;
int lan_init(void);
/* poll のループへの登録 (lan_slirp.c が picosim.c の poll 表に fd を足す) */
struct pollfd;
int lan_pollfds(struct pollfd *pfd, int max, uint32_t *timeout_ms);
void lan_pollfds_done(struct pollfd *pfd, int n);
void lan_stats(void);
/* Z (netpipe のブリッジ) を LAN の 1 台にする。zip が変わったら呼ぶ (0 で外す) */
void lan_set_zip(uint32_t zip_be);
void lan_forward_to(uint32_t zip_be);
/* Z からの IP パケット 1 個を LAN に出す */
void lan_from_z(const uint8_t *ip, int len);
/* Z 宛ての IP パケット (netpipe.c が受け取る)。LAN 側が呼ぶ */
void np_to_z(const uint8_t *ip, int len);

/* ---- サービスの口 ----
 * ほかの Ethernet の口を LAN に足す (Pico 自身の lwIP の netif の代わり。tistack の tsp_svc.c の lwIP をここにつなぐ):
 *   mac / ip_be の宛て (と ARP 要求、同報) のフレームを rx に渡す。送るときは lan_send */
typedef void (*lan_rx_fn)(const uint8_t *frame, int len, void *arg);
int lan_attach(const uint8_t mac[6], uint32_t ip_be, lan_rx_fn rx, void *arg);
void lan_send(const uint8_t *frame, int len);

/* netpipe のレコードのうち先頭が mark のものを受け持つサービス (0xA6 = tistack のソケット要求など)。
 *   exchange: Z の要求 1 個 (mark から reqlen バイト) を処理し、応答を res に書いて長さを返す (0 = 載せない)。
 *             *more = 1 で Z にまだ渡すものがある (応答見出しの more に足す)
 *   poll:     ループの 1 周ごと (lwIP の sys_check_timeouts など)。NULL 可
 * 登録は main の前 (__attribute__((constructor))) か np_init の前に */
struct picosim_svc {
  const char *name;
  uint8_t mark;
  void (*init)(void);
  int (*exchange)(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more);
  void (*poll)(void);
};
void picosim_register_svc(const struct picosim_svc *svc);

#endif
