/*
 * npd.sys -- ESP/X (xip.x / inetd.x) 用 netpipe ネットワークドライバ (Human68k / X68000 Z)
 *
 * CONFIG.SYS: DEVICE = \SYS\npd.sys  (slip.sys / ppp.sys の代わり。どれも /dev/ep0 なので同時には入れない)
 *
 * xip.x からは Ethernet カードのドライバ (ESP/X の etherdrv API) に見える。運ぶのは常駐スレッド npd.x で、
 * x68kzremotedrv (Pico W) の SCSI ID 6 を通して Wi-Fi の LAN に出る (Pico がブリッジする)。
 * 中身は IP パケットを長さ付きで並べたレコードで、SLIP のようなバイトの詰め替えはしない。
 *   送信: xip.x から受けた Ethernet フレームの見出し 14 バイトを外し、[長さ 2][IP パケット][偶数にそろえる詰め物]
 *         を送信リングに積んですぐ戻る。npd.x が ETD 命令 19 でまとめて取り出す。
 *   受信: npd.x が Pico から受けたパケットを ETD 命令 16 で渡してくるので、見出しを付けて xip.x の受信ハンドラを呼ぶ。
 * ARP は捨てる (xip.x は相手に ARP を引かない)。自分の MAC = 02:00:00:00:00:02、相手 = 02:00:00:00:00:01。
 *
 * 暗号アクセラレータ (net/accel): ssh.x などの要求 (X25519 / Ed25519) を 1 つ預かり (ETD 命令 20)、npd.x が送信
 * レコードの後ろに制御レコードとして載せ (ETD 19)、Pico の応答を受け取る (ETD 23)。要求側は ETD 21 で結果を取る。
 * 要求も結果もここ (常駐) のバッファに写すので、要求側が途中で終了しても解放されたメモリには書かない。
 */
#include <stdint.h>
#include "../accel/accel_proto.h"

#define ETH_HLEN  14
#define TXSIZE    8192       /* 送信リング (2 の累乗) */
#define ID_EP0    (('e' << 24) | ('p' << 16) | ('0' << 8))

typedef void (*handler_t)(long, void *, long);

extern void call_handler(handler_t, long, void *, long);
extern void dos_print(const char *);

static uint8_t mymac[6]   = { 2, 0, 0, 0, 0, 2 };
static const uint8_t peermac[6] = { 2, 0, 0, 0, 0, 1 };
static handler_t ip_handler, arp_handler;

/* 送信リング。書くのは npd_send (xip.x が SUPER_JSR で呼ぶ)、読むのは npd.x のスレッド (ETD 命令 19)。
 * どちらもスーパーバイザモードで、Human68k はスーパーバイザモードの途中ではスレッドを切り替えない */
static uint8_t txbuf[TXSIZE];
static volatile uint16_t tx_head, tx_tail;

/* npd.x のスレッドが動いているか */
static uint8_t running;
/* 受信を xip.x に渡してよいか。xip.x の設定 (ifconfig / inetdconf) が済んで npd.x が起動される (ETD 命令 18) まで
   渡さない (設定前の xip.x にパケットを渡すと落ちた)。xip.x -r で IP の受信ハンドラが外れたら 0 に戻す */
static uint8_t accepting;

/* 統計 (etd_get_statistics) */
static uint32_t st_rx, st_tx, st_drop;

/* 暗号アクセラレータの要求 (同時に 1 つ) */
#define ACC_IDLE    0                    /* 空 */
#define ACC_QUEUED  1                    /* 要求あり (まだ送っていない) */
#define ACC_SENT    2                    /* 送った (応答待ち) */
#define ACC_DONE    3                    /* 応答あり (要求側が取るのを待つ) */
static uint8_t acc_state;
static uint16_t acc_id, acc_nextid, acc_reqlen, acc_reslen;
static uint8_t acc_req[ACCEL_REQ_MAX];
static uint8_t acc_res[ACCEL_RES_MAX];
static uint32_t st_acc_req, st_acc_res, st_acc_stale;

/* tis.x -p の入口 (TSP レコードを作る / 受ける)。0 = 居ない */
static void *tsp_entry;
static void *zss_entry;                    /* (x68k-zos の zos.x) 端末のセッション (0xA7) の入口 */

#define TX_PUT(c) (txbuf[h] = (c), h = (h + 1) & (TXSIZE - 1))

/* IP パケットをレコードにして送信リングに積む。入りきらないときは捨てる (TCP が再送する) */
static void npd_send(const uint8_t *p, long n)
{
  uint16_t h = tx_head;
  if (((tx_tail - h - 1) & (TXSIZE - 1)) < n + 3) {
    st_drop++;
    return;
  }
  TX_PUT(n >> 8);
  TX_PUT(n);
  while (n-- > 0)
    TX_PUT(*p++);
  if (h & 1)                               /* リングの先頭からの位置は常に偶数 (レコードの長さが偶数) */
    TX_PUT(0);
  tx_head = h;                             /* 積み終わってから見せる */
  st_tx++;
}

static handler_t *slot(long type)
{
  if (type == 0x0800) return &ip_handler;
  if (type == 0x0806) return &arp_handler;
  return 0;
}

/* ETD 命令 (ESP/X etherdrv.h の etd_cmd。ppp.sys / slip.sys と同じ解釈) */
long etd_dispatch(long cmd, long *a)
{
  handler_t *h;
  int i;
  switch (cmd) {
  case -1:                                 /* trap 番号の問い合わせ: -1 = 直接呼び出し */
    return -1;
  case 0:                                  /* etd_get_driver_version */
    return 0x100;
  case 1:                                  /* etd_get_mac_addr */
  case 2:                                  /* etd_get_prom_addr */
    for (i = 0; i < 6; i++) ((uint8_t *)a)[i] = mymac[i];
    return (long)a;
  case 3:                                  /* etd_set_mac_addr */
    for (i = 0; i < 6; i++) mymac[i] = ((uint8_t *)a)[i];
    return 0;
  case 4: {                                /* etd_send_ether_packet: a[0] = size, a[1] = frame */
    long size = a[0];
    const uint8_t *f = (const uint8_t *)a[1];
    if (size > ETH_HLEN && f[12] == 0x08 && f[13] == 0x00)
      npd_send(f + ETH_HLEN, size - ETH_HLEN);
    return 0;                              /* ARP など IPv4 以外は捨てる */
  }
  case 5:                                  /* etd_set_int_addr: a[0] = type, a[1] = handler */
    if (!(h = slot(a[0])))
      return -1;
    *h = (handler_t)a[1];
    return 0;
  case 6:                                  /* etd_get_int_addr: *a = type */
    if (!(h = slot(*a)))
      return -1;
    return (long)*h;
  case 7:                                  /* etd_del_int_addr: *a = type */
    if (!(h = slot(*a)))
      return -1;
    *h = 0;
    if (h == &ip_handler)
      accepting = 0;
    return 0;
  case 8:                                  /* etd_set_multicast_addr */
    return 0;
  case 9:                                  /* etd_get_statistics: rx, tx, drop, 0 */
    ((uint32_t *)a)[0] = st_rx; ((uint32_t *)a)[1] = st_tx;
    ((uint32_t *)a)[2] = st_drop; ((uint32_t *)a)[3] = 0;
    return 0;
  case 16: {                               /* (独自) npd.x が受けた IP パケットを渡す:
                                              a[0] = IP の長さ, a[1] = 見出し 14 バイト分の余白付きの先頭 */
    uint8_t *p = (uint8_t *)a[1];
    if (!ip_handler || !accepting) {
      st_drop++;
      return 0;
    }
    for (i = 0; i < 6; i++) { p[i] = mymac[i]; p[6 + i] = peermac[i]; }
    p[12] = 0x08; p[13] = 0x00;            /* 見出し: 宛先 = 自分, 送信元 = 相手, 種別 = IPv4 */
    call_handler(ip_handler, a[0] + ETH_HLEN, p, ID_EP0);
    st_rx++;
    return 1;
  }
  case 18:                                 /* (独自) npd.x の起動: 受信の受け入れを始める。戻り値 = スレッドが動いていれば 1 */
    i = running;
    running = 1;
    accepting = 1;
    return i;
  case 19: {                               /* (独自) 送信リングからレコードをまとめて取り出す:
                                              a[0] = 書く先, a[1] = 最大バイト数。戻り値 = 書いたバイト数 */
    uint8_t *d = (uint8_t *)a[0];
    long max = a[1], n = 0;
    uint16_t t = tx_tail, hd = tx_head;
    while (t != hd) {
      long len = (txbuf[t] << 8) | txbuf[(t + 1) & (TXSIZE - 1)];
      long rl = 2 + ((len + 1) & ~1);
      if (n + rl > max)
        break;
      while (rl-- > 0) {
        d[n++] = txbuf[t];
        t = (t + 1) & (TXSIZE - 1);
      }
    }
    tx_tail = t;
    /* 暗号アクセラレータの要求があれば、制御レコード [長さ 2][ACCEL_MARK ...][詰め物] として後ろに載せる */
    if (acc_state == ACC_QUEUED) {
      long rl = 2 + ((acc_reqlen + 1) & ~1);
      if (n + rl <= max) {
        d[n++] = acc_reqlen >> 8;
        d[n++] = acc_reqlen;
        for (i = 0; i < acc_reqlen; i++)
          d[n++] = acc_req[i];
        if (acc_reqlen & 1)
          d[n++] = 0;
        acc_state = ACC_SENT;
      }
    }
    return n;
  }
  case 20: {                               /* (accel) 要求を預かる: a[0] = 要求 (ACCEL_MARK から), a[1] = 長さ。
                                              id (+2) はここで振る。戻り値 = id (> 0)、預かれなければ -1 (別の要求が進行中) / -2 (長さ) */
    const uint8_t *p = (const uint8_t *)a[0];
    long len = a[1];
    if (acc_state != ACC_IDLE)
      return -1;
    if (len < ACCEL_REQ_HDR || len > ACCEL_REQ_MAX || p[0] != ACCEL_MARK)
      return -2;
    for (i = 0; i < len; i++)
      acc_req[i] = p[i];
    if (++acc_nextid == 0)
      acc_nextid = 1;
    acc_id = acc_nextid;
    accel_put16(acc_req + 2, acc_id);
    acc_reqlen = len;
    acc_state = ACC_QUEUED;
    st_acc_req++;
    return acc_id;
  }
  case 21: {                               /* (accel) 結果を取る: a[0] = 書く先, a[1] = 最大バイト数。
                                              戻り値 = 応答の長さ (> 0、取ったら空になる) / 0 (まだ) / -1 (要求が無い) / -3 (入らない) */
    uint8_t *d = (uint8_t *)a[0];
    if (acc_state == ACC_IDLE)
      return -1;
    if (acc_state != ACC_DONE)
      return 0;
    if (acc_reslen > a[1])
      return -3;
    for (i = 0; i < acc_reslen; i++)
      d[i] = acc_res[i];
    acc_state = ACC_IDLE;
    return acc_reslen;
  }
  case 22:                                 /* (accel) 要求を取り消す (遅れて届いた応答は捨てる) */
    acc_state = ACC_IDLE;
    return 0;
  case 23: {                               /* (accel, npd.x から) Pico の応答: a[0] = 長さ, a[1] = 応答 (ACCEL_MARK から)。
                                              戻り値 = 1 受け取った / 0 捨てた (id が違う、待っていない) */
    const uint8_t *p = (const uint8_t *)a[1];
    long len = a[0];
    if (len < ACCEL_RES_HDR || len > ACCEL_RES_MAX || acc_state != ACC_SENT || accel_get16(p + 2) != acc_id) {
      st_acc_stale++;
      return 0;
    }
    for (i = 0; i < len; i++)
      acc_res[i] = p[i];
    acc_reslen = len;
    acc_state = ACC_DONE;
    st_acc_res++;
    return 1;
  }
  case 25:                                 /* (tistack) tis.x -p の入口を預かる (0 で外す) */
    tsp_entry = (void *)a;
    return 0;
  case 26:                                 /* (tistack, npd.x から) tis.x の入口。0 = 居ない */
    return (long)tsp_entry;
  case 27:                                 /* (zos.x) 端末のセッション (0xA7、net/npd/zss.h) の入口を預かる (0 で外す) */
    zss_entry = (void *)a;
    return 0;
  case 28:                                 /* (zos.x、npd.x から) その入口。0 = 居ない */
    return (long)zss_entry;
  case 24:                                 /* (accel) 統計: state, req, res, stale */
    ((uint32_t *)a)[0] = acc_state; ((uint32_t *)a)[1] = st_acc_req;
    ((uint32_t *)a)[2] = st_acc_res; ((uint32_t *)a)[3] = st_acc_stale;
    return 0;
  default:
    return -1;
  }
}

struct req_header {
  uint8_t magic, unit, command, errl, errh, reserved[8], attr;
  void *addr;
  uint32_t status;
  void *fcb;
} __attribute__((packed, aligned(2)));

/* デバイスドライバの割り込みルーチン (初期化だけ受け付ける) */
long dev_interrupt(struct req_header *req)
{
  extern char _end, __bss_start;
  if (req->command == 0) {
    /* CONFIG.SYS から組み込まれるデバイスドライバの BSS は 0 クリアされない */
    char *b;
    for (b = &__bss_start; b < &_end; b++)
      *b = 0;
    dos_print("\r\nnetpipe driver for ESP/X (/dev/ep0) npd.sys 1.3 (accel, tistack, zss)\r\n");
    req->addr = &_end;
    return 0;
  }
  return 0x5003;                           /* 未対応のコマンド */
}
