/*
 * pico.c -- tis.x の Pico backend: TCP / UDP / DNS を Pico (lwIP) に任せる
 *
 * ソケット操作を TSP の要求メッセージ (tsp.h) にして溜め、npd.x のスレッドが交換のたびに取りに来る
 * (TIS_NP_OUT)。Pico の応答 (接続できた・データ・相手の FIN・エラー・送ってよい量) は TIS_NP_IN で渡され、
 * API 層の状態 (ts[]) と受信リングに写す。backend は待たない: ブロッキングの待ちは tis.c が _CHANGE_PR しながら
 * 状態を見直すだけで、その間に npd.x のスレッドが交換する。
 *
 * 127.0.0.0/8 と自分のアドレス (-a) はループバック (lo.c) のまま。listen はどちらからの接続も受ける。
 *
 * Z → Pico のデータは、ソケットごとの送信リング (ps[].tx) に溜めて、Pico が CREDIT で許した分だけ送る。
 * Pico → Z のデータは、受信リングの空きを WINDOW で知らせ、Pico はその分だけ送る。どちらも端から端までの
 * フロー制御で、1 本のソケットが交換を占めないよう、データはソケットを順に回って分ける。
 */
#include "be.h"
#include "tsp.h"

#ifdef __m68k__
#define __IOCS_INLINE__
#include <x68k/iocs.h>
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
static uint32_t now_cs(void) { return _iocs_ontime().sec; }   /* 1/100 秒 (0 時に戻るが、差しか使わない) */
#else
#include <string.h>
uint32_t pico_host_clock_cs(void);      /* 試験 (picosim.c) が仮想の時計を出す */
static uint32_t now_cs(void) { return pico_host_clock_cs(); }
#endif

/* Pico が TSP を話さない (今のファーム) / npd.x が動いていないときに、待ちを止める時間 (1/100 秒) */
#define NOTSP_CS    300                 /* 要求を送り直し続けて答えが無い: Pico は 0xA6 を知らない */
#define NONPD_CS    500                 /* 送るものがあるのに npd.x が取りに来ない */

/* t からの経過 (1/100 秒)。ONTIME は 0 時に 8,640,000 から 0 に戻る */
static uint32_t since(uint32_t t)
{
  uint32_t n = now_cs();
  return n >= t ? n - t : n + 8640000 - t;
}

#define CQSIZE  8192                    /* 制御メッセージの待ち行列 (CONNECT / LISTEN / ADOPT / SENDTO / RESOLVE) */

struct psl {
  uint8_t gen;                          /* この番号を Pico 側で使い直すたびに増やす */
  uint8_t opened;                       /* Pico 側のソケットがある (開く要求を積んだ) */
  uint8_t dead;                         /* Pico 側のソケットは無くなった (ERROR を受けた) */
  uint8_t closing;                      /* close 済み: 送信リングを出し切ったら CLOSE を積んで S_FREE に */
  uint8_t shut;                         /* 1 = SHUT を積む (送信リングを出し切ったら)、2 = 積んだ */
  uint8_t udpbound;                     /* UDP: UDPOPEN を積んだ */
  uint8_t waitbound;                    /* LISTEN / UDPOPEN の返事 (BOUND) 待ち: 1 待つ / 2 断られた */
  uint8_t busy;                         /* 要求を組み始めたとき送信リングにデータがあった */
  uint16_t ctl;                         /* 待ち行列に残っているこのソケットの制御メッセージの数 */
  uint32_t credit;                      /* いま Pico に送ってよいバイト数 */
  uint32_t pview;                       /* Pico が知っている Z の受信リングの空き */
  uint32_t pwant;                       /* Pico に知らせた送信リングの残り (CREDIT を決める手がかり) */
  struct ring tx;
};
static struct psl ps[NSOCK];

static struct {
  uint8_t state;                        /* 0 空き / 1 積んだ / 2 答えあり */
  uint8_t code;
  uint32_t addr;
} qt[TSP_NQUERY];

static uint8_t cq[CQSIZE];              /* 制御メッセージ (そのままの形で並べる) */
static uint32_t cqlen;
static uint8_t req[TSP_REQ_MAX];        /* 送ったが答えをまだ受けていない要求 (送り直す) */
static uint32_t reqlen;
static uint16_t zseq;
static uint32_t zepoch, pepoch;
static uint8_t pknown, zreset;
static uint8_t rr;                      /* データを分けるときの始まりの番号 (交換ごとに回す) */
static uint8_t notsp;                   /* Pico に TSP が無い (答えが来ない)。この間は新しい Pico 側のソケットを断る */
static uint32_t last_ok, last_np;       /* 最後に応答を受けた / npd.x が要求を取りに来た時刻 (1/100 秒) */

struct pico_stat pstat;

/* 名前で引いたアドレスの控え (CONNECT に名前を添える。TLS の SNI / IPv6 のために Pico が名前を知る) */
#define NCACHE  4
static struct { uint32_t addr; char name[TSP_NAME_MAX + 1]; } nc[NCACHE];
static uint8_t ncpos;

/* そのソケットのデータより先に Pico に届かなければならない制御メッセージか (開く・引き取る・UDP) */
static int ordered(int type)
{
  return type != TSP_C_RESOLVE && type != TSP_C_REJECT;
}

static uint8_t *cq_msg(int type, int slot, int gen, unsigned len)
{
  uint8_t *p;
  if (cqlen + tsp_msize(len) > CQSIZE)
    return 0;
  p = tsp_msg(cq + cqlen, type, slot, gen, len);
  if (len & 1)
    p[len] = 0;
  cqlen += tsp_msize(len);
  if (ordered(type))
    ps[slot].ctl++;
  return p;
}

static int remote(int s)
{
  return ts[s].remote && ts[s].peer < 0;
}

/* ---- Pico 側のソケットが無くなったとき ---- */

static void dead(int s, int code)
{
  struct tsock *t = &ts[s];
  struct psl *q = &ps[s];
  q->dead = 1;
  q->credit = 0;
  if (q->waitbound)
    q->waitbound = 2;
  q->tx.head = q->tx.len = 0;
  if (t->st == S_ZOMBIE) {
    t->st = S_FREE;
    t->remote = 0;
    return;
  }
  t->err = tsp_errstr(code);
  if (t->dgram) {
    q->opened = q->udpbound = 0;        /* 次の sendto で開き直す */
    q->dead = 0;
    return;
  }
  if (t->st == S_SYNSENT || t->st == S_LISTEN)
    t->st = S_CLOSED;
  else {
    t->reset = 1;
    t->rx_eof = 1;
  }
}

/* Pico が入れ替わった (再起動): Pico 側のソケットを全部無くしたことにする */
static void netdown(void)
{
  int i;
  for (i = 0; i < NSOCK; i++) {
    ps[i].ctl = 0;
    if (ts[i].st != S_FREE && ts[i].remote && (ps[i].opened || ts[i].st == S_ZOMBIE))
      dead(i, TSP_ERR_NETDOWN);
  }
  cqlen = 0;
  for (i = 0; i < TSP_NQUERY; i++)
    if (qt[i].state == 1) {
      qt[i].state = 2;
      qt[i].code = TSP_ERR_NETDOWN;
    }
  pstat.netdown++;
}

/* ---- backend の口 ---- */

/* Pico から答えが来ない: 待っているものを全部 "Network is down" で終わらせ、以後の新しいソケットは断る
   (応答が来れば戻る)。今のファームは 0xA6 を知らずに捨てるので、ここに来る */
static void no_tsp(void)
{
  if (notsp)
    return;
  notsp = 1;
  netdown();
}

static void pi_tick(void)
{
  be_loop.tick();
  if (!notsp && (reqlen || cqlen) && since(last_np) > NONPD_CS)
    no_tsp();                           /* npd.x が動いていない */
}

static void open_slot(int s)
{
  struct psl *q = &ps[s];
  struct ring tx = q->tx;
  uint8_t gen = q->gen + 1;
  memset(q, 0, sizeof *q);
  q->tx = tx;
  q->tx.head = q->tx.len = 0;
  q->gen = gen;
  q->opened = 1;
  ts[s].remote = 1;
}

static int pi_listen(int s)
{
  struct tsock *t = &ts[s];
  uint8_t *p;
  be_loop.listen(s);
  if ((t->laddr >> 24) == 127 || notsp)
    return 0;                           /* (Pico に TSP が無ければループバックだけで受ける) */
  open_slot(s);
  if (!(p = cq_msg(TSP_C_LISTEN, s, ps[s].gen, 8)))
    return 0;                           /* 待ち行列が一杯: ループバックだけで受ける */
  ps[s].waitbound = 1;
  p[0] = t->backlog;
  p[1] = 0;
  tsp_put16(p + 2, t->lport);
  tsp_put32(p + 4, t->laddr);
  return 0;
}

static int pi_connect(int s, uint32_t addr, uint16_t port)
{
  struct tsock *t = &ts[s];
  uint8_t *p;
  int k, nl = 0;
  const char *name = 0;
  if (is_local(addr))
    return be_loop.connect(s, addr, port);
  if (notsp) {
    t->err = tsp_errstr(TSP_ERR_NETDOWN);
    return -1;
  }
  for (k = 0; k < NCACHE; k++)
    if (nc[k].addr == addr && nc[k].name[0]) {
      name = nc[k].name;
      while (name[nl])
        nl++;
      break;
    }
  open_slot(s);
  if (!(p = cq_msg(TSP_C_CONNECT, s, ps[s].gen, 10 + nl))) {
    t->err = "No buffer space";
    t->remote = 0;
    ps[s].opened = 0;
    return -1;
  }
  p[0] = TSP_PROTO_TCP;
  p[1] = name ? TSP_CF_NAME : 0;
  tsp_put16(p + 2, t->lport);
  tsp_put16(p + 4, port);
  tsp_put32(p + 6, addr);
  if (nl)
    memcpy(p + 10, name, nl);
  t->st = S_SYNSENT;
  t->raddr = addr;
  t->rport = port;
  pstat.connects++;
  return 0;
}

static uint32_t pi_sendspace(int s)
{
  if (!remote(s))
    return be_loop.sendspace(s);
  if (ps[s].dead)
    return 0;
  return ps[s].tx.size - ps[s].tx.len;
}

static uint32_t pi_send(int s, const void *p, uint32_t n)
{
  if (!remote(s))
    return be_loop.send(s, p, n);
  if (ps[s].dead)
    return 0;
  return ring_put(&ps[s].tx, p, n);
}

static int udp_open(int s)
{
  struct tsock *t = &ts[s];
  uint8_t *p;
  if (ps[s].udpbound && t->remote)
    return 0;
  if (notsp) {
    t->err = tsp_errstr(TSP_ERR_NETDOWN);
    return -1;
  }
  if (!t->remote || !ps[s].opened)
    open_slot(s);
  if (!(p = cq_msg(TSP_C_UDPOPEN, s, ps[s].gen, 8)))
    return -1;
  p[0] = p[1] = 0;
  tsp_put16(p + 2, t->lport);
  tsp_put32(p + 4, t->laddr);
  ps[s].udpbound = 1;
  ps[s].waitbound = 1;
  return 0;
}

static int pi_bind(int s)
{
  struct tsock *t = &ts[s];
  if (t->dgram && (t->laddr >> 24) != 127)
    return udp_open(s);
  return 0;
}

static int pi_sendto(int s, uint32_t addr, uint16_t port, const void *buf, uint32_t n)
{
  struct tsock *t = &ts[s];
  uint8_t *p;
  if (is_local(addr))
    return be_loop.sendto(s, addr, port, buf, n);
  if (n > 1472 || udp_open(s) < 0 || !(p = cq_msg(TSP_C_SENDTO, s, ps[s].gen, 6 + n))) {
    t->err = "No buffer space";
    return -1;
  }
  tsp_put32(p, addr);
  tsp_put16(p + 4, port);
  memcpy(p + 6, buf, n);
  pstat.dgrams_out++;
  return n;
}

static void pi_shutdown(int s, int how)
{
  if (!remote(s)) {
    be_loop.shutdown(s, how);
    return;
  }
  if (how != 0 && !ps[s].shut)
    ps[s].shut = 1;
}

/* 戻り値 1: 番号はまだ使う (送信リングを出し切って CLOSE を積むまで S_ZOMBIE) */
static int pi_close(int s)
{
  struct psl *q = &ps[s];
  if (!ts[s].remote || ts[s].peer >= 0)
    return be_loop.close(s);
  if (!q->opened || q->dead) {
    ts[s].remote = 0;
    return 0;
  }
  q->closing = 1;
  return 1;
}

/* listen / UDP の bind が Pico で済んだか: 1 まだ / 0 済んだ (かループバックだけ) / -1 断られた */
static int pi_pending(int s)
{
  struct psl *q = &ps[s];
  if (!ts[s].remote || q->waitbound == 0)
    return 0;
  if (q->waitbound == 2) {
    q->waitbound = 0;
    return -1;
  }
  return 1;
}

/* 名前を引く。戻り値: 問い合わせ番号 / -1 (表が一杯、待てば空く) / -2 (Pico に TSP が無い) */
static int pi_resolve_start(const char *name)
{
  int k, n = 0;
  uint8_t *p;
  if (notsp)
    return -2;
  while (name[n] && n < TSP_NAME_MAX)
    n++;
  for (k = 0; k < TSP_NQUERY; k++)
    if (qt[k].state == 0)
      break;
  if (k == TSP_NQUERY || !(p = cq_msg(TSP_C_RESOLVE, k, 0, n)))   /* slot 欄 = 問い合わせ番号 */
    return -1;
  memcpy(p, name, n);
  qt[k].state = 1;
  return k;
}

/* 戻り値: 1 引けた (*addr) / 0 まだ / -1 引けない。1 と -1 で番号を返す */
static int pi_resolve_poll(int k, const char *name, uint32_t *addr)
{
  int n;
  if (qt[k].state != 2)
    return 0;
  qt[k].state = 0;
  if (qt[k].code != TSP_OK)
    return -1;
  *addr = qt[k].addr;
  for (n = 0; name[n] && n < TSP_NAME_MAX; n++)
    nc[ncpos].name[n] = name[n];
  nc[ncpos].name[n] = 0;
  nc[ncpos].addr = *addr;
  ncpos = (ncpos + 1) % NCACHE;
  return 1;
}

/* ---- 要求を組む (npd.x の交換ごと) ---- */

/* WINDOW を送るか: 受信リングが 1/4 (最大 1KB) 以上空いた / 空になった、または送りたい量が CREDIT を超えた・無くなった */
static int window_due(int i)
{
  struct tsock *t = &ts[i];
  struct psl *q = &ps[i];
  uint32_t fr = t->rx.size - t->rx.len, th = t->rx.size / 4 < 1024 ? t->rx.size / 4 : 1024;
  if (t->st == S_FREE || t->st == S_ZOMBIE || !t->remote || !q->opened || q->dead || q->ctl || t->st == S_LISTEN)
    return 0;
  if (fr > q->pview && (fr - q->pview >= th || fr == t->rx.size))
    return 1;
  if (t->dgram)
    return 0;
  /* 送りたい量 (want) は「送信リングが空でない間は送信リング全部」: 書き続けるアプリには毎回それだけの
     CREDIT が要る。空なら 0 (Pico は床の分だけ残す) */
  return (q->busy || q->tx.len ? q->tx.size : 0) != q->pwant;
}

static int urgent(void)
{
  int i;
  if (cqlen || zreset)
    return 1;
  for (i = 0; i < NSOCK; i++) {
    struct psl *q = &ps[i];
    struct tsock *t = &ts[i];
    if (t->st == S_FREE || !t->remote || !q->opened || q->dead || q->ctl)
      continue;
    if (q->tx.len && q->credit)
      return 1;
    if (q->tx.len == 0 && (q->closing || q->shut == 1))
      return 1;
    if (window_due(i))
      return 1;
  }
  return 0;
}

static uint32_t build(uint32_t max)
{
  uint8_t *p = req + TSP_HDR;
  uint32_t room = max - TSP_HDR, k, n;
  int i, j, ready;

  req[0] = TSP_MARK;
  req[1] = TSP_VERSION;
  req[2] = zreset ? TSP_ZF_RESET : 0;
  req[3] = 0;
  tsp_put16(req + 4, ++zseq);
  tsp_put16(req + 6, 0);
  tsp_put32(req + 8, zepoch);
  zreset = 0;
  for (i = 0; i < NSOCK; i++)
    ps[i].busy = ps[i].tx.len != 0;

  /* 1. 制御メッセージ (積んだ順) */
  for (k = 0; k < cqlen; ) {
    uint32_t m = tsp_msize(tsp_get16(cq + k + 4));
    if (m > room)
      break;
    memcpy(p, cq + k, m);
    if (ordered(cq[k]) && ps[cq[k + 1]].ctl)
      ps[cq[k + 1]].ctl--;
    p += m;
    room -= m;
    k += m;
  }
  if (k) {
    memcpy(cq, cq + k, cqlen - k);
    cqlen -= k;
  }

  /* 3. データ: 送れるソケットで等分し (最低 512)、余りを順に詰める */
  for (ready = 0, i = 0; i < NSOCK; i++)
    if (ts[i].st != S_FREE && ts[i].remote && ps[i].opened && !ps[i].dead && !ps[i].ctl && ps[i].tx.len && ps[i].credit)
      ready++;
  if (ready) {
    uint32_t share = room / ready, pass;
    if (share < 512)
      share = 512;
    for (pass = 0; pass < 2; pass++)
      for (j = 0; j < NSOCK; j++) {
        struct psl *q;
        i = (rr + j) % NSOCK;
        q = &ps[i];
        if (ts[i].st == S_FREE || !ts[i].remote || !q->opened || q->dead || q->ctl || !q->tx.len || !q->credit)
          continue;
        if (room <= TSP_MHDR + 1)
          break;
        n = q->tx.len;
        if (n > q->credit)
          n = q->credit;
        if (n > room - TSP_MHDR)
          n = room - TSP_MHDR;
        if (pass == 0 && n > share)
          n = share;
        n &= ~1u;                       /* 詰め物を省くため偶数に (1 バイトだけなら 1) */
        if (n == 0)
          n = 1;
        if (n > q->tx.len || tsp_msize(n) > room)
          continue;
        {
          /* リングから 2 回に分けて写す (折り返し) */
          uint8_t *d = tsp_msg(p, TSP_C_DATA, i, q->gen, n);
          uint32_t first = q->tx.size - q->tx.head;
          if (first > n)
            first = n;
          memcpy(d, q->tx.buf + q->tx.head, first);
          memcpy(d + first, q->tx.buf, n - first);
          if (n & 1)
            d[n] = 0;
          q->tx.head += n;
          if (q->tx.head >= q->tx.size)
            q->tx.head -= q->tx.size;
          q->tx.len -= n;
          if (q->tx.len == 0)
            q->tx.head = 0;
        }
        q->credit -= n;
        pstat.tx_bytes += n;
        p += tsp_msize(n);
        room -= tsp_msize(n);
      }
  }

  /* 4. 受信の空きと送信の残り (WINDOW)。データの後に置くので、残りはこの要求で送った分を引いたもの */
  for (j = 0; j < NSOCK; j++) {
    struct psl *q;
    i = (rr + j) % NSOCK;
    q = &ps[i];
    if (!window_due(i))
      continue;
    if (room < tsp_msize(8))
      break;
    p = tsp_msg(p, TSP_C_WINDOW, i, q->gen, 8);
    q->pview = ts[i].rx.size - ts[i].rx.len;
    q->pwant = ts[i].dgram ? 0 : q->busy || q->tx.len ? q->tx.size : 0;
    tsp_put32(p, q->pview);
    tsp_put32(p + 4, q->pwant);
    p += 8;
    room -= tsp_msize(8);
  }

  /* 5. 出し切ったソケットの SHUT / CLOSE */
  for (i = 0; i < NSOCK; i++) {
    struct psl *q = &ps[i];
    struct tsock *t = &ts[i];
    if (t->st == S_FREE || !t->remote || !q->opened || q->dead || q->ctl || q->tx.len)
      continue;
    if (q->shut == 1 && !q->closing && room >= tsp_msize(1)) {
      tsp_msg(p, TSP_C_SHUT, i, q->gen, 1)[0] = 1;
      p[TSP_MHDR + 1] = 0;
      p += tsp_msize(1);
      room -= tsp_msize(1);
      q->shut = 2;
    }
    if (q->closing && room >= tsp_msize(0)) {
      tsp_msg(p, TSP_C_CLOSE, i, q->gen, 0);
      p += tsp_msize(0);
      room -= tsp_msize(0);
      q->closing = 0;
      q->opened = 0;
      t->remote = 0;
      if (t->st == S_ZOMBIE)
        t->st = S_FREE;
    }
  }
  rr++;
  return p - req;
}

/* npd.x から (TIS_NP_OUT): 送る TSP レコードを buf に写す。*urg = 1 なら、すぐ交換してほしい */
static int pi_np_out(uint8_t *buf, int max, int *urg)
{
  if (max == 0) {
    *urg = reqlen ? !notsp : urgent();  /* 問い合わせだけ (npd.x が交換するかを決める) */
    return 0;
  }
  if (max > TSP_REQ_MAX)
    max = TSP_REQ_MAX;
  last_np = now_cs();
  if (reqlen) {
    *urg = !notsp;                      /* 前の交換の答えが来ていない: 同じ要求をもう一度 (TSP が無ければ急がない) */
    if (!notsp && since(last_ok) > NOTSP_CS)
      no_tsp();
    if (reqlen > (uint32_t)max)
      return 0;                         /* IP のレコードで埋まっている: 次の交換で */
    pstat.resend++;
  } else {
    if (max < TSP_HDR + 64) {
      *urg = 1;
      return 0;
    }
    *urg = urgent();
    reqlen = build(max);
  }
  memcpy(buf, req, reqlen);
  pstat.xchg++;
  return reqlen;
}

/* ---- 応答をほどく ---- */

static int slot_ok(int s, int gen)
{
  return s < NSOCK && ts[s].st != S_FREE && ts[s].remote && ps[s].opened && ps[s].gen == gen;
}

static void ev_accept(int l, const uint8_t *b)
{
  struct tsock *lt = &ts[l];
  int h = b[0], c;
  uint8_t *p;
  if (lt->st != S_LISTEN) {
  reject:
    if ((p = cq_msg(TSP_C_REJECT, l, ps[l].gen, 1)) != 0)
      p[0] = h;
    return;
  }
  if (lt->backlog == 0) {
    if (lt->acc0)
      goto reject;
    c = l;
    lt->acc0 = 1;
  } else {
    if (lt->qn >= lt->backlog || (c = sock_alloc(TYPE_TCP)) < 0)
      goto reject;
    open_slot(c);
    ts[c].bound = 1;
    ts[c].parent = l;
    lt->q[lt->qn++] = c;
  }
  ts[c].st = S_EST;
  ts[c].laddr = tsp_get32(b + 2);
  ts[c].lport = tsp_get16(b + 6);
  ts[c].raddr = tsp_get32(b + 8);
  ts[c].rport = tsp_get16(b + 12);
  if (!(p = cq_msg(TSP_C_ADOPT, c, ps[c].gen, 8)))
    return;
  p[0] = h;
  p[1] = c == l ? TSP_AF_LISTENER : 0;
  p[2] = p[3] = 0;
  ps[c].pview = ts[c].rx.size - ts[c].rx.len;
  tsp_put32(p + 4, ps[c].pview);
  pstat.accepts++;
}

static void message(int type, int s, int gen, const uint8_t *b, uint32_t n)
{
  struct tsock *t;
  struct psl *q;
  if (type == TSP_E_RESOLVED) {
    if (s < TSP_NQUERY && qt[s].state == 1 && n >= 6) {
      qt[s].state = 2;
      qt[s].code = b[0];
      qt[s].addr = tsp_get32(b + 2);
    }
    return;
  }
  if (!slot_ok(s, gen)) {
    pstat.stale++;
    return;
  }
  t = &ts[s];
  q = &ps[s];
  if (t->st == S_ZOMBIE && type != TSP_E_ERROR && type != TSP_E_CREDIT)
    return;                             /* 閉じたソケット: 受信は捨てる */
  switch (type) {
  case TSP_E_CONNECTED:
    if (n < 12)
      break;
    t->laddr = tsp_get32(b);
    t->lport = tsp_get16(b + 4);
    t->raddr = tsp_get32(b + 6);
    t->rport = tsp_get16(b + 10);
    if (t->st == S_SYNSENT)
      t->st = S_EST;
    break;
  case TSP_E_ACCEPT:
    if (n >= 14)
      ev_accept(s, b);
    break;
  case TSP_E_DATA:
    if (ring_put(&t->rx, b, n) != n)
      pstat.overrun++;
    q->pview = q->pview > n ? q->pview - n : 0;
    pstat.rx_bytes += n;
    break;
  case TSP_E_DGRAM:
    if (n < 6)
      break;
    {
      uint8_t h[8];
      uint32_t dl = n - 6;
      h[0] = dl >> 8; h[1] = dl;
      h[2] = b[4]; h[3] = b[5];
      h[4] = b[0]; h[5] = b[1]; h[6] = b[2]; h[7] = b[3];
      if (t->rx.size - t->rx.len >= dl + 8) {
        ring_put(&t->rx, h, 8);
        ring_put(&t->rx, b + 6, dl);
        pstat.dgrams_in++;
      } else
        pstat.overrun++;
    }
    break;
  case TSP_E_CREDIT:
    if (n >= 4)
      q->credit = tsp_get32(b);
    break;
  case TSP_E_EOF:
    t->rx_eof = 1;
    break;
  case TSP_E_ERROR:
    dead(s, n >= 1 ? b[0] : 0);
    break;
  case TSP_E_BOUND:
    if (n >= 2)
      t->lport = tsp_get16(b);
    q->waitbound = 0;
    break;
  }
}

/* npd.x から (TIS_NP_IN): Pico の TSP レコード。戻り値: bit 0 = Pico にまだ続きがある */
static int pi_np_in(const uint8_t *r, int len)
{
  const uint8_t *p, *e;
  uint32_t pe;
  if (len < TSP_HDR || r[0] != TSP_MARK || r[1] != TSP_VERSION)
    return 0;
  if (!reqlen || tsp_get16(r + 4) != zseq) {
    pstat.stale_rec++;                  /* 前の交換の応答 (読み込みのキャッシュ、送り直しの重複) */
    return 0;
  }
  reqlen = 0;
  last_ok = now_cs();
  notsp = 0;
  pe = tsp_get32(r + 8);
  if (pknown && pe != pepoch)
    netdown();
  pepoch = pe;
  pknown = 1;
  for (p = r + TSP_HDR, e = r + len; p + TSP_MHDR <= e; ) {
    uint32_t n = tsp_get16(p + 4);
    if (p + TSP_MHDR + n > e)
      break;
    message(p[0], p[1], p[2], p + TSP_MHDR, n);
    p += tsp_msize(n);
  }
  return (r[2] & TSP_PF_MORE) ? 1 : 0;
}

/* 常駐の前に: tx はソケットごとの送信リング (pool から txsz バイトずつ) */
void pico_init(uint8_t *pool, uint32_t txsz, uint32_t seed)
{
  int i;
  memset(ps, 0, sizeof ps);
  memset(qt, 0, sizeof qt);
  memset(nc, 0, sizeof nc);
  memset(&pstat, 0, sizeof pstat);
  for (i = 0; i < NSOCK; i++) {
    ps[i].tx.buf = pool + i * txsz;
    ps[i].tx.size = txsz;
  }
  cqlen = reqlen = 0;
  pknown = zreset = notsp = 0;
  last_ok = last_np = now_cs();
  rr = 0;
  seed *= 2654435761u;
  zseq = seed >> 16;
  zepoch = seed | 1;
  ncpos = 0;
}

/* tis.x -r: Pico 側のソケットを全部捨ててもらう (次の交換の要求の flags) */
void pico_reset(void)
{
  zreset = 1;
}

const struct backend be_pico = {
  "pico", pi_tick, pi_listen, pi_connect, pi_sendspace, pi_send, pi_sendto, pi_shutdown, pi_close,
  pi_bind, pi_pending, pi_resolve_start, pi_resolve_poll, pi_np_out, pi_np_in,
};
