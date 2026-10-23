/*
 * lan_slirp.c -- picosim の LAN (Pico の Wi-Fi の代わり)。libslirp (QEMU の user-mode network) の仮想 LAN に Z を 1 台として置く
 *
 * root も tun/utun も要らない。Z から外への TCP/UDP は Mac のソケットで出ていき (NAT)、ゲートウェイ (既定 192.168.1.1)
 * 宛ては Mac の 127.0.0.1 に届く。ping (ICMP echo) はゲートウェイなら slirp が自分で答える。
 * Mac から Z へは -L で決めたポート (既定 127.0.0.1:2323 -> Z:2323 = rcmd.x) を転送する。
 *
 * ファームのブリッジとの対応: Pico が Z の IP を名乗る (代理 ARP) のと同じく、slirp からの Z 宛ての ARP 要求には
 * Z の (架空の) MAC で答え、Z 宛ての IPv4 を netpipe (np_to_z) に渡す。Z からの IP は Ethernet に包んで slirp に渡す。
 * ほかに lan_attach で口 (Pico 自身の lwIP の netif など) を足せる。その IP / MAC 宛てのフレームはそちらに渡す。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "picosim.h"
#include "libslirp.h"

struct lan_config lan_cfg;
FILE *lan_pcap;                         /* -p: LAN を流れたフレームを pcap に書く (tcpdump -r で読む) */

static void pcap_frame(const uint8_t *f, int len)
{
  static int hdr;
  uint64_t t = picosim_now_us();
  uint32_t h[4] = { (uint32_t)(t / 1000000), (uint32_t)(t % 1000000), (uint32_t)len, (uint32_t)len };
  if (!hdr) {
    static const uint32_t g[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1 };
    fwrite(g, 4, 6, lan_pcap);          /* 2.4、Ethernet、ホストのバイト順 */
    hdr = 1;
  }
  fwrite(h, 4, 4, lan_pcap);
  fwrite(f, 1, len, lan_pcap);
  fflush(lan_pcap);
}

static Slirp *slirp;
static const uint8_t z_mac[6] = { 0x02, 0x58, 0x36, 0x38, 0x00, 0x68 };   /* Z の架空の MAC ("X68") */
static uint8_t gw_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };          /* slirp の MAC (最初のフレームで覚える) */
static uint32_t zip;
static uint32_t st_in, st_out, st_arp;

#define NATT 4
static struct { uint8_t mac[6]; uint32_t ip; lan_rx_fn rx; void *arg; } att[NATT];
static int natt;

/* ---- slirp の時計とタイマー ---- */
struct ps_timer { SlirpTimerId id; void *cb_opaque; int64_t expire_ms; int armed; struct ps_timer *next; };
static struct ps_timer *timers;

static int64_t clock_ns(void *opaque)
{
  (void)opaque;
  return (int64_t)picosim_now_us() * 1000;
}

static void *timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
  struct ps_timer *t = calloc(1, sizeof *t);
  (void)opaque;
  t->id = id;
  t->cb_opaque = cb_opaque;
  t->next = timers;
  timers = t;
  return t;
}

static void timer_free(void *timer, void *opaque)
{
  (void)opaque;
  for (struct ps_timer **p = &timers; *p; p = &(*p)->next)
    if (*p == timer) {
      *p = (*p)->next;
      free(timer);
      return;
    }
}

static void timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
  struct ps_timer *t = timer;
  (void)opaque;
  t->expire_ms = expire_ms;
  t->armed = 1;
}

static void guest_error(const char *msg, void *opaque)
{
  (void)opaque;
  picosim_log("slirp: %s\n", msg);
}

static void reg_fd(int fd, void *opaque) { (void)fd; (void)opaque; }
static void notify(void *opaque) { (void)opaque; }

/* ---- フレーム ---- */
static void put_arp(uint8_t *f, int op, const uint8_t *smac, uint32_t sip, const uint8_t *dmac, uint32_t dip)
{
  static const uint8_t hdr[8] = { 0, 1, 8, 0, 6, 4, 0, 0 };
  memcpy(f, dmac, 6);
  memcpy(f + 6, smac, 6);
  f[12] = 0x08;
  f[13] = 0x06;
  memcpy(f + 14, hdr, 8);
  f[21] = op;
  memcpy(f + 22, smac, 6);
  memcpy(f + 28, &sip, 4);
  memcpy(f + 32, op == 2 ? dmac : (const uint8_t[6]){ 0 }, 6);
  memcpy(f + 38, &dip, 4);
}


static int find_att_ip(uint32_t ip)
{
  for (int i = 0; i < natt; i++)
    if (att[i].ip == ip)
      return i;
  return -1;
}

static int find_att_mac(const uint8_t *mac)
{
  for (int i = 0; i < natt; i++)
    if (memcmp(att[i].mac, mac, 6) == 0)
      return i;
  return -1;
}

/* LAN に出たフレーム 1 個 (slirp からと、口から) を、宛ての Z / 口 / slirp に配る。from: -1 = slirp, それ以外は口の番号 */
static void deliver(const uint8_t *f, int len, int from)
{
  static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
  int type = len >= 14 ? (f[12] << 8) | f[13] : 0;
  if (lan_pcap)
    pcap_frame(f, len);
  if (picosim_verbose >= 3)
    picosim_log("lan: %s %02x:%02x:%02x:%02x:%02x:%02x type %04x len %d\n", from == -1 ? "slirp>" : from == -2 ? "z>" : "att>",
                f[0], f[1], f[2], f[3], f[4], f[5], type, len);
  if (from == -1)
    memcpy(gw_mac, f + 6, 6);
  if (type == 0x0806 && len >= 42) {
    uint32_t tip;
    memcpy(&tip, f + 38, 4);
    if (f[21] == 1 && zip && tip == zip && find_att_ip(zip) < 0) {
      /* (つないだ口が Z の IP を名乗っているとき (tistack の tis.x -p: Pico が Z の IP を自分の IP にする) は、
         代理 ARP をせずにその口に答えさせる) */
      /* Z 宛ての ARP 要求に Z の MAC で答える (ファームの代理 ARP) */
      uint8_t r[42];
      uint32_t sip;
      memcpy(&sip, f + 28, 4);
      put_arp(r, 2, z_mac, zip, f + 22, sip);
      st_arp++;
      deliver(r, sizeof r, -2);
      return;
    }
  }
  /* 宛て先を決める: 同報は全員 (送り元以外)、それ以外は MAC で */
  int to_bcast = memcmp(f, bcast, 6) == 0;
  if (to_bcast || memcmp(f, z_mac, 6) == 0) {
    if (type == 0x0800 && len > 14 && zip) {
      uint32_t dip;
      memcpy(&dip, f + 30, 4);
      if (dip == zip) {
        st_in++;
        np_to_z(f + 14, len - 14);
      }
    }
    if (!to_bcast)
      return;
  }
  for (int i = 0; i < natt; i++)
    if (i != from && (to_bcast || memcmp(f, att[i].mac, 6) == 0)) {
      att[i].rx(f, len, att[i].arg);
      if (!to_bcast)
        return;
    }
  if (from != -1 && slirp)
    slirp_input(slirp, f, len);
}

static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque)
{
  (void)opaque;
  deliver(buf, (int)len, -1);
  return (slirp_ssize_t)len;
}

/* IPv4 の宛先か送り元を置き換え、IP と UDP のチェックサムを付け直す */
static uint16_t sum16(const uint8_t *b, int n, uint32_t s)
{
  for (int i = 0; i + 1 < n; i += 2)
    s += (b[i] << 8) | b[i + 1];
  if (n & 1)
    s += b[n - 1] << 8;
  while (s >> 16)
    s = (s & 0xffff) + (s >> 16);
  return (uint16_t)s;
}

static void readdr(uint8_t *ip, int len, int off, uint32_t addr)
{
  int hl = (ip[0] & 15) * 4;
  memcpy(ip + off, &addr, 4);
  ip[10] = ip[11] = 0;
  uint16_t c = ~sum16(ip, hl, 0);
  ip[10] = c >> 8;
  ip[11] = c;
  if (ip[9] == 17 && len >= hl + 8) {
    uint8_t *u = ip + hl;
    int ul = len - hl;
    uint8_t ph[12];
    memcpy(ph, ip + 12, 8);
    ph[8] = 0;
    ph[9] = 17;
    ph[10] = ul >> 8;
    ph[11] = ul;
    u[6] = u[7] = 0;
    c = ~sum16(u, ul, sum16(ph, 12, 0));
    if (c == 0)
      c = 0xffff;
    u[6] = c >> 8;
    u[7] = c;
  }
}

/* DNS: ゲートウェイ (.1、家のルーターと同じく zshrc の NETDNS の既定) と DNS の口 (-D、既定 .3) の UDP 53 番は、
   slirp に渡さず picosim が Mac の resolver (getaddrinfo / getnameinfo) で答える。
   slirp の DNS の転送は Mac の DNS サーバ (Tailscale の 100.100.100.100 など) に UDP で送るが、picosim からは
   "No route to host" になった (python からは届く。macOS のローカルネットワークの許可のせいと見ている)。
   getaddrinfo は mDNSResponder が引くので届く。問い合わせ 1 個 (A / PTR) だけを扱い、ほかの型は
   答え無しの NOERROR。名前を引く間 (最大数秒) は picosim のループが止まる */
static int dns_name(const uint8_t *m, int n, int off, char *out, int max)
{
  int o = 0;
  while (off < n && m[off]) {
    int l = m[off++];
    if (l > 63 || off + l > n || o + l + 2 > max)
      return -1;
    if (o)
      out[o++] = '.';
    memcpy(out + o, m + off, l);
    o += l;
    off += l;
  }
  out[o] = 0;
  return off < n ? off + 1 : -1;
}

static int dns_put_name(uint8_t *p, const char *name)
{
  int o = 0;
  while (*name) {
    const char *d = strchr(name, '.');
    int l = d ? (int)(d - name) : (int)strlen(name);
    if (l > 63)
      return -1;
    p[o++] = l;
    memcpy(p + o, name, l);
    o += l;
    name += l + (d ? 1 : 0);
  }
  p[o++] = 0;
  return o;
}

static uint32_t st_dns;
static int dns_to = -1;                 /* 答えを返す先: -1 = Z (netpipe)、それ以外は lan_attach の口の番号 */

static int dns_answer(const uint8_t *ip, int len)
{
  int hl = (ip[0] & 15) * 4;
  uint32_t dst;
  if (len < hl + 8 + 12 || ip[9] != 17 || ((ip[hl + 2] << 8) | ip[hl + 3]) != 53)
    return 0;
  memcpy(&dst, ip + 16, 4);
  if (dst != lan_cfg.gw && dst != lan_cfg.dns)
    return 0;
  const uint8_t *q = ip + hl + 8;
  int qn = len - hl - 8, qend, qtype;
  char name[256];
  if (q[2] & 0x80 || ((q[4] << 8) | q[5]) != 1 || (qend = dns_name(q, qn, 12, name, sizeof name)) < 0 || qend + 4 > qn)
    return 1;                           /* 応答や問い合わせ 2 個以上は捨てる */
  qtype = (q[qend] << 8) | q[qend + 1];
  uint8_t r[1500];
  int rl = 0, an = 0, rcode = 0;
  uint8_t *m = r + 28;                  /* IP 20 + UDP 8 の後ろ */
  memcpy(m, q, qend + 4);
  rl = qend + 4;
  if (qtype == 1) {                     /* A */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL, *a;
    if (getaddrinfo(name, NULL, &hints, &res) != 0)
      rcode = 3;
    for (a = res; a && an < 8 && rl + 16 < 1400; a = a->ai_next) {
      static const uint8_t rr[10] = { 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60 };
      memcpy(m + rl, rr, 10);
      m[rl + 10] = 0;
      m[rl + 11] = 4;
      memcpy(m + rl + 12, &((struct sockaddr_in *)a->ai_addr)->sin_addr, 4);
      rl += 16;
      an++;
    }
    if (res)
      freeaddrinfo(res);
  } else if (qtype == 12) {             /* PTR: a.b.c.d.in-addr.arpa */
    unsigned b[4];
    char host[256];
    struct sockaddr_in sa = { .sin_family = AF_INET };
#ifdef __APPLE__
    sa.sin_len = sizeof sa;
#endif
    if (sscanf(name, "%u.%u.%u.%u.in-addr.arpa", &b[3], &b[2], &b[1], &b[0]) == 4) {
      sa.sin_addr.s_addr = htonl(b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
      /* 仮想の LAN のアドレスは Mac に聞かない (家の LAN の同じアドレスの名前が返ってしまう) */
      int local = (sa.sin_addr.s_addr & lan_cfg.mask) == (lan_cfg.net & lan_cfg.mask);
      if (local)
        snprintf(host, sizeof host, "%s", sa.sin_addr.s_addr == lan_cfg.gw ? "gateway.picosim" :
                 sa.sin_addr.s_addr == zip ? "x68000z.picosim" : "");
      if (local ? host[0] != 0 : getnameinfo((struct sockaddr *)&sa, sizeof sa, host, sizeof host, NULL, 0, NI_NAMEREQD) == 0) {
        static const uint8_t rr[8] = { 0xc0, 12, 0, 12, 0, 1, 0, 0 };
        memcpy(m + rl, rr, 8);
        m[rl + 8] = 0;
        m[rl + 9] = 60;
        int nl = dns_put_name(m + rl + 12, host);
        if (nl > 0) {
          m[rl + 10] = nl >> 8;
          m[rl + 11] = nl;
          rl += 12 + nl;
          an++;
        }
      } else
        rcode = 3;
    } else
      rcode = 3;
  }
  m[2] = 0x81 | (q[2] & 0x01);          /* QR, RD を写す */
  m[3] = 0x80 | rcode;                  /* RA */
  m[6] = an >> 8;
  m[7] = an;
  m[8] = m[9] = m[10] = m[11] = 0;
  /* IP と UDP: 宛先と送り元を入れ替える */
  int tot = 28 + rl;
  memset(r, 0, 28);
  r[0] = 0x45;
  r[2] = tot >> 8;
  r[3] = tot;
  r[8] = 64;
  r[9] = 17;
  memcpy(r + 12, ip + 16, 4);
  memcpy(r + 16, ip + 12, 4);
  memcpy(r + 20, ip + hl + 2, 2);
  memcpy(r + 22, ip + hl, 2);
  r[24] = (8 + rl) >> 8;
  r[25] = 8 + rl;
  readdr(r, tot, 12, dst);              /* チェックサム */
  st_dns++;
  picosim_log("lan: DNS %s %s -> rcode %d, %d answer(s)\n", qtype == 1 ? "A" : qtype == 12 ? "PTR" : "?", name, rcode, an);
  if (dns_to < 0)
    np_to_z(r, tot);
  else {
    /* 口 (Pico 自身の lwIP など) から聞かれたら、ゲートウェイ (slirp) の MAC から返す */
    uint8_t fr[14 + 1500];
    memcpy(fr, att[dns_to].mac, 6);
    memcpy(fr + 6, gw_mac, 6);
    fr[12] = 0x08;
    fr[13] = 0x00;
    memcpy(fr + 14, r, tot);
    if (lan_pcap)
      pcap_frame(fr, 14 + tot);
    att[dns_to].rx(fr, 14 + tot, att[dns_to].arg);
  }
  return 1;
}

void lan_from_z(const uint8_t *ip, int len)
{
  uint8_t f[14 + 1500];
  uint32_t dip;
  int a;
  if (len > 1500 || len < 20)
    return;
  memcpy(&dip, ip + 16, 4);
  a = find_att_ip(dip);
  memcpy(f, a >= 0 ? att[a].mac : gw_mac, 6);
  memcpy(f + 6, z_mac, 6);
  f[12] = 0x08;
  f[13] = 0x00;
  if (dns_answer(ip, len))
    return;
  memcpy(f + 14, ip, len);
  st_out++;
  deliver(f, 14 + len, -2);
}

void lan_send(const uint8_t *frame, int len)
{
  int a = len >= 12 ? find_att_mac(frame + 6) : -1;
  /* 口からの DNS (ゲートウェイか DNS の口の UDP 53) も Z と同じく picosim が答える (slirp の DNS の転送は届かない) */
  if (a >= 0 && len > 14 + 28 && frame[12] == 0x08 && frame[13] == 0x00) {
    int tot = (frame[16] << 8) | frame[17];   /* Ethernet の詰め物を除く */
    dns_to = a;
    int done = tot >= 28 && tot <= len - 14 && dns_answer(frame + 14, tot);
    dns_to = -1;
    if (done)
      return;
  }
  deliver(frame, len, a < 0 ? -2 : a);
}

int lan_attach(const uint8_t mac[6], uint32_t ip_be, lan_rx_fn rx, void *arg)
{
  if (natt >= NATT)
    return -1;
  memcpy(att[natt].mac, mac, 6);
  att[natt].ip = ip_be;
  att[natt].rx = rx;
  att[natt].arg = arg;
  return natt++;
}

static void add_fwds(uint32_t z)
{
  struct in_addr lo = { htonl(INADDR_LOOPBACK) }, g = { z };
  for (int i = 0; i < lan_cfg.nfwd; i++) {
    if (slirp_add_hostfwd(slirp, lan_cfg.fwd[i].udp, lo, lan_cfg.fwd[i].hport, g, lan_cfg.fwd[i].zport) < 0)
      fprintf(stderr, "picosim: cannot forward %s 127.0.0.1:%d (in use?)\n", lan_cfg.fwd[i].udp ? "udp" : "tcp",
              lan_cfg.fwd[i].hport);
    else
      picosim_log("lan: forward %s 127.0.0.1:%d -> %s:%d\n", lan_cfg.fwd[i].udp ? "udp" : "tcp", lan_cfg.fwd[i].hport,
                  inet_ntoa(g), lan_cfg.fwd[i].zport);
  }
}

static void del_fwds(void)
{
  struct in_addr lo = { htonl(INADDR_LOOPBACK) };
  for (int i = 0; i < lan_cfg.nfwd; i++)
    slirp_remove_hostfwd(slirp, lan_cfg.fwd[i].udp, lo, lan_cfg.fwd[i].hport);
}

/* Z の IP への -L の転送だけを張る (代理 ARP はしない。--pzl では PZL の netpipe が Z の IP を名乗る) */
void lan_forward_to(uint32_t z)
{
  add_fwds(z);
}

void lan_set_zip(uint32_t z)
{
  if (z == zip)
    return;
  if (zip)
    del_fwds();
  zip = z;
  if (!z)
    return;
  if ((z & lan_cfg.mask) != (lan_cfg.net & lan_cfg.mask)) {
    struct in_addr a = { z };
    fprintf(stderr, "picosim: Z's IP %s is outside the LAN (-n); slirp will not answer it\n", inet_ntoa(a));
  }
  /* 名乗り: ゲートウェイへの ARP 要求 (slirp が Z の MAC を覚え、返事でこちらも slirp の MAC を覚える) */
  uint8_t f[42];
  static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
  put_arp(f, 1, z_mac, zip, bcast, lan_cfg.gw);
  deliver(f, sizeof f, -2);
  add_fwds(z);
}

/* ---- poll ---- */
static struct pollfd *cur_pfd;
static int cur_n, cur_max;

static int add_poll(int fd, int events, void *opaque)
{
  (void)opaque;
  if (cur_n >= cur_max)
    return -1;
  short e = 0;
  if (events & SLIRP_POLL_IN) e |= POLLIN;
  if (events & SLIRP_POLL_OUT) e |= POLLOUT;
  if (events & SLIRP_POLL_PRI) e |= POLLPRI;
  cur_pfd[cur_n].fd = fd;
  cur_pfd[cur_n].events = e;
  cur_pfd[cur_n].revents = 0;
  return cur_n++;
}

static int get_revents(int idx, void *opaque)
{
  (void)opaque;
  if (idx < 0 || idx >= cur_n)
    return 0;
  short r = cur_pfd[idx].revents;
  int e = 0;
  if (r & POLLIN) e |= SLIRP_POLL_IN;
  if (r & POLLOUT) e |= SLIRP_POLL_OUT;
  if (r & POLLPRI) e |= SLIRP_POLL_PRI;
  if (r & POLLERR) e |= SLIRP_POLL_ERR;
  if (r & POLLHUP) e |= SLIRP_POLL_HUP;
  return e;
}

int lan_pollfds(struct pollfd *pfd, int max, uint32_t *timeout_ms)
{
  int64_t now = (int64_t)(picosim_now_us() / 1000);
  cur_pfd = pfd;
  cur_n = 0;
  cur_max = max;
  for (struct ps_timer *t = timers; t; t = t->next)
    if (t->armed) {
      int64_t d = t->expire_ms - now;
      if (d < 0) d = 0;
      if ((uint32_t)d < *timeout_ms)
        *timeout_ms = (uint32_t)d;
    }
  slirp_pollfds_fill(slirp, timeout_ms, add_poll, NULL);
  return cur_n;
}

void lan_pollfds_done(struct pollfd *pfd, int n)
{
  int64_t now = (int64_t)(picosim_now_us() / 1000);
  cur_pfd = pfd;
  cur_n = n;
  slirp_pollfds_poll(slirp, 0, get_revents, NULL);
  for (struct ps_timer *t = timers; t; t = t->next)
    if (t->armed && t->expire_ms <= now) {
      t->armed = 0;
      slirp_handle_timer(slirp, t->id, t->cb_opaque);
    }
}

int lan_init(void)
{
  static const SlirpCb cb = {
    .send_packet = send_packet,
    .guest_error = guest_error,
    .clock_get_ns = clock_ns,
    .timer_free = timer_free,
    .timer_mod = timer_mod,
    .register_poll_fd = reg_fd,
    .unregister_poll_fd = reg_fd,
    .notify = notify,
    .timer_new_opaque = timer_new_opaque,
  };
  SlirpConfig c;
  memset(&c, 0, sizeof c);
  c.version = 4;
  c.in_enabled = true;
  c.vnetwork.s_addr = lan_cfg.net;
  c.vnetmask.s_addr = lan_cfg.mask;
  c.vhost.s_addr = lan_cfg.gw;
  c.vnameserver.s_addr = lan_cfg.dns;
  c.vdhcp_start.s_addr = htonl((ntohl(lan_cfg.net) & ntohl(lan_cfg.mask)) | 200);
  c.vhostname = "picosim";
  c.if_mtu = 1500;
  c.if_mru = 1500;
  c.disable_dhcp = true;
  slirp = slirp_new(&c, &cb, NULL);
  if (!slirp)
    return -1;
  struct in_addr a = { lan_cfg.gw };
  picosim_log("lan: slirp %s (gateway = this Mac's 127.0.0.1)\n", inet_ntoa(a));
  return 0;
}

void lan_stats(void)
{
  fprintf(stderr, "lan: Z->LAN %u frames  LAN->Z %u frames  ARP for Z %u  DNS %u\n", st_out, st_in, st_arp, st_dns);
}
