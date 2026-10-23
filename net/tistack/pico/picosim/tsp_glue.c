/*
 * tsp_glue.c -- harness/picosim (Mac の上の Pico の代わり) に tsp_svc (Pico 側のソケット窓口) と lwIP を載せる
 *
 *   make -C harness/picosim SVC="..." (net/tistack/tests/xeij_pico.sh がする)
 *
 * lwIP の netif は picosim の LAN (libslirp) の 1 台になる (lan_attach / lan_send)。IP は既定で Z の IP
 * (192.168.1.68): Pico が Z の IP を自分の IP として名乗る形 (README「SLIP / xip.x の道との共存」の案)。
 * picosim は Z の IP を名乗る口があれば代理 ARP をしないので、-L の転送 (Mac → Z:2323) はこの lwIP に来る。
 * 環境変数: TSP_IP (既定 192.168.1.68)、TSP_GW (192.168.1.1)、TSP_DNS (192.168.1.3 = slirp の DNS)、
 *           TSP_OFF=1 (0xA6 に答えない: TSP を知らない今のファームの真似)
 * 名前 mac.test はゲートウェイ (Mac) を指す (lwIP の DNS の手元の表。ほかの名前は DNS に聞く)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/dns.h"
#include "netif/ethernet.h"
#include "tsp.h"
#include "tsp_svc.h"
#include "picosim.h"

int tsp_log_on = 1;
static struct netif nif;
static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x76 };

u32_t sys_now(void) { return (u32_t)(picosim_now_us() / 1000); }
u32_t lwip_port_rand(void) { return (u32_t)random(); }

static err_t out(struct netif *n, struct pbuf *p)
{
  uint8_t f[1600];
  (void)n;
  if (p->tot_len > sizeof f)
    return ERR_IF;
  pbuf_copy_partial(p, f, p->tot_len, 0);
  lan_send(f, p->tot_len);
  return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
  n->name[0] = 'w';
  n->name[1] = 'l';
  n->output = etharp_output;
  n->linkoutput = out;
  n->mtu = 1500;
  n->hwaddr_len = 6;
  memcpy(n->hwaddr, mac, 6);
  n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
  return ERR_OK;
}

/* LAN から来たフレームは溜めておき、poll で lwIP に渡す。lwIP が送ったフレームに slirp がその場で答える
   (lan_send の中から rx が呼ばれる) ので、すぐ渡すと tcp_output の途中で tcp_input が走って壊れる
   (lwIP は入れ子に呼べない。Pico でも cyw43 の受信は別の文脈から来るので同じ形になる) */
#define RXQ 64
static struct pbuf *rxq[RXQ];
static int rxh, rxn;

static void rx(const uint8_t *f, int len, void *arg)
{
  struct pbuf *p;
  (void)arg;
  if (rxn == RXQ || !(p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL)))
    return;                             /* プールが尽きた: Wi-Fi の取りこぼしと同じ (相手が再送する) */
  pbuf_take(p, f, len);
  rxq[(rxh + rxn++) % RXQ] = p;
}

static void rx_drain(void)
{
  while (rxn) {
    struct pbuf *p = rxq[rxh];
    rxh = (rxh + 1) % RXQ;
    rxn--;
    if (nif.input(p, &nif) != ERR_OK)
      pbuf_free(p);
  }
}

static uint32_t env_ip(const char *name, const char *def)
{
  const char *v = getenv(name);
  return inet_addr(v && *v ? v : def);
}

static void init(void)
{
  ip4_addr_t ip, mask, gw, dns;
  ip_addr_t d, m;
  ip.addr = env_ip("TSP_IP", "192.168.1.68");
  gw.addr = env_ip("TSP_GW", "192.168.1.1");
  dns.addr = env_ip("TSP_DNS", "192.168.1.3");
  mask.addr = inet_addr("255.255.255.0");
  setvbuf(stdout, 0, _IOLBF, 0);         /* 落ちたときに記録が残るように */
  lwip_init();
  netif_add(&nif, &ip, &mask, &gw, NULL, nif_init, ethernet_input);
  netif_set_default(&nif);
  netif_set_up(&nif);
  netif_set_link_up(&nif);
  ip_addr_copy_from_ip4(d, dns);
  dns_setserver(0, &d);
  ip_addr_copy_from_ip4(m, gw);
  dns_local_addhost("mac.test", &m);
  lan_attach(mac, ip.addr, rx, 0);
  srandom((unsigned)picosim_now_us() ^ (unsigned)getpid());
  tsp_init((uint32_t)random() | 1);
  picosim_log("tsp: lwIP %s on the LAN, socket service for tis.x -p (0xA6)\n", ip4addr_ntoa(&ip));
}

static void poll_(void)
{
  rx_drain();
  sys_check_timeouts();
}

static int exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  int n;
  static int off = -1;
  if (off < 0)
    off = getenv("TSP_OFF") && *getenv("TSP_OFF") == '1';
  if (off)
    return 0;                           /* TSP_OFF=1: 0xA6 を知らない今のファームの真似 (答えない) */
  rx_drain();                           /* 届いているもの (ACK・データ) を先に lwIP に */
  n = tsp_exchange(req, reqlen, res, resmax, more);
  return n;
}

static const struct picosim_svc svc = { "tistack TSP (lwIP)", TSP_MARK, init, exchange, poll_ };

__attribute__((constructor)) static void reg(void)
{
  picosim_register_svc(&svc);
}
