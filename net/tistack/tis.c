/*
 * tis.x -- xip.x の代わりになる常駐 TCP/IP 窓口 (_TI_* 入口表の API 層)
 *
 * libsocket (elf2x68k) / ESP/X libnetwork のプログラムは、常駐ブロックのうちプログラム領域が "TCP/IP" で
 * 始まるものを探して text + 8 を呼ぶ (start.S)。ここ (ti_dispatch) が命令番号ごとに振り分ける。
 * ソケット表・受信リング・状態・ブロッキングの待ちはここが持ち、相手との間は backend (be.h) に任せる。
 * backend はループバック (lo.c) か、TCP / UDP / DNS を Pico に任せるもの (pico.c、-p)。
 *
 *   tis.x [-a a.b.c.d] [-b KB]   常駐する
 *   tis.x -s                     ソケット表を表示する
 *   tis.x -r                     常駐を解く (-rf: ソケットが開いていても解く)
 *
 * 排他: 入口に入ったら DOS _SUPER でスーパーバイザに移る。Human68k は割り込まれた側がスーパーバイザなら
 * スレッドを切り替えないので、これで 1 CPU の上の排他になる。待つときはユーザーモードに戻って _CHANGE_PR し、
 * 戻ったら調べ直す。
 *
 * libsocket の約束事 (README.md の表):
 *   - ブロッキングの read / write が -1 を返すと、libsocket は状態が 'S' でない限り譲らずに再試行し続ける。
 *     だから中で待ち、本当のエラーで -1 を返すときは errflag を立てて、続く sockstate を 1 回だけ NULL にする
 *     (libsocket はそれで EIO にする)
 *   - ブロッキング connect は 0 を返す (-1 だと状態が 'S'/'E' なら EINPROGRESS にされる)
 *   - ノンブロッキングかどうかは usock[].noblock だけを見る (libsocket が直接書く)
 */
#define __DOS_INLINE__
#include <x68k/dos.h>
#include "be.h"

enum {
  _TI_get_version,
  _TI_add_arp_table, _TI_del_arp_table, _TI_search_arp_table, _TI_get_arp_table_top, _TI_arp_request,
  _TI_get_iface_list, _TI_get_new_iface, _TI_link_new_iface,
  _TI_rt_top, _TI_rt_lookup, _TI_rt_lookupb, _TI_rt_drop, _TI_rt_add,
  _TI_dns_add, _TI_dns_drop, _TI_dns_get, _TI_set_domain_name, _TI_get_domain_name,
  _TI_res_query, _TI_res_search, _TI_res_mkquery, _TI_res_sendquery,
  _TI_get_MIB,
  _TI_socket, _TI_bind, _TI_listen, _TI_accept, _TI_connect, _TI_read_s, _TI_write_s, _TI_recvfrom, _TI_sendto,
  _TI_close_s, _TI_socklen, _TI_getsockname, _TI_getpeername, _TI_sockkick, _TI_shutdown, _TI_usesock,
  _TI_recvline, _TI_sendline, _TI_rrecvchar, _TI_recvchar, _TI_usflush, _TI_seteol, _TI_sockmode,
  _TI_setflush, _TI_psocket, _TI_sockerr, _TI_sockstate,
  _TI_sock_top, _TI_ntoa_sock,
  _TI_gethostbyname, _TI_gethostbyaddr, _TI_getnetbyname, _TI_getnetbyaddr, _TI_getservbyname,
  _TI_getservbyport, _TI_getprotobyname, _TI_getprotobynumber,
  _TI_rip,
};

struct usock usock[NSOCK];
struct tsock ts[NSOCK];
static uint8_t errflag[NSOCK];          /* 1 = 次の sockstate で NULL を返す */
static uint16_t next_port = 1024;

struct tis_info info = { "TISINFO", usock, ts, NSOCK, 8192, 0, "loopback" };
const struct backend *be = &be_loop;

/* -ffreestanding でも gcc は構造体のコピーなどで memcpy / memset を呼ぶ (ホストの試験では libc のものを使う) */
#ifdef __m68k__
/* 受信 / 送信リングと TSP の要求・応答はこれで写すので、Z の速さの律速 (README「速さ」)。
   番地の偶奇がそろえば long で 16 バイトずつ (68000 は奇数番地に word / long で触れない)、そろわなければ 1 バイトずつを
   8 回ずつ並べる (dbra の分を減らす) */
void *memcpy(void *d, const void *s, unsigned long n)
{
  uint8_t *p = d;
  const uint8_t *q = s;
  if (n >= 16) {
    unsigned long k;
    if ((((uint32_t)p ^ (uint32_t)q) & 1) == 0) {
      if ((uint32_t)p & 1) {
        *p++ = *q++;
        n--;
      }
      if ((k = n >> 4) != 0)
        __asm__ volatile ("1: move.l (%0)+,(%1)+\n\tmove.l (%0)+,(%1)+\n\tmove.l (%0)+,(%1)+\n\tmove.l (%0)+,(%1)+\n"
                          "\tsubq.l #1,%2\n\tbne.s 1b" : "+a"(q), "+a"(p), "+d"(k) : : "memory");
      n &= 15;
    } else {
      k = n >> 3;
      __asm__ volatile ("1: move.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n"
                        "\tmove.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n\tmove.b (%0)+,(%1)+\n"
                        "\tsubq.l #1,%2\n\tbne.s 1b" : "+a"(q), "+a"(p), "+d"(k) : : "memory");
      n &= 7;
    }
  }
  while (n--)
    *p++ = *q++;
  return d;
}

void *memset(void *d, int c, unsigned long n)
{
  uint8_t *p = d;
  while (n--)
    *p++ = c;
  return d;
}
#else
#include <string.h>
#endif

static int streqi(const char *a, const char *b)
{
  for (; *a && *b; a++, b++) {
    int x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y)
      return 0;
  }
  return *a == *b;
}

/* ---- 排他と待ち ---- */

static int enter(void)
{
#ifdef __m68k__
  unsigned short sr;
  __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
  if (sr & 0x2000)
    return 0;
#endif
  return _dos_super(0);
}

static void leave(int ssp)
{
  if (ssp > 0)
    _dos_super(ssp);
}

static void yield(int *ssp)
{
  info.waits++;
  if (*ssp > 0) {
    _dos_super(*ssp);
    _dos_change_pr();
    *ssp = _dos_super(0);
  } else
    _dos_change_pr();
  be->tick();
}

/* ---- リング ---- */

uint32_t ring_put(struct ring *r, const void *p, uint32_t n)
{
  uint32_t tail, first;
  if (n > r->size - r->len)
    n = r->size - r->len;
  tail = r->head + r->len;
  if (tail >= r->size)
    tail -= r->size;
  first = r->size - tail;
  if (first > n)
    first = n;
  memcpy(r->buf + tail, p, first);
  memcpy(r->buf, (const uint8_t *)p + first, n - first);
  r->len += n;
  return n;
}

static uint32_t ring_get(struct ring *r, void *p, uint32_t n)
{
  uint32_t first;
  if (n > r->len)
    n = r->len;
  first = r->size - r->head;
  if (first > n)
    first = n;
  if (p) {
    memcpy(p, r->buf + r->head, first);
    memcpy((uint8_t *)p + first, r->buf, n - first);
  }
  r->head += n;
  if (r->head >= r->size)
    r->head -= r->size;
  r->len -= n;
  if (r->len == 0)
    r->head = 0;
  return n;
}

static int ring_at(struct ring *r, uint32_t i)
{
  i += r->head;
  if (i >= r->size)
    i -= r->size;
  return r->buf[i];
}

/* ---- ソケット表 ---- */

int is_local(uint32_t addr)
{
  return (addr >> 24) == 127 || addr == 0 || (info.own_addr && addr == info.own_addr);
}

int sock_alloc(int type)
{
  int i;
  for (i = 0; i < NSOCK; i++)
    if (ts[i].st == S_FREE) {
      struct ring rx = ts[i].rx;
      memset(&ts[i], 0, sizeof ts[i]);
      ts[i].rx = rx;
      ts[i].rx.head = ts[i].rx.len = 0;
      ts[i].st = S_CLOSED;
      ts[i].peer = ts[i].parent = -1;
      memset(&usock[i], 0, sizeof usock[i]);
      usock[i].refcnt = 1;
      usock[i].type = type;
      usock[i].eol[0] = '\r';
      usock[i].eol[1] = '\n';
      errflag[i] = 0;
      return i;
    }
  return -1;
}

static void sock_free(int i)
{
  struct tsock *t = &ts[i];
  int k;
  /* accept されずに並んでいる子は閉じる (相手には切断に見える) */
  while (t->qn > 0) {
    int c = t->q[--t->qn];
    ts[c].parent = -1;
    sock_free(c);
  }
  if (t->parent >= 0) {
    struct tsock *l = &ts[t->parent];
    for (k = 0; k < l->qn; k++)
      if (l->q[k] == i) {
        for (; k + 1 < l->qn; k++)
          l->q[k] = l->q[k + 1];
        l->qn--;
        break;
      }
  }
  t->st = be->close(i) ? S_ZOMBIE : S_FREE;
  t->rx.head = t->rx.len = 0;
  usock[i].refcnt = 0;
}

static int sk(long fd)
{
  if (fd < SOCKBASE || fd >= SOCKBASE + NSOCK || ts[fd - SOCKBASE].st == S_FREE || ts[fd - SOCKBASE].st == S_ZOMBIE)
    return -1;
  return fd - SOCKBASE;
}

static int fail(int i, const char *err)
{
  ts[i].err = err;
  errflag[i] = 1;
  return -1;
}

static uint16_t ephemeral(void)
{
  int n, i;
  for (n = 0; n < 64512; n++) {
    uint16_t p = next_port++;
    if (next_port == 0)
      next_port = 1024;
    for (i = 0; i < NSOCK; i++)
      if (ts[i].st != S_FREE && ts[i].bound && ts[i].lport == p)
        break;
    if (i == NSOCK)
      return p;
  }
  return 0;
}

const char *state_str(struct tsock *t)
{
  switch (t->st) {
  case S_CLOSED:  return "CLOSED";
  case S_LISTEN:  return "LISTEN";
  case S_SYNSENT: return "SYN SENT";
  case S_SYNRCVD: return "SYN RECEIVED";
  case S_ZOMBIE:  return "DRAINING";    /* close 済み、送信リングを Pico に出し切るまで (tis.x -s だけが見る) */
  case S_EST:
    if (t->dgram)
      return "ESTABLISHED";
    if (t->tx_shut && t->rx_eof)
      return "CLOSING";
    if (t->tx_shut)
      return "FIN WAIT 2";
    if (t->rx_eof)
      return "CLOSE WAIT";
    return "ESTABLISHED";
  }
  return 0;
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t get16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* sockaddr_in に書く。lenp は int* でも socklen_t* (どちらも 4 バイト) */
static void put_addr(void *sa, long *lenp, uint32_t addr, uint16_t port)
{
  uint8_t b[16];
  long n;
  if (!sa || !lenp)
    return;
  memset(b, 0, sizeof b);
  b[2] = port >> 8; b[3] = port;
  b[4] = addr >> 24; b[5] = addr >> 16; b[6] = addr >> 8; b[7] = addr;
  n = *lenp < 16 ? *lenp : 16;
  if (n > 0)
    memcpy(sa, b, n);
  *lenp = 16;
}

/* ---- 各命令 ---- */

static long t_socket(long *a)
{
  int i;
  if (a[0] != 0 || (a[1] != 0 && a[1] != 1))   /* AF_INET = 0, SOCK_STREAM = 0, SOCK_DGRAM = 1 */
    return -1;
  if ((i = sock_alloc(a[1] ? TYPE_UDP : TYPE_TCP)) < 0)
    return -1;
  ts[i].dgram = a[1] == 1;
  return i + SOCKBASE;
}

/* Pico に listen / bind を頼んだら返事を待つ (使用中のポートをここで -1 にする。待たないと、すぐ来た接続が断られる) */
static int wait_bound(int i, int *ssp)
{
  int r;
  while (be->pending && (r = be->pending(i)) != 0) {
    if (r < 0)
      return -1;
    yield(ssp);
    if (ts[i].st == S_FREE || ts[i].st == S_ZOMBIE)
      return -1;
  }
  return 0;
}

static long t_bind(long *a, int *ssp)
{
  int i = sk(a[0]), j;
  const uint8_t *sa = (const uint8_t *)a[1];
  uint32_t addr;
  uint16_t port;
  if (i < 0 || !sa)
    return -1;
  if (ts[i].bound || ts[i].st != S_CLOSED)
    return fail(i, "Invalid argument");
  addr = get32(sa + 4);
  port = get16(sa + 2);
  if (addr && !is_local(addr))
    return fail(i, "Address not available");
  if (port == 0)
    port = ephemeral();
  for (j = 0; j < NSOCK; j++) {
    struct tsock *o = &ts[j];
    /* 接続済みの子 (listen と同じポート) や、閉じたばかりのポートは邪魔しない (TIME WAIT 無し) */
    if (j == i || o->st == S_FREE || !o->bound || o->dgram != ts[i].dgram || o->lport != port)
      continue;
    if (o->st != S_LISTEN && o->st != S_CLOSED && !o->dgram)
      continue;
    if (o->laddr == 0 || addr == 0 || o->laddr == addr)
      return fail(i, "Address already in use");
  }
  ts[i].laddr = addr;
  ts[i].lport = port;
  ts[i].bound = 1;
  if (be->bind) {
    be->bind(i);
    if (wait_bound(i, ssp) < 0) {
      ts[i].bound = 0;
      return fail(i, ts[i].err ? ts[i].err : "Address already in use");
    }
  }
  return 0;
}

static void autobind(int i)
{
  if (!ts[i].bound) {
    ts[i].lport = ephemeral();
    ts[i].bound = 1;
  }
}

static long t_listen(long *a, int *ssp)
{
  int i = sk(a[0]);
  long bl = a[1];
  if (i < 0)
    return -1;
  if (ts[i].dgram || ts[i].st != S_CLOSED)
    return fail(i, "Invalid argument");
  autobind(i);
  if (bl < 0) bl = 0;
  if (bl > MAXQ) bl = MAXQ;
  ts[i].backlog = bl;
  ts[i].st = S_LISTEN;
  if (be->listen(i) < 0 || wait_bound(i, ssp) < 0) {
    if (ts[i].st == S_LISTEN)
      ts[i].st = S_CLOSED;
    return fail(i, ts[i].err ? ts[i].err : "Address already in use");
  }
  return 0;
}

static long t_accept(long *a, int *ssp)
{
  for (;;) {
    int i = sk(a[0]);
    struct tsock *t;
    if (i < 0)
      return -1;
    t = &ts[i];
    if (t->acc0 && (t->st == S_SYNRCVD || t->st == S_EST)) {
      t->acc0 = 0;
      put_addr((void *)a[1], (long *)a[2], t->raddr, t->rport);
      return a[0];
    }
    if (t->st != S_LISTEN)
      return -1;
    if (t->qn > 0) {
      int c = t->q[0], k;
      for (k = 0; k + 1 < t->qn; k++)
        t->q[k] = t->q[k + 1];
      t->qn--;
      ts[c].parent = -1;
      usock[c].noblock = usock[i].noblock;   /* KA9Q と同じく受け継ぐ (libsocket はブロッキングに直す) */
      put_addr((void *)a[1], (long *)a[2], ts[c].raddr, ts[c].rport);
      info.conns++;
      return c + SOCKBASE;
    }
    if (usock[i].noblock)
      return -1;                        /* 状態は LISTEN のまま → libsocket は EAGAIN */
    yield(ssp);
  }
}

static long t_connect(long *a, int *ssp)
{
  int i = sk(a[0]);
  const uint8_t *sa = (const uint8_t *)a[1];
  uint32_t addr;
  uint16_t port;
  if (i < 0 || !sa)
    return -1;
  addr = get32(sa + 4);
  port = get16(sa + 2);
  if (addr == 0)
    addr = 0x7f000001;
  autobind(i);
  if (ts[i].dgram) {
    ts[i].raddr = addr;
    ts[i].rport = port;
    ts[i].st = S_EST;
    return 0;
  }
  if (ts[i].st != S_CLOSED)
    return fail(i, "Already connected");
  if (be->connect(i, addr, port) < 0) {
    ts[i].st = S_CLOSED;
    return -1;                          /* 状態は CLOSED → libsocket は EIO */
  }
  if (usock[i].noblock)
    return -1;                          /* SYN SENT → libsocket は EINPROGRESS */
  for (;;) {
    if ((i = sk(a[0])) < 0)
      return -1;
    if (ts[i].st != S_SYNSENT)
      return ts[i].st == S_EST ? 0 : -1;
    yield(ssp);
  }
}

/* UDP の 1 個を取り出す: [長さ 2][送り元ポート 2][送り元アドレス 4][データ] */
static long dgram_read(int i, uint8_t *buf, uint32_t len, void *from, long *fromlen)
{
  uint8_t h[8];
  uint32_t n, dl;
  ring_get(&ts[i].rx, h, 8);
  dl = get16(h);
  n = dl < len ? dl : len;
  ring_get(&ts[i].rx, buf, n);
  ring_get(&ts[i].rx, 0, dl - n);       /* 入りきらない分は捨てる */
  put_addr(from, fromlen, get32(h + 4), get16(h + 2));
  return n;
}

static long do_read(long fd, uint8_t *buf, uint32_t len, void *from, long *fromlen, int *ssp)
{
  for (;;) {
    int i = sk(fd);
    struct tsock *t;
    if (i < 0)
      return -1;
    t = &ts[i];
    if (len == 0)
      return 0;
    if (t->dgram) {
      if (t->rx.len >= 8)
        return dgram_read(i, buf, len, from, fromlen);
    } else {
      if (t->rx.len > 0) {
        uint32_t n = ring_get(&t->rx, buf, len);
        info.rbytes += n;
        put_addr(from, fromlen, t->raddr, t->rport);
        return n;
      }
      if (t->rx_eof || t->rd_shut)
        return 0;                       /* EOF */
      if (t->st != S_EST && t->st != S_SYNSENT && t->st != S_SYNRCVD)
        return fail(i, "Not connected");
    }
    if (usock[i].noblock)
      return -1;                        /* → EAGAIN */
    yield(ssp);
  }
}

/* ブロッキングは全部受け取るまで待つ (途中で切れたらそこまでの数)。ノンブロッキングは受け取れた分だけ */
static long do_write(long fd, const uint8_t *buf, uint32_t len, const uint8_t *to, int *ssp)
{
  uint32_t done = 0;
  for (;;) {
    int i = sk(fd);
    struct tsock *t;
    uint32_t sp;
    if (i < 0)
      return done ? (long)done : -1;
    t = &ts[i];
    if (t->dgram) {
      autobind(i);
      if (to)
        return be->sendto(i, get32(to + 4), get16(to + 2), buf, len);
      if (t->st != S_EST)
        return fail(i, "Not connected");
      return be->sendto(i, t->raddr, t->rport, buf, len);
    }
    if (done == len)
      return done;
    if (t->st == S_SYNSENT || t->st == S_SYNRCVD) {
      if (usock[i].noblock)
        return -1;
      yield(ssp);
      continue;
    }
    if (t->st != S_EST || t->tx_shut)
      return done ? (long)done : fail(i, "Not connected");
    if (t->reset || (t->peer < 0 && !t->remote))
      return done ? (long)done : fail(i, t->err ? t->err : "Connection reset");
    if ((sp = be->sendspace(i)) > 0) {
      uint32_t n = be->send(i, buf + done, len - done < sp ? len - done : sp);
      info.wbytes += n;
      done += n;
      if (usock[i].noblock || done == len)
        return done;
      continue;
    }
    if (usock[i].noblock)
      return done ? (long)done : -1;    /* 状態は ESTABLISHED → EAGAIN */
    yield(ssp);
  }
}

static long t_close(long fd)
{
  int i = sk(fd);
  /* 閉じ済みの番号を閉じ直すのは 0 にする。dropbear は sock_in と sock_out (同じ fd) を別々に閉じ、
     2 回目が -1 だと libsocket が EIO にして "Error closing fd" で終わる (EBADF なら黙る) */
  if (i < 0)
    return fd >= SOCKBASE && fd < SOCKBASE + NSOCK ? 0 : -1;
  if (--usock[i].refcnt > 0)
    return 0;
  sock_free(i);
  return 0;
}

static long t_socklen(long *a)
{
  int i = sk(a[0]);
  struct tsock *t;
  if (i < 0)
    return -1;
  t = &ts[i];
  if (a[1] == 0) {
    if (t->rx.len > 0)
      return t->rx.len;
    if (t->st == S_LISTEN)
      return t->qn;                     /* accept を待っている数 */
    if (t->dgram || t->st == S_LISTEN || t->st == S_SYNSENT || t->st == S_SYNRCVD)
      return 0;
    if (t->st != S_EST || t->rx_eof || t->rd_shut)
      return -1;                        /* EOF / 未接続: poll は POLLERR、続く read が 0 */
    return 0;
  }
  if (t->dgram || t->st == S_LISTEN || t->st == S_SYNSENT || t->st == S_SYNRCVD)
    return 0;
  if (t->st != S_EST || t->tx_shut || t->reset || (t->peer < 0 && !t->remote))
    return -1;
  if (be->sendspace(i) > 0)
    return 0;
  return t->peer >= 0 && ts[t->peer].rx.len ? (long)ts[t->peer].rx.len : 1;   /* 相手のリングが一杯: POLLOUT にしない */
}

static long t_sockname(long *a, int peer)
{
  int i = sk(a[0]);
  struct tsock *t;
  if (i < 0)
    return -1;
  t = &ts[i];
  if (peer) {
    if (t->st != S_EST && t->st != S_SYNRCVD && t->st != S_SYNSENT)
      return fail(i, "Not connected");
    put_addr((void *)a[1], (long *)a[2], t->raddr, t->rport);
  } else
    put_addr((void *)a[1], (long *)a[2], t->laddr, t->lport);
  return 0;
}

static long t_shutdown(long *a)
{
  int i = sk(a[0]);
  if (i < 0)
    return -1;
  if (a[1] == 0 || a[1] == 2)
    ts[i].rd_shut = 1;
  if (a[1] == 1 || a[1] == 2)
    ts[i].tx_shut = 1;
  be->shutdown(i, a[1]);
  return 0;
}

/* 1 文字。ASCII モードでは CR を捨てる。EOF / エラーは -1 */
static long t_recvchar(long fd, int *ssp)
{
  for (;;) {
    int i = sk(fd), c;
    if (i < 0)
      return -1;
    if (ts[i].rx.len > 0) {
      c = ring_at(&ts[i].rx, 0);
      ring_get(&ts[i].rx, 0, 1);
      if (c == '\r' && ts[i].ascii)
        continue;
      return c;
    }
    if (ts[i].rx_eof || ts[i].rd_shut || ts[i].st != S_EST)
      return -1;
    if (usock[i].noblock)
      return -1;
    yield(ssp);
  }
}

/* 改行まで (改行を含む) か len バイトまで。ASCII モードでは CR を捨てる。EOF で何も無ければ -1 */
static long t_recvline(long *a, int *ssp)
{
  uint8_t *buf = (uint8_t *)a[1];
  uint32_t len = a[2];
  for (;;) {
    int i = sk(a[0]);
    struct tsock *t;
    uint32_t n, k, m;
    if (i < 0)
      return -1;
    t = &ts[i];
    n = t->rx.len < len ? t->rx.len : len;
    for (k = 0; k < n; k++)
      if (ring_at(&t->rx, k) == '\n')
        break;
    if (k < n || t->rx.len >= len || (t->rx.len > 0 && (t->rx_eof || t->st != S_EST))) {
      if (k < n)
        n = k + 1;
      for (k = m = 0; k < n; k++) {
        int c = ring_at(&t->rx, k);
        if (c == '\r' && t->ascii)
          continue;
        buf[m++] = c;
      }
      ring_get(&t->rx, 0, n);
      info.rbytes += n;
      return m;
    }
    if (t->rx_eof || t->rd_shut || (t->st != S_EST && t->st != S_SYNRCVD && t->st != S_SYNSENT))
      return -1;
    if (usock[i].noblock)
      return -1;
    yield(ssp);
  }
}

static long t_sockmode(long *a)
{
  int i = sk(a[0]), prev;
  if (i < 0)
    return -1;
  prev = ts[i].ascii;
  if (a[1] == 0 || a[1] == 1)
    ts[i].ascii = a[1];
  return prev;
}

/* "a.b.c.d:port" */
char *fmt_addr(char *p, uint32_t addr, uint16_t port)
{
  int k, sh;
  for (sh = 24; sh >= 0; sh -= 8) {
    int v = (addr >> sh) & 255;
    char d[3];
    k = 0;
    do d[k++] = '0' + v % 10; while ((v /= 10) > 0);
    while (k > 0) *p++ = d[--k];
    *p++ = sh ? '.' : ':';
  }
  {
    char d[5];
    unsigned v = port;
    k = 0;
    do d[k++] = '0' + v % 10; while ((v /= 10) > 0);
    while (k > 0) *p++ = d[--k];
  }
  *p = 0;
  return p;
}

static long t_psocket(const uint8_t *sa)
{
  static char s[24];
  if (!sa)
    return 0;
  fmt_addr(s, get32(sa + 4), get16(sa + 2));
  return (long)s;
}

/* ---- 名前 ---- */

static char *lo_aliases[] = { "loopback", 0 };
static char *no_alias_h[] = { 0 };
static uint32_t lo_addr = 0x7f000001;
static char *lo_addrs[] = { (char *)&lo_addr, 0 };
static struct { char *h_name; char **h_aliases; int h_addrtype, h_length; char **h_addr_list; } lo_host = {
  "localhost", lo_aliases, 0, 4, lo_addrs,
};

/* Pico で引いた名前 (呼ぶたびに上書きする。xip.x も静的な領域を返す) */
static char net_name[256];
static uint32_t net_addr;
static char *net_addrs[] = { (char *)&net_addr, 0 };
static struct { char *h_name; char **h_aliases; int h_addrtype, h_length; char **h_addr_list; } net_host = {
  net_name, no_alias_h, 0, 4, net_addrs,
};

static long t_gethostbyname(const char *name, int *ssp)
{
  int q, r, n;
  uint32_t addr;
  if (!name)
    return 0;
  if (streqi(name, "localhost") || streqi(name, "loopback") || streqi(name, "localhost.localdomain"))
    return (long)&lo_host;
  if (!be->resolve_start)
    return 0;
  for (n = 0; name[n] && n < 255; n++)
    net_name[n] = name[n];
  net_name[n] = 0;
  /* 問い合わせの表が一杯なら空くまで待つ。答えは npd.x のスレッドが運んでくる */
  while ((q = be->resolve_start(net_name)) == -1)
    yield(ssp);
  if (q < 0)
    return 0;                           /* Pico に頼めない */
  while ((r = be->resolve_poll(q, net_name, &addr)) == 0)
    yield(ssp);
  if (r < 0)
    return 0;
  for (n = 0; name[n] && n < 255; n++)  /* 待っている間に別のスレッドが net_name を使ったかもしれない */
    net_name[n] = name[n];
  net_name[n] = 0;
  net_addr = addr;
  return (long)&net_host;
}

static long t_gethostbyaddr(long *a)
{
  const uint8_t *p = (const uint8_t *)a[0];
  if (p && a[1] == 4 && p[0] == 127)
    return (long)&lo_host;
  return 0;
}

static struct serv { char *s_name; char **s_aliases; int s_port; char *s_proto; } servs[] = {
  { "ftp", 0, 21, "tcp" }, { "ssh", 0, 22, "tcp" }, { "telnet", 0, 23, "tcp" }, { "smtp", 0, 25, "tcp" },
  { "domain", 0, 53, "udp" }, { "http", 0, 80, "tcp" }, { "rcmd", 0, 2323, "tcp" },
};
static char *no_alias[] = { 0 };

static long t_getserv(long *a, int byport)
{
  unsigned k;
  const char *proto = (const char *)a[1];
  for (k = 0; k < sizeof servs / sizeof servs[0]; k++) {
    struct serv *s = &servs[k];
    s->s_aliases = no_alias;
    if (proto && !streqi(proto, s->s_proto))
      continue;
    if (byport ? s->s_port == a[0] : (a[0] && streqi((const char *)a[0], s->s_name)))
      return (long)s;
  }
  return 0;
}

static struct proto { char *p_name; char **p_aliases; int p_proto; } protos[] = {
  { "ip", no_alias, 0 }, { "icmp", no_alias, 1 }, { "tcp", no_alias, 6 }, { "udp", no_alias, 17 },
};

static long t_getproto(long v, int bynum)
{
  unsigned k;
  for (k = 0; k < sizeof protos / sizeof protos[0]; k++)
    if (bynum ? protos[k].p_proto == v : (v && streqi((const char *)v, protos[k].p_name)))
      return (long)&protos[k];
  return 0;
}

/* 常駐の前に表を空にする (Human68k は BSS を 0 にしない) */
void tis_init(void)
{
  memset(usock, 0, sizeof usock);
  memset(ts, 0, sizeof ts);
  memset(errflag, 0, sizeof errflag);
}

/* ---- 入口 ---- */

static long dispatch(long cmd, long *a, int *ssp)
{
  int i;
  switch (cmd) {
  case _TI_get_version:   return 0x0100;
  case _TI_socket:        return t_socket(a);
  case _TI_bind:          return t_bind(a, ssp);
  case _TI_listen:        return t_listen(a, ssp);
  case _TI_accept:        return t_accept(a, ssp);
  case _TI_connect:       return t_connect(a, ssp);
  case _TI_read_s:        return do_read(a[0], (uint8_t *)a[1], a[2], 0, 0, ssp);
  case _TI_recvfrom:      return do_read(a[0], (uint8_t *)a[1], a[2], (void *)a[4], (long *)a[5], ssp);
  case _TI_write_s:       return do_write(a[0], (const uint8_t *)a[1], a[2], 0, ssp);
  case _TI_sendto:        return do_write(a[0], (const uint8_t *)a[1], a[2], (const uint8_t *)a[4], ssp);
  case _TI_close_s:       return t_close((long)a);
  case _TI_socklen:       return t_socklen(a);
  case _TI_getsockname:   return t_sockname(a, 0);
  case _TI_getpeername:   return t_sockname(a, 1);
  case _TI_shutdown:      return t_shutdown(a);
  case _TI_usesock:
    if ((i = sk((long)a)) < 0) return -1;
    usock[i].refcnt++;
    return 0;
  case _TI_sockkick:
  case _TI_usflush:       return sk((long)a) < 0 ? -1 : 0;
  case _TI_recvline:      return t_recvline(a, ssp);
  case _TI_recvchar:
  case _TI_rrecvchar:     return t_recvchar((long)a, ssp);
  case _TI_seteol:
    if ((i = sk(a[0])) < 0) return -1;
    {
      const char *e = (const char *)a[1];
      usock[i].eol[0] = e && e[0] ? e[0] : 0;
      usock[i].eol[1] = e && e[0] ? e[1] : 0;
    }
    return 0;
  case _TI_setflush:
    if ((i = sk(a[0])) < 0) return -1;
    usock[i].flush = a[1];
    return 0;
  case _TI_sockmode:      return t_sockmode(a);
  case _TI_psocket:       return t_psocket((const uint8_t *)a);
  case _TI_sockerr:
    if ((i = sk((long)a)) < 0) return 0;
    return (long)(ts[i].err ? ts[i].err : "");
  case _TI_sockstate:
    if ((i = sk((long)a)) < 0) return 0;
    if (errflag[i]) {                   /* 直前の -1 が本当のエラー: libsocket に EIO と分からせる */
      errflag[i] = 0;
      return 0;
    }
    return (long)state_str(&ts[i]);
  case _TI_sock_top:      return (long)usock;
  case _TI_gethostbyname: return t_gethostbyname((const char *)a, ssp);
  case _TI_gethostbyaddr: return t_gethostbyaddr(a);
  case _TI_getservbyname: return t_getserv(a, 0);
  case _TI_getservbyport: return t_getserv(a, 1);
  case _TI_getprotobyname:   return t_getproto((long)a, 0);
  case _TI_getprotobynumber: return t_getproto((long)a, 1);
  case TIS_INFO:          return (long)&info;
  case TIS_NP_OUT:
    if (!be->np_out) return 0;
    {
      int urg = 0;
      long n = be->np_out((uint8_t *)a[0], a[1], &urg);
      a[2] = urg;
      return n;
    }
  case TIS_NP_IN:         return be->np_in ? be->np_in((const uint8_t *)a[1], a[0]) : 0;
  }
  /* ループバックに無い命令 (ARP / iface / 経路 / DNS / MIB / RIP)。表や構造体を返すものは NULL、
     ほかは -1。FAST_API の問い合わせ (-1) も -1 (trap ではなく jsr で呼んでもらう) */
  switch (cmd) {
  case _TI_search_arp_table: case _TI_get_arp_table_top: case _TI_get_iface_list: case _TI_get_new_iface:
  case _TI_rt_top: case _TI_rt_lookup: case _TI_rt_lookupb: case _TI_dns_get: case _TI_get_domain_name:
  case _TI_get_MIB: case _TI_ntoa_sock: case _TI_getnetbyname: case _TI_getnetbyaddr:
    return 0;
  }
  return -1;
}

long ti_dispatch(long cmd, long *a)
{
  int ssp = enter();
  long r;
  info.calls++;
  be->tick();
  r = dispatch(cmd, a, &ssp);
  /* 記録: ソケットを引数に取る命令だけ。socklen / sockstate が 0 / 状態を返すだけのものは除く */
  if (cmd >= _TI_socket && cmd <= _TI_sockstate && cmd != _TI_psocket) {
    long fd = (cmd == _TI_socket) ? r : (cmd == _TI_close_s || cmd == _TI_sockstate || cmd == _TI_sockerr ||
              cmd == _TI_usflush || cmd == _TI_sockkick || cmd == _TI_usesock || cmd == _TI_recvchar ||
              cmd == _TI_rrecvchar) ? (long)a : a[0];
    if (!((cmd == _TI_socklen && r == 0) || (cmd == _TI_sockstate && r != 0))) {
      int k = info.tpos++ % TRACE;
      info.trace[k].cmd = cmd;
      info.trace[k].fd = fd;
      info.trace[k].len = (cmd == _TI_read_s || cmd == _TI_write_s || cmd == _TI_recvfrom || cmd == _TI_sendto ||
                           cmd == _TI_recvline || cmd == _TI_socklen || cmd == _TI_shutdown || cmd == _TI_sockmode) ? a[cmd == _TI_socklen || cmd == _TI_shutdown || cmd == _TI_sockmode ? 1 : 2] : 0;
      info.trace[k].ret = r;
    }
  }
  leave(ssp);
  return r;
}
