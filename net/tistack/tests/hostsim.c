/*
 * hostsim -- tis.x の API 層 (tis.c) とループバック (lo.c) を Mac の上でそのまま動かして試す
 *
 *   cc -I tests/hostinc tests/hostsim.c lo.c -o hostsim && ./hostsim [乱数の回数]
 *
 * tis.c を include し、_TI_* の入口 (ti_dispatch) を libsocket / libnetwork と同じ引数で呼ぶ。
 * DOS _SUPER は何もせず、_CHANGE_PR は on_yield を呼ぶ: ここで「ほかのスレッド」(相手側の操作) を差し込み、
 * ブロッキングの待ちが正しく起きるか、待っている間に閉じられても壊れないかを見る。
 * 見るのは _TI_ の約束 (README.md の表): 戻り値、状態の文字列、errflag (続く sockstate が 1 回 NULL)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tis.c"

int _dos_super(int stack) { (void)stack; return 0; }

static void (*on_yield)(void);
static long nyield;
void _dos_change_pr(void)
{
  if (++nyield > 100000) {              /* 待ちが終わらない: 試験の失敗として止める */
    printf("FAIL wait never ends\n");
    exit(1);
  }
  if (on_yield)
    on_yield();
}

static int fails, passes;
#define CHECK(name, cond) do { int ok_ = (cond); if (!ok_ || verbose) printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (ok_) passes++; else fails++; } while (0)
static int verbose = 1;

static uint8_t pool[NSOCK][65536];

static void reset(uint32_t bs)
{
  int i;
  tis_init();
  for (i = 0; i < NSOCK; i++) {
    ts[i].rx.buf = pool[i];
    ts[i].rx.size = bs;
  }
  on_yield = 0;
  nyield = 0;
}

/* libsocket / libnetwork の呼び方 */
static long T(long cmd, long a0, long a1, long a2) { long a[6] = { a0, a1, a2, 0, 0, 0 }; return ti_dispatch(cmd, a); }
static long T1(long cmd, long fd) { return ti_dispatch(cmd, (long *)fd); }
static long sock(int type) { return T(_TI_socket, 0, type, 0); }
static uint8_t *sin(uint32_t addr, int port)
{
  static uint8_t b[4][16];
  static int k;
  uint8_t *p = b[k++ & 3];
  memset(p, 0, 16);
  p[2] = port >> 8; p[3] = port; p[4] = addr >> 24; p[5] = addr >> 16; p[6] = addr >> 8; p[7] = addr;
  return p;
}
static long bind_(long s, int port) { return T(_TI_bind, s, (long)sin(0, port), 16); }
static long conn(long s, int port) { return T(_TI_connect, s, (long)sin(0x7f000001, port), 16); }
static long rd(long s, void *b, long n) { return T(_TI_read_s, s, (long)b, n); }
static long wr(long s, const void *b, long n) { return T(_TI_write_s, s, (long)b, n); }
static long slen(long s, int m) { return T(_TI_socklen, s, m, 0); }
static const char *st(long s) { return (const char *)T1(_TI_sockstate, s); }
static void nb(long s, int on) { ((struct usock *)T1(_TI_sock_top, 0))[s - 128].noblock = on; }
static long acc(long s) { long l = 16; uint8_t a[16]; return T(_TI_accept, s, (long)a, (long)&l); }

static long lsn(int port, int backlog)
{
  long s = sock(0);
  bind_(s, port);
  T(_TI_listen, s, backlog, 0);
  return s;
}

/* 非同期の connect → accept で 1 本 */
static void pair(int port, long *c, long *s)
{
  long l = lsn(port, 2);
  *c = sock(0);
  nb(*c, 1);
  conn(*c, port);
  *s = acc(l);
  T1(_TI_close_s, l);
}

/* ---- ブロッキングの待ちに「ほかのスレッド」を差し込む ---- */
static long g_c, g_s;
static void y_write_late(void) { if (nyield == 3) wr(g_c, "late", 4); }
static void y_close_late(void) { if (nyield == 3) T1(_TI_close_s, g_c); }
static void y_drain(void) { static char b[100]; rd(g_s, b, sizeof b); }
static void y_connect_late(void) { if (nyield == 2) { g_c = sock(0); nb(g_c, 1); conn(g_c, 7000); } }
static void y_close_self(void) { if (nyield == 2) T1(_TI_close_s, g_s); }

static void basic(void)
{
  long l, c, s, r, k;
  char b[64];

  reset(8192);
  CHECK("FAST_API probe (-1) -> -1", ti_dispatch(-1, 0) == -1);
  CHECK("sock_top is usock[]", T1(_TI_sock_top, 0) == (long)usock);
  CHECK("sockstate of unused fd -> NULL", st(130) == 0 && st(5) == 0);
  l = lsn(7000, 2);
  CHECK("listen: state LISTEN", l == 128 && !strcmp(st(l), "LISTEN"));
  c = sock(0);
  nb(c, 1);
  r = conn(c, 7000);
  CHECK("nonblocking connect -> -1 + SYN SENT -> (tick) ESTABLISHED", r == -1 && st(c)[0] == 'E');
  CHECK("socklen(listener,0) = queued", slen(l, 0) == 1);
  s = acc(l);
  CHECK("accept -> new fd, ESTABLISHED", s == 130 && !strcmp(st(s), "ESTABLISHED"));
  nb(l, 1);
  CHECK("nonblocking accept, empty -> -1 + LISTEN (EAGAIN)", acc(l) == -1 && st(l)[0] == 'L');
  CHECK("write 5", wr(c, "hello", 5) == 5 && slen(s, 0) == 5 && slen(c, 1) == 0);
  CHECK("read 5", rd(s, b, sizeof b) == 5 && !memcmp(b, "hello", 5));
  nb(s, 1);
  CHECK("nonblocking read empty -> -1, state E (EAGAIN), no errflag", rd(s, b, 1) == -1 && st(s)[0] == 'E');
  /* ブロッキング read: 相手が後から書く */
  nb(s, 0);
  g_c = c; g_s = s; nyield = 0; on_yield = y_write_late;
  r = rd(s, b, sizeof b);
  CHECK("blocking read waits (yields) until data", r == 4 && !memcmp(b, "late", 4) && nyield == 3);
  /* ブロッキング read: 相手が後から閉じる → 0 */
  nyield = 0; on_yield = y_close_late;
  r = rd(s, b, sizeof b);
  CHECK("blocking read -> 0 when peer closes while waiting", r == 0 && !strcmp(st(s), "CLOSE WAIT"));
  on_yield = 0;
  CHECK("socklen at EOF -> -1 (POLLERR)", slen(s, 0) == -1);
  r = wr(s, "x", 1);
  CHECK("write to vanished peer -> -1, then sockstate NULL once (EIO)", r == -1 && st(s) == 0 && st(s) != 0);
  CHECK("close", T1(_TI_close_s, s) == 0 && st(s) == 0);
  CHECK("close again -> 0 (dropbear double close)", T1(_TI_close_s, s) == 0);
  CHECK("close out of range -> -1", T1(_TI_close_s, 200) == -1);

  /* ブロッキング write: 相手のリングが一杯なら、読まれるまで待って全部書く */
  reset(256);
  pair(7001, &c, &s);
  nb(c, 0);
  {
    static char big[1000];
    for (k = 0; k < 1000; k++) big[k] = k;
    g_s = s; nyield = 0; on_yield = y_drain;
    r = wr(c, big, 1000);
    CHECK("blocking write 1000 into 256-byte ring -> 1000 (yields while peer drains)", r == 1000 && nyield > 0);
    on_yield = 0;
    nb(c, 1);
    nb(s, 1);
    while (rd(s, b, sizeof b) > 0)
      ;
    r = wr(c, big, 1000);
    CHECK("nonblocking write 1000 into 256 ring -> 256 (partial)", r == 256);
    CHECK("full: write -> -1 (EAGAIN), socklen(c,1) != 0 (no POLLOUT)", wr(c, big, 1) == -1 && st(c)[0] == 'E' && slen(c, 1) > 0);
  }

  /* ブロッキング accept: 待っている間に接続が来る */
  reset(8192);
  l = lsn(7000, 1);
  nyield = 0; on_yield = y_connect_late;
  s = acc(l);
  CHECK("blocking accept waits until a connect", s >= 128 && s != l && nyield == 2 && !strcmp(st(g_c), "ESTABLISHED"));
  /* 待っている間に自分が閉じられる */
  g_s = s; nyield = 0; on_yield = y_close_self;
  r = rd(s, b, sizeof b);
  CHECK("blocking read on a socket closed by another thread -> -1", r == -1);
  on_yield = 0;

  /* backlog 0 (rcmd.x) */
  reset(8192);
  l = lsn(2323, 0);
  c = sock(0);
  r = conn(c, 2323);
  CHECK("backlog 0: blocking connect -> 0, listener becomes ESTABLISHED", r == 0 && !strcmp(st(l), "ESTABLISHED"));
  CHECK("backlog 0: accept -> same fd, then -1", acc(l) == l && acc(l) == -1);
  c = sock(0);
  CHECK("backlog 0: second client refused", conn(c, 2323) == -1 && !strcmp(st(c), "CLOSED"));
  /* ASCII モードの recvline */
  wr(128 + 1, "cmd one\r\nrest", 13);
  T(_TI_sockmode, l, 1, 0);
  memset(b, 0, sizeof b);
  r = T(_TI_recvline, l, (long)b, 63);
  CHECK("recvline ASCII: \"cmd one\\n\" (CR dropped)", r == 8 && !strcmp(b, "cmd one\n"));
  CHECK("sockmode returns previous", T(_TI_sockmode, l, 0, 0) == 1 && T(_TI_sockmode, l, 2, 0) == 0);
  nb(l, 1);
  CHECK("recvline without newline, nonblocking -> -1", T(_TI_recvline, l, (long)b, 63) == -1);
  T1(_TI_close_s, 129);
  r = T(_TI_recvline, l, (long)b, 63);
  CHECK("recvline at EOF returns the rest", r == 4 && !memcmp(b, "rest", 4));
  CHECK("recvline after EOF -> -1", T(_TI_recvline, l, (long)b, 63) == -1);
  T1(_TI_close_s, l);
  T1(_TI_close_s, c);
  l = lsn(2323, 0);
  CHECK("rebind right after close (no TIME WAIT)", l >= 128 && !strcmp(st(l), "LISTEN"));
  c = sock(0);
  CHECK("bind to a listening port fails, socket stays usable", bind_(c, 2323) == -1 && st(c) == 0 && !strcmp(st(c), "CLOSED"));

  /* refcnt (usesock) */
  reset(8192);
  pair(7002, &c, &s);
  T1(_TI_usesock, s);
  T1(_TI_close_s, s);
  CHECK("usesock: first close keeps it open", st(s) && st(s)[0] == 'E');
  T1(_TI_close_s, s);
  CHECK("usesock: second close frees", st(s) == 0);

  /* shutdown */
  reset(8192);
  pair(7003, &c, &s);
  wr(c, "abc", 3);
  T(_TI_shutdown, c, 1, 0);
  CHECK("shutdown WR: client FIN WAIT 2, server CLOSE WAIT", !strcmp(st(c), "FIN WAIT 2") && !strcmp(st(s), "CLOSE WAIT"));
  CHECK("server reads the rest, then 0", rd(s, b, 64) == 3 && rd(s, b, 64) == 0);
  CHECK("reverse direction still works", wr(s, "zz", 2) == 2 && rd(c, b, 64) == 2);
  CHECK("write after own shutdown -> -1", wr(c, "q", 1) == -1);

  /* 拒否・届かない・名前 */
  reset(8192);
  c = sock(0);
  CHECK("connect to closed port -> -1, CLOSED", conn(c, 9) == -1 && !strcmp(st(c), "CLOSED"));
  c = sock(0);
  CHECK("connect 10.0.0.1 -> -1", T(_TI_connect, c, (long)sin(0x0a000001, 80), 16) == -1);
  info.own_addr = 0xc0a80144;
  l = lsn(8000, 1);
  c = sock(0);
  CHECK("own address (-a) is local", T(_TI_connect, c, (long)sin(0xc0a80144, 8000), 16) == 0);
  info.own_addr = 0;
  CHECK("gethostbyname localhost", T1(_TI_gethostbyname, (long)"LocalHost") != 0 && T1(_TI_gethostbyname, (long)"example.com") == 0);
  CHECK("res_search -> -1", T(_TI_res_search, (long)"x", 1, 1) == -1);
  CHECK("get_iface_list -> NULL", T1(_TI_get_iface_list, 0) == 0);
  {
    long a[2] = { (long)"ssh", (long)"tcp" };
    struct serv *sv = (struct serv *)ti_dispatch(_TI_getservbyname, a);
    CHECK("getservbyname ssh/tcp -> 22", sv && sv->s_port == 22);
  }

  /* UDP */
  reset(64);
  {
    long u1 = sock(1), u2 = sock(1), fl = 16;
    uint8_t from[16];
    long a[6];
    bind_(u1, 53);
    a[0] = u2; a[1] = (long)"0123456789"; a[2] = 10; a[3] = 0; a[4] = (long)sin(0x7f000001, 53); a[5] = 16;
    CHECK("udp sendto", ti_dispatch(_TI_sendto, a) == 10);
    a[1] = (long)"0123456789012345678901234567890123456789012345678901234567"; a[2] = 50;
    ti_dispatch(_TI_sendto, a);         /* 入らない: 捨てる */
    a[0] = u1; a[1] = (long)b; a[2] = 4; a[4] = (long)from; a[5] = (long)&fl;
    r = ti_dispatch(_TI_recvfrom, a);
    CHECK("udp recvfrom truncates to buffer, drops the rest", r == 4 && !memcmp(b, "0123", 4) && from[4] == 127);
    nb(u1, 1);
    CHECK("udp: overflowing datagram was dropped", ti_dispatch(_TI_recvfrom, a) == -1);
  }
}

/* 乱数: 2 本の接続を、ばらばらの大きさの読み書きで流し、中身と表の整合を確かめる (リングの折り返し) */
static void fuzz(int rounds)
{
  int round;
  srand(12345);
  verbose = 0;
  for (round = 0; round < rounds; round++) {
    uint32_t bs = 1 + rand() % 300;
    long c, s;
    long sent = 0, got = 0, i;
    uint8_t w[700], r[700];
    int bad = 0;
    reset(bs);
    pair(6000 + round % 100, &c, &s);
    nb(s, 1);
    for (i = 0; i < 400; i++) {
      int n = rand() % 700, k, m;
      for (k = 0; k < n; k++) w[k] = (uint8_t)(sent + k) * 31;
      m = wr(c, w, n);
      if (m < 0) m = 0;
      if (m > n || ts[s - 128].rx.len > bs) bad = 1;
      sent += m;
      n = rd(s, r, rand() % 700 + 1);
      for (k = 0; k < n; k++)
        if (r[k] != (uint8_t)((got + k) * 31)) bad = 2;
      if (n > 0) got += n;
    }
    while ((i = rd(s, r, sizeof r)) > 0) {
      int k;
      for (k = 0; k < i; k++)
        if (r[k] != (uint8_t)((got + k) * 31)) bad = 3;
      got += i;
    }
    CHECK("fuzz stream", !bad && got == sent);
    if (bad || got != sent) {
      printf("  round %d bs %u bad %d sent %ld got %ld\n", round, bs, bad, sent, got);
      break;
    }
  }
  /* 乱数の命令列: 落ちない、表が壊れない */
  for (round = 0; round < rounds; round++) {
    int k;
    char b[200];
    reset(1 + rand() % 200);
    for (k = 0; k < 300; k++) {
      long fd = 128 + rand() % 34 - 1, port = 100 + rand() % 4;
      int j;
      for (j = 0; j < NSOCK; j++)       /* 全部ノンブロッキング (相手のいない待ちは終わらない) */
        usock[j].noblock = 1;
      switch (rand() % 12) {
      case 0: sock(rand() % 2); break;
      case 1: bind_(fd, port); break;
      case 2: T(_TI_listen, fd, rand() % 3, 0); break;
      case 3: nb(128 + rand() % 32, 1); conn(fd, port); break;
      case 4: nb(fd < 128 || fd >= 160 ? 128 : fd, 1); acc(fd); break;
      case 5: wr(fd, b, rand() % 200); break;
      case 6: nb(fd < 128 || fd >= 160 ? 128 : fd, 1); rd(fd, b, rand() % 200); break;
      case 7: T1(_TI_close_s, fd); break;
      case 8: T(_TI_shutdown, fd, rand() % 3, 0); break;
      case 9: slen(fd, rand() % 2); st(fd); break;
      case 10: T1(_TI_usesock, fd); break;
      case 11: nb(fd < 128 || fd >= 160 ? 128 : fd, 1); T(_TI_recvline, fd, (long)b, 100); break;
      }
    }
    {
      int i, bad = 0;
      for (i = 0; i < NSOCK; i++) {
        struct tsock *t = &ts[i];
        if (t->st == S_FREE) continue;
        if (t->rx.len > t->rx.size || t->rx.head >= t->rx.size) bad = 1;
        if (t->peer >= 0 && (ts[t->peer].st == S_FREE || ts[t->peer].peer != i)) bad = 2;
        if (t->qn > MAXQ) bad = 3;
        for (k = 0; k < t->qn; k++)
          if (ts[t->q[k]].parent != i || ts[t->q[k]].st == S_FREE) bad = 4;
      }
      CHECK("fuzz ops: table consistent", !bad);
      if (bad) {
        printf("  round %d bad %d\n", round, bad);
        break;
      }
    }
  }
  verbose = 1;
}

int main(int argc, char **argv)
{
  basic();
  fuzz(argc > 1 ? atoi(argv[1]) : 300);
  printf("done: %d pass, %d fail\n", passes, fails);
  return fails ? 1 : 0;
}
