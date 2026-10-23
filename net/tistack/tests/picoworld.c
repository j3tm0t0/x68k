/*
 * picoworld.c -- picosim の Pico と LAN (picoworld.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/priv/tcp_priv.h"
#include "tsp_svc.h"
#include "picoworld.h"

uint64_t world_us;
int tsp_log_on = 1;

u32_t sys_now(void) { return (u32_t)(world_us / 1000); }
u32_t lwip_port_rand(void) { return (u32_t)random(); }

static struct netif lan;
static uint32_t holed;

static err_t lan_out(struct netif *n, struct pbuf *p, const ip4_addr_t *ip)
{
  (void)n; (void)p; (void)ip;
  holed++;                              /* 10.0.0.1 以外: 黒い穴 */
  return ERR_OK;
}

static err_t lan_init(struct netif *n)
{
  n->name[0] = 'l';
  n->name[1] = 'n';
  n->output = lan_out;
  n->mtu = 1500;
  n->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
  return ERR_OK;
}

static ip_addr_t ipa(uint32_t v)
{
  ip_addr_t a;
  ip_addr_set_ip4_u32(&a, lwip_htonl(v));
  return a;
}

/* ---- 相手のサーバ (lwIP の raw API) ---- */

struct discard_stat dstat;
uint32_t src_conns, echo_conns, echo_resets, sshd_got, udp_peer_got;
struct client cl[NCLIENT];

/* 送るものの列 (echo / source / sshd / client が使う) */
struct conn {
  struct tcp_pcb *pcb;
  int kind;                             /* 'e' echo, 'd' discard, 's' source, 'h' sshd, 'r' reset, 'c' client */
  struct pbuf *q;                       /* echo: まだ送っていない受信 */
  uint64_t off;                         /* discard: 次の位置 / source: 送った量 */
  uint64_t total;                       /* source: 送る量 */
  char line[24];
  int linelen, fin, closed, ci;
  uint8_t *out;                         /* sshd / client: 送るもの */
  uint32_t outlen, outpos;
  int close_after;
};

static void conn_free(struct conn *c)
{
  if (c->q)
    pbuf_free(c->q);
  free(c->out);
  free(c);
}

static void conn_close(struct conn *c)
{
  if (c->closed)
    return;
  c->closed = 1;
  tcp_arg(c->pcb, NULL);
  tcp_recv(c->pcb, NULL);
  tcp_sent(c->pcb, NULL);
  tcp_err(c->pcb, NULL);
  if (tcp_close(c->pcb) != ERR_OK)
    tcp_abort(c->pcb);
  if (c->kind == 'c')
    cl[c->ci].closed = 1;
  conn_free(c);
}

static void pump(struct conn *c)
{
  struct tcp_pcb *pcb = c->pcb;
  if (c->kind == 'e') {
    while (c->q && tcp_sndbuf(pcb) > 0 && tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN - 2) {
      uint32_t n = c->q->tot_len;
      uint8_t b[1460];
      if (n > tcp_sndbuf(pcb)) n = tcp_sndbuf(pcb);
      if (n > sizeof b) n = sizeof b;
      pbuf_copy_partial(c->q, b, n, 0);
      if (tcp_write(pcb, b, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        break;
      c->q = pbuf_free_header(c->q, n);
      tcp_recved(pcb, n);
    }
    tcp_output(pcb);
    if (!c->q && c->fin)
      conn_close(c);
    return;
  }
  if (c->kind == 's') {
    while (c->off < c->total && tcp_sndbuf(pcb) > 0 && tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN - 2) {
      uint8_t b[1460];
      uint32_t n = c->total - c->off, k;
      if (n > tcp_sndbuf(pcb)) n = tcp_sndbuf(pcb);
      if (n > sizeof b) n = sizeof b;
      for (k = 0; k < n; k++)
        b[k] = pat(c->off + k);
      if (tcp_write(pcb, b, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        break;
      c->off += n;
    }
    tcp_output(pcb);
    if (c->total && c->off == c->total)
      conn_close(c);
    return;
  }
  if (c->out) {
    while (c->outpos < c->outlen && tcp_sndbuf(pcb) > 0 && tcp_sndqueuelen(pcb) < TCP_SND_QUEUELEN - 2) {
      uint32_t n = c->outlen - c->outpos;
      if (n > tcp_sndbuf(pcb)) n = tcp_sndbuf(pcb);
      if (n > 1460) n = 1460;
      if (tcp_write(pcb, c->out + c->outpos, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        break;
      c->outpos += n;
    }
    tcp_output(pcb);
    if (c->outpos == c->outlen && c->close_after)
      conn_close(c);
  }
}

static err_t s_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
  (void)pcb; (void)len;
  if (arg)
    pump(arg);
  return ERR_OK;
}

static err_t s_poll(void *arg, struct tcp_pcb *pcb)
{
  (void)pcb;
  if (arg)
    pump(arg);
  return ERR_OK;
}

static void s_err(void *arg, err_t err)
{
  struct conn *c = arg;
  if (!c)
    return;
  if (c->kind == 'e' && err == ERR_RST)
    echo_resets++;
  if (c->kind == 'c') {
    if (!cl[c->ci].connected)
      cl[c->ci].refused = 1;
    else
      cl[c->ci].reset = 1;
    cl[c->ci].closed = 1;
  }
  conn_free(c);
}

static err_t s_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  struct conn *c = arg;
  (void)err;
  if (!c) {
    if (p) {
      tcp_recved(pcb, p->tot_len);
      pbuf_free(p);
    }
    return ERR_OK;
  }
  if (!p) {
    c->fin = 1;
    if (c->kind == 'd') {
      dstat.fins++;
      dstat.done_us = world_us;
      conn_close(c);
    } else if (c->kind == 'e')
      pump(c);
    else
      conn_close(c);
    return ERR_OK;
  }
  switch (c->kind) {
  case 'e':
    if (c->q)
      pbuf_cat(c->q, p);
    else
      c->q = p;
    pump(c);
    return ERR_OK;
  case 'd': {
    struct pbuf *q;
    for (q = p; q; q = q->next) {
      uint32_t k;
      for (k = 0; k < q->len; k++)
        if (((uint8_t *)q->payload)[k] != pat(c->off + k))
          dstat.bad++;
      c->off += q->len;
    }
    dstat.bytes += p->tot_len;
    break;
  }
  case 's': {
    uint32_t k;
    for (k = 0; k < p->tot_len && c->linelen < (int)sizeof c->line - 1; k++) {
      char ch = pbuf_get_at(p, k);
      if (ch == '\n') {
        c->line[c->linelen] = 0;
        c->total = strtoull(c->line, 0, 10);
        pump(c);
        break;
      }
      c->line[c->linelen++] = ch;
    }
    break;
  }
  case 'h':
    sshd_got += p->tot_len;
    if (sshd_got >= 26 + 304) {         /* "SSH-2.0-dropbear_2022.83\r\n" + KEXINIT */
      tcp_recved(pcb, p->tot_len);
      pbuf_free(p);
      conn_close(c);
      return ERR_OK;
    }
    break;
  case 'r':
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    tcp_arg(pcb, NULL);
    tcp_abort(pcb);
    conn_free(c);
    return ERR_ABRT;
  case 'c': {
    struct client *k = &cl[c->ci];
    uint32_t n = p->tot_len;
    if (k->rxlen + n > sizeof k->rx)
      n = sizeof k->rx - k->rxlen;
    pbuf_copy_partial(p, k->rx + k->rxlen, n, 0);
    k->rxlen += n;
    break;
  }
  }
  tcp_recved(pcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

static struct conn *conn_new(struct tcp_pcb *pcb, int kind)
{
  struct conn *c = calloc(1, sizeof *c);
  c->pcb = pcb;
  c->kind = kind;
  tcp_arg(pcb, c);
  tcp_recv(pcb, s_recv);
  tcp_sent(pcb, s_sent);
  tcp_err(pcb, s_err);
  tcp_poll(pcb, s_poll, 1);
  return c;
}

static err_t s_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
  int kind = (int)(intptr_t)arg;
  struct conn *c;
  if (err != ERR_OK || !pcb)
    return ERR_VAL;
  c = conn_new(pcb, kind);
  switch (kind) {
  case 'e': echo_conns++; break;
  case 'd': dstat.conns++; break;
  case 's': src_conns++; break;
  case 'h': {
    static const char banner[] = "SSH-2.0-OpenSSH_9.0\r\n";
    uint32_t k;
    c->outlen = strlen(banner) + 304;
    c->out = malloc(c->outlen);
    memcpy(c->out, banner, strlen(banner));
    for (k = strlen(banner); k < c->outlen; k++)
      c->out[k] = (uint8_t)k;
    pump(c);
    break;
  }
  }
  return ERR_OK;
}

static void serve(uint16_t port, int kind)
{
  struct tcp_pcb *p = tcp_new();
  tcp_bind(p, IP_ANY_TYPE, port);
  p = tcp_listen(p);
  tcp_arg(p, (void *)(intptr_t)kind);
  tcp_accept(p, s_accept);
}

/* LAN 側のクライアント */
static err_t c_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
  struct conn *c = arg;
  (void)pcb; (void)err;
  if (c) {
    cl[c->ci].connected = 1;
    cl[c->ci].lport = pcb->local_port;
    pump(c);
  }
  return ERR_OK;
}

static struct conn *ccon[NCLIENT];

int client_start(uint16_t port, const void *data, uint32_t n, int close_after)
{
  int k;
  struct tcp_pcb *pcb;
  struct conn *c;
  ip_addr_t a = ipa(PICO_IP);
  for (k = 0; k < NCLIENT; k++)
    if (!cl[k].used)
      break;
  if (k == NCLIENT)
    return -1;
  memset(&cl[k], 0, sizeof cl[k]);
  cl[k].used = 1;
  pcb = tcp_new();
  c = conn_new(pcb, 'c');
  c->ci = k;
  c->close_after = close_after;
  if (n) {
    c->out = malloc(n);
    memcpy(c->out, data, n);
    c->outlen = n;
  }
  ccon[k] = c;
  tcp_connect(pcb, &a, port, c_connected);
  return k;
}

void client_send(int k, const void *data, uint32_t n)
{
  struct conn *c = ccon[k];
  if (cl[k].closed)
    return;
  c->out = realloc(c->out, c->outlen + n);
  memcpy(c->out + c->outlen, data, n);
  c->outlen += n;
  if (cl[k].connected)
    pump(c);
}

void client_close(int k)
{
  if (!cl[k].closed)
    conn_close(ccon[k]);
  cl[k].used = 0;
}

/* UDP: echo (7)、相手の受け口 (9999)、DNS (53) */
static struct udp_pcb *uecho, *upeer, *udns;

static void u_echo(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  (void)arg;
  udp_sendto(pcb, p, addr, port);
  pbuf_free(p);
}

static void u_peer(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  (void)arg; (void)pcb; (void)addr; (void)port;
  udp_peer_got++;
  pbuf_free(p);
}

void udp_peer_send(uint16_t from_port, uint16_t to_port, const void *data, uint32_t n)
{
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
  ip_addr_t a = ipa(PICO_IP);
  (void)from_port;
  pbuf_take(p, data, n);
  udp_sendto(upeer, p, &a, to_port);
  pbuf_free(p);
}

/* DNS: *.test の A を答える (hole.test は黒い穴、それ以外は 10.0.0.1)。ほかは NXDOMAIN */
static void u_dns(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  uint8_t q[512], r[600];
  int n = p->tot_len, k, qend, ok;
  char name[256];
  int nl = 0;
  (void)arg;
  if (n > (int)sizeof q || n < 17) {
    pbuf_free(p);
    return;
  }
  pbuf_copy_partial(p, q, n, 0);
  pbuf_free(p);
  for (k = 12; k < n && q[k]; k += q[k] + 1) {
    if (nl)
      name[nl++] = '.';
    memcpy(name + nl, q + k + 1, q[k]);
    nl += q[k];
  }
  name[nl] = 0;
  qend = k + 5;
  ok = nl > 5 && !strcmp(name + nl - 5, ".test");
  memcpy(r, q, qend);
  r[2] = 0x81;
  r[3] = ok ? 0x80 : 0x83;
  r[6] = 0; r[7] = ok ? 1 : 0;
  r[8] = r[9] = r[10] = r[11] = 0;
  k = qend;
  if (ok) {
    uint32_t a = !strcmp(name, "hole.test") ? HOLE_IP : PICO_IP;
    static const uint8_t ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
    memcpy(r + k, ans, sizeof ans);
    k += sizeof ans;
    r[k++] = a >> 24; r[k++] = a >> 16; r[k++] = a >> 8; r[k++] = a;
  }
  p = pbuf_alloc(PBUF_TRANSPORT, k, PBUF_RAM);
  pbuf_take(p, r, k);
  udp_sendto(pcb, p, addr, port);
  pbuf_free(p);
}

/* ---- 入口 ---- */

void world_init(uint32_t pepoch)
{
  ip_addr_t a = ipa(PICO_IP), m = ipa(0xffffff00), g = ipa(0);
  world_us = 1000000;
  lwip_init();
  netif_add(&lan, ip_2_ip4(&a), ip_2_ip4(&m), ip_2_ip4(&g), NULL, lan_init, ip_input);
  netif_set_up(&lan);
  netif_set_link_up(&lan);
  dns_setserver(0, &a);
  serve(7, 'e');
  serve(9, 'd');
  serve(19, 's');
  serve(22, 'h');
  serve(70, 'r');
  uecho = udp_new();
  udp_bind(uecho, IP_ANY_TYPE, 7);
  udp_recv(uecho, u_echo, 0);
  upeer = udp_new();
  udp_bind(upeer, IP_ANY_TYPE, 9999);
  udp_recv(upeer, u_peer, 0);
  udns = udp_new();
  udp_bind(udns, IP_ANY_TYPE, 53);
  udp_recv(udns, u_dns, 0);
  tsp_init(pepoch);
}

void world_poll(void)
{
  int k;
  for (k = 0; k < 4; k++)
    netif_poll(&lan);
  sys_check_timeouts();
}

int world_exchange(const uint8_t *req, int n, uint8_t *res, int max, int *more)
{
  int r = tsp_exchange(req, n, res, max, more);
  world_poll();
  return r;
}

void world_pico_reboot(uint32_t pepoch)
{
  tsp_shutdown();
  tsp_init(pepoch);
}

int world_pico_sockets(void) { return tsp_nsock(); }
uint32_t world_errmem(void) { return tsp_stat.errmem; }
uint32_t world_held_peak(void) { return tsp_stat.held_peak; }
uint32_t world_sndq_peak(void) { return tsp_stat.sndq_peak; }
uint32_t world_dup(void) { return tsp_stat.dup; }

int world_tcp_pcbs(void)
{
  int n = 0;
  struct tcp_pcb *p;
  for (p = tcp_active_pcbs; p; p = p->next)
    n++;
  return n;
}

void world_stat(void)
{
  printf("  pico: xchg %u dup %u badreq %u aborts %u connects %u accepts %u refused %u dns %u\n",
         tsp_stat.xchg, tsp_stat.dup, tsp_stat.badreq, tsp_stat.aborts, tsp_stat.connects, tsp_stat.accepts,
         tsp_stat.refused, tsp_stat.dns);
  printf("  pico: tx %u rx %u dgram in %u out %u drop %u errmem %u, held peak %u, sndq peak %u, holed %u\n",
         tsp_stat.tx_bytes, tsp_stat.rx_bytes, tsp_stat.dgrams_in, tsp_stat.dgrams_out, tsp_stat.udp_drop,
         tsp_stat.errmem, tsp_stat.held_peak, tsp_stat.sndq_peak, holed);
}
