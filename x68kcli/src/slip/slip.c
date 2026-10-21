/*
 * slip.sys -- ESP/X (xip.x / inetd.x) 用 SLIP ネットワークドライバ (Human68k / X68000 Z)
 *
 * CONFIG.SYS: DEVICE = \SYS\slip.sys  (ppp.sys の代わり。どちらも /dev/ep0 なので同時には入れない)
 *
 * xip.x からは Ethernet カードのドライバ (ESP/X の etherdrv API) に見える。シリアルの読み書きは
 * 常駐スレッド slipd.x が ppp.x と同じく IOCS (_INP232C / _OUT232C、つまり ROM と RSDRV.SYS) で行い、
 * このドライバは SCC にも割り込みベクタにも触らない。
 *   送信: xip.x から受けた Ethernet フレームの見出し 14 バイトを外し、IP パケットを SLIP (RFC 1055) にして
 *         送信リングに積んですぐ戻る。slipd.x が ETD 命令 17 で 1 バイトずつ取り出して _OUT232C で書く。
 *   受信: slipd.x が _INP232C で読んで SLIP をほどき、ETD 命令 16 で渡してくるので、見出しを付けて
 *         xip.x の受信ハンドラを呼ぶ。
 * 以前は SCC の受信・送信割り込みを自前で横取りしていたが、xip.x のヒープやスタックが壊れて
 * バスエラー / アドレスエラー / 画面のスクロール崩れが起きた。ROM の割り込み処理に任せる今の形では起きない。
 *
 * - 通信速度は IOCS _SET232C に任せる (X68000 Z は IOCS 経由の設定だけ UART に反映する)。
 * - ARP は捨てる (xip.x は SLIP の相手に ARP を引かない)。
 *   自分の MAC = 02:00:00:00:00:02、相手 = 02:00:00:00:00:01 (どちらも中身に意味はない)。
 */
#include <stdint.h>

#define SLIP_END      0xc0
#define SLIP_ESC      0xdb
#define SLIP_ESC_END  0xdc
#define SLIP_ESC_ESC  0xdd

#define ETH_HLEN  14
#define TXSIZE    8192       /* 送信リング (2 の累乗。SLIP にした 1500 バイトのパケットは最悪 3002 バイト) */
#define ID_EP0    (('e' << 24) | ('p' << 16) | ('0' << 8))

typedef void (*handler_t)(long, void *, long);

extern void call_handler(handler_t, long, void *, long);
extern void dos_print(const char *);

static uint8_t mymac[6]   = { 2, 0, 0, 0, 0, 2 };
static const uint8_t peermac[6] = { 2, 0, 0, 0, 0, 1 };
static handler_t ip_handler, arp_handler;

/* 送信リング。書くのは slip_send (xip.x が SUPER_JSR で呼ぶ)、読むのは slipd.x のスレッド (ETD 命令 17)。
 * どちらもスーパーバイザモードで、Human68k はスーパーバイザモードの途中ではスレッドを切り替えないので
 * 割り込みを止める必要はない。止めてはいけない: 以前は積む間 IPL 7 にしていて、1 パケット数ミリ秒の間
 * SCC の受信 (FIFO 3 バイト = 38400 bps で 0.8 ミリ秒) が溢れ、受信パケットが壊れた */
static uint8_t txbuf[TXSIZE];
static volatile uint16_t tx_head, tx_tail;

/* slipd.x のスレッドが動いているか */
static uint8_t running;
/* 受信を xip.x に渡してよいか。xip.x の設定 (ifconfig / inetdconf) が済んで slipd.x が起動される (ETD 命令 18) まで
   渡さない。設定前の xip.x にパケットを渡すと xip.x のコードを書き換えて (/ を \ に) 落ちた。
   xip.x -r で IP の受信ハンドラが外れたら 0 に戻し、次の netup の slipd.x で再開する */
static uint8_t accepting;

/* 統計 (etd_get_statistics) */
static uint32_t st_rx, st_tx, st_drop;

#define TX_PUT(c) (txbuf[h] = (c), h = (h + 1) & (TXSIZE - 1))

/* IP パケットを SLIP にして送信リングに積む。入りきらないときは捨てる (TCP が再送する) */
static void slip_send(const uint8_t *p, long n)
{
  uint16_t h = tx_head;
  if (((tx_tail - h - 1) & (TXSIZE - 1)) < 2 * n + 2) {
    st_drop++;
    return;
  }
  TX_PUT(SLIP_END);                        /* 前のゴミを区切る */
  while (n-- > 0) {
    uint8_t c = *p++;
    if (c == SLIP_END) {
      TX_PUT(SLIP_ESC);
      TX_PUT(SLIP_ESC_END);
    } else if (c == SLIP_ESC) {
      TX_PUT(SLIP_ESC);
      TX_PUT(SLIP_ESC_ESC);
    } else {
      TX_PUT(c);
    }
  }
  TX_PUT(SLIP_END);
  tx_head = h;                             /* 積み終わってから見せる */
  st_tx++;
}

static handler_t *slot(long type)
{
  if (type == 0x0800) return &ip_handler;
  if (type == 0x0806) return &arp_handler;
  return 0;
}

/* ETD 命令 (ESP/X etherdrv.h の etd_cmd。ppp.sys と同じ解釈) */
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
      slip_send(f + ETH_HLEN, size - ETH_HLEN);
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
  case 16: {                               /* (slip.sys 独自) slipd.x が組み立てた IP パケットを渡す:
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
  case 17: {                               /* (slip.sys 独自) 送信リングから 1 バイト取り出す。空なら -1 */
    long c = -1;
    uint16_t t = tx_tail;
    if (t != tx_head) {
      c = txbuf[t];
      tx_tail = (t + 1) & (TXSIZE - 1);
    }
    return c;
  }
  case 18:                                 /* (slip.sys 独自) slipd.x の起動: 受信の受け入れを始める。戻り値 = スレッドが動いていれば 1 */
    i = running;
    running = 1;
    accepting = 1;
    return i;
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
    /* CONFIG.SYS から組み込まれるデバイスドライバの BSS は 0 クリアされない。
       ip_handler などにゴミが残ると、でたらめなアドレスを呼んで落ちる */
    char *b;
    for (b = &__bss_start; b < &_end; b++)
      *b = 0;
    dos_print("\r\nSLIP driver for ESP/X (/dev/ep0) slip.sys 1.0\r\n");
    req->addr = &_end;
    return 0;
  }
  return 0x5003;                           /* 未対応のコマンド */
}
