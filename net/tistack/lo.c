/*
 * lo.c -- tis.x のループバック backend
 *
 * 同じ X68000 の中のソケット同士をつなぐ。送信は相手の受信リングに直接書く (送信バッファ無し)。
 * connect は相手を決めて SYN SENT / SYN RECEIVED にするだけで、ESTABLISHED への昇格は次の tick で行う
 * (ノンブロッキング connect → poll(POLLOUT) の非同期の道を、Pico 版と同じように通すため)。
 */
#include "be.h"

static int match_addr(uint32_t bound, uint32_t addr)
{
  return bound == 0 || bound == addr || ((bound >> 24) == 127 && (addr >> 24) == 127);
}

static void lo_tick(void)
{
  int i;
  for (i = 0; i < NSOCK; i++) {
    struct tsock *t = &ts[i];
    if (t->st != S_SYNSENT || t->remote)   /* Pico 側の connect は Pico の知らせで進む */
      continue;
    if (t->peer < 0) {                  /* 受け付け待ちのうちに listen 側が閉じた */
      t->st = S_CLOSED;
      t->err = "Connection refused";
      continue;
    }
    t->st = S_EST;
    if (ts[t->peer].st == S_SYNRCVD)
      ts[t->peer].st = S_EST;
  }
}

static int lo_listen(int s)
{
  (void)s;
  return 0;
}

static int lo_connect(int s, uint32_t addr, uint16_t port)
{
  struct tsock *t = &ts[s], *l;
  int i, c;
  if (!is_local(addr)) {
    t->err = "Network unreachable";
    return -1;
  }
  for (i = 0; i < NSOCK; i++)
    if (ts[i].st == S_LISTEN && !ts[i].dgram && ts[i].lport == port && match_addr(ts[i].laddr, addr))
      break;
  if (i == NSOCK) {
    t->err = "Connection refused";
    return -1;
  }
  l = &ts[i];
  if (t->laddr == 0)
    t->laddr = addr;
  if (l->backlog == 0) {
    /* backlog 0 (rcmd.x): listen したソケット自身が接続になる。accept は同じ番号を返す */
    c = i;
    l->acc0 = 1;
  } else {
    if (l->qn >= l->backlog || (c = sock_alloc(TYPE_TCP)) < 0) {
      t->err = "Connection refused";
      return -1;
    }
    ts[c].lport = port;
    ts[c].bound = 1;
    ts[c].parent = i;
    l->q[l->qn++] = c;
  }
  ts[c].st = S_SYNRCVD;
  ts[c].laddr = addr;
  ts[c].raddr = t->laddr;
  ts[c].rport = t->lport;
  ts[c].peer = s;
  t->st = S_SYNSENT;
  t->peer = c;
  t->raddr = addr;
  t->rport = port;
  return 0;
}

static uint32_t lo_sendspace(int s)
{
  int p = ts[s].peer;
  if (p < 0)
    return 0;
  return ts[p].rx.size - ts[p].rx.len;
}

static uint32_t lo_send(int s, const void *buf, uint32_t n)
{
  int p = ts[s].peer;
  if (p < 0)
    return 0;
  if (ts[p].rd_shut)                    /* 相手が受信を閉じている: 捨てて受け取ったことにする */
    return n;
  return ring_put(&ts[p].rx, buf, n);
}

/* UDP: 受け手のリングに [長さ 2][送り元ポート 2][送り元アドレス 4][データ] を積む。入らなければ捨てる */
static int lo_sendto(int s, uint32_t addr, uint16_t port, const void *buf, uint32_t n)
{
  int i;
  uint8_t h[8];
  struct tsock *t = &ts[s];
  if (!is_local(addr)) {
    t->err = "Network unreachable";
    return -1;
  }
  if (n > 0xffff)
    return -1;
  for (i = 0; i < NSOCK; i++)
    if (ts[i].st != S_FREE && ts[i].st != S_ZOMBIE && ts[i].dgram && ts[i].bound && ts[i].lport == port && match_addr(ts[i].laddr, addr))
      break;
  if (i == NSOCK)
    return n;
  if (ts[i].rx.size - ts[i].rx.len < n + 8)
    return n;
  h[0] = n >> 8; h[1] = n;
  h[2] = t->lport >> 8; h[3] = t->lport;
  {
    uint32_t a = t->laddr ? t->laddr : addr;
    h[4] = a >> 24; h[5] = a >> 16; h[6] = a >> 8; h[7] = a;
  }
  ring_put(&ts[i].rx, h, 8);
  ring_put(&ts[i].rx, buf, n);
  return n;
}

static void lo_shutdown(int s, int how)
{
  int p = ts[s].peer;
  if (p >= 0 && how != 0)
    ts[p].rx_eof = 1;
}

static int lo_close(int s)
{
  int p = ts[s].peer;
  if (p >= 0) {
    ts[p].peer = -1;
    ts[p].rx_eof = 1;
    ts[p].reset = 1;
  }
  return 0;
}

const struct backend be_loop = {
  "loopback", lo_tick, lo_listen, lo_connect, lo_sendspace, lo_send, lo_sendto, lo_shutdown, lo_close,
  0, 0, 0, 0, 0, 0,
};
