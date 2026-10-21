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
 */
#include <stdint.h>

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
    return n;
  }
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
    dos_print("\r\nnetpipe driver for ESP/X (/dev/ep0) npd.sys 1.0\r\n");
    req->addr = &_end;
    return 0;
  }
  return 0x5003;                           /* 未対応のコマンド */
}
