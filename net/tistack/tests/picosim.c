/*
 * picosim -- tis.x の Pico backend (pico.c) と Pico のソケット窓口 (pico/tsp_svc.c) を、Mac の上で
 *            npd の交換を真似た管でつないで試す
 *
 *   make -C tests picosim && tests/picosim [-x 交換の時間 µs] [-c Z の写しの ns/バイト] [-q]
 *
 * Z 側: tis.c (API 層) + lo.c + pico.c をそのまま。_TI_* の入口を libsocket / libnetwork と同じ引数で呼ぶ。
 * 管 (npd.x の代わり): _CHANGE_PR のたびに仮想の時計を進め、npd.x と同じ決まりで交換する。
 *   - 交換は Z からだけ。1 回に要求 1 個 (≦ 4078 バイト) と応答 1 個 (≦ 4064 バイト)
 *   - Z に急ぎ (送るもの) があるか、前の応答が more なら、すぐ次の交換。どちらも無ければ 60ms ごと
 *   - 交換 1 回に -x µs (既定 5000: SCSI の書き込み + 読み込み)。-c で Z の写し (memcpy) の時間を足す
 *   - 書き込みの失敗 (Pico に届かない)・読み込みの失敗 (応答が届かない)・古い応答 (キャッシュ) を混ぜられる
 * Pico 側: 本物の lwIP 2.2 (pico-sdk の lib/lwip、NO_SYS、仮想の時計) + tsp_svc.c。LAN の相手も同じ lwIP の中
 * (picoworld.c)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tis.c"
#include "../tsp.h"
#include "picoworld.h"

int _dos_super(int stack) { (void)stack; return 0; }
uint32_t pico_host_clock_cs(void);

static int fails, passes, verbose = 1;
#define CHECK(name, cond) do { int ok_ = (cond); if (!ok_ || verbose) printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (ok_) passes++; else fails++; } while (0)

/* ---- 管 (npd.x の代わり) ---- */
#define YIELD_US  100                   /* _CHANGE_PR 1 回 (ほかのスレッドが少し動く) */
#define IDLE_US   60000                 /* npd.x の問い合わせ間隔 (ZIDLE = 6) */
static uint32_t xcost_us = 5000;        /* 交換 1 回 */
static uint32_t zcopy_ns = 0;           /* Z の写し 1 バイト (送り 3 回・受け 2 回写す) */
static double wloss, rloss, stale;      /* 書き込みの失敗・読み込みの失敗・古い応答の割合 */
static uint64_t next_x, nx, nx_data;
static int pmore;
static uint8_t zbuf[TSP_REQ_MAX], rbuf[TSP_RES_MAX], oldres[TSP_RES_MAX];
static int oldlen;
static uint64_t deadline;
static void (*on_yield)(void);

static int no_tsp_fw;                   /* 1: Pico が 0xA6 を知らない (今のファーム): 要求を捨てて応答を返さない */
uint32_t pico_host_clock_cs(void) { return (uint32_t)(world_us / 10000 % 8640000); }

static double rnd(void) { return (double)random() / 2147483648.0; }

static void exchange(void)
{
  long a[3] = { (long)zbuf, 0, 0 }, b[2];
  int n, r, more = 0, f;
  uint32_t tx0 = pstat.tx_bytes, rx0 = pstat.rx_bytes;
  ti_dispatch(TIS_NP_OUT, a);           /* 問い合わせ: 急ぎがあるか */
  if (!a[2] && !pmore && world_us < next_x)
    return;
  a[1] = TSP_REQ_MAX;
  n = ti_dispatch(TIS_NP_OUT, a);
  nx++;
  pmore = 0;
  world_us += xcost_us;
  if (n <= 0)
    goto done;
  if ((wloss && rnd() < wloss) || no_tsp_fw)
    goto done;                          /* 書き込みが Pico に届かなかった */
  r = world_exchange(zbuf, n, rbuf, TSP_RES_MAX, &more);
  if (stale && oldlen && rnd() < stale) {
    b[0] = oldlen;                      /* 前の回の応答 (Z の読み込みキャッシュ) を先に読む */
    b[1] = (long)oldres;
    ti_dispatch(TIS_NP_IN, (long *)b);
  }
  if (rloss && rnd() < rloss)
    goto done;                          /* 応答を読めなかった */
  if (r > 0) {
    memcpy(oldres, rbuf, r);
    oldlen = r;
    b[0] = r;
    b[1] = (long)rbuf;
    f = ti_dispatch(TIS_NP_IN, (long *)b);
    pmore = more || (f & 1);
  }
done:
  if (pstat.tx_bytes != tx0 || pstat.rx_bytes != rx0)
    nx_data++;
  world_us += (uint64_t)((pstat.tx_bytes - tx0) * 3 + (pstat.rx_bytes - rx0) * 2) * zcopy_ns / 1000;
  next_x = world_us + IDLE_US;
}

static void step(void)
{
  world_us += YIELD_US;
  world_poll();
  exchange();
  if (world_us > deadline) {
    printf("FAIL virtual time limit\n");
    world_stat();
    exit(1);
  }
}

void _dos_change_pr(void)
{
  step();
  if (on_yield)
    on_yield();
}

static void run_ms(int ms)
{
  uint64_t end = world_us + (uint64_t)ms * 1000;
  while (world_us < end)
    step();
}

/* ---- libsocket / libnetwork の呼び方 ---- */
static long T(long cmd, long a0, long a1, long a2) { long a[6] = { a0, a1, a2, 0, 0, 0 }; return ti_dispatch(cmd, a); }
static long T6(long cmd, long a0, long a1, long a2, long a3, long a4, long a5) { long a[6] = { a0, a1, a2, a3, a4, a5 }; return ti_dispatch(cmd, a); }
static long T1(long cmd, long fd) { return ti_dispatch(cmd, (long *)fd); }
static long sock(int type) { return T(_TI_socket, 0, type, 0); }
static uint8_t *sin_(uint32_t addr, int port)
{
  static uint8_t b[4][16];
  static int k;
  uint8_t *p = b[k++ & 3];
  memset(p, 0, 16);
  p[2] = port >> 8; p[3] = port; p[4] = addr >> 24; p[5] = addr >> 16; p[6] = addr >> 8; p[7] = addr;
  return p;
}
static long conn(long s, uint32_t addr, int port) { return T(_TI_connect, s, (long)sin_(addr, port), 16); }
static long rd(long s, void *b, long n) { return T(_TI_read_s, s, (long)b, n); }
static long wr(long s, const void *b, long n) { return T(_TI_write_s, s, (long)b, n); }
static long slen(long s, int m) { return T(_TI_socklen, s, m, 0); }
static const char *st(long s) { const char *p = (const char *)T1(_TI_sockstate, s); return p ? p : ""; }
static int st_null(long s) { return T1(_TI_sockstate, s) == 0; }   /* 直前の -1 が本当のエラー (libsocket は EIO) */
static const char *serr(long s) { return (const char *)T1(_TI_sockerr, s); }
static void nb(long s, int on) { ((struct usock *)T1(_TI_sock_top, 0))[s - 128].noblock = on; }
static long acc(long s) { long l = 16; uint8_t a[16]; return T(_TI_accept, s, (long)a, (long)&l); }
static long cls(long s) { return T1(_TI_close_s, s); }
static uint32_t host_addr(long h)
{
  /* struct hostent の h_addr_list[0] (tis.c の lo_host / net_host と同じ並び) */
  struct { char *h_name; char **h_aliases; int h_addrtype, h_length; char **h_addr_list; } *he = (void *)h;
  uint32_t v;
  memcpy(&v, he->h_addr_list[0], 4);
  return v;
}
static long lsn(int port, int backlog)
{
  long s = sock(0);
  T(_TI_bind, s, (long)sin_(0, port), 16);
  T(_TI_listen, s, backlog, 0);
  return s;
}

static int open_socks(void)
{
  int i, n = 0;
  for (i = 0; i < NSOCK; i++)
    if (ts[i].st != S_FREE)
      n++;
  return n;
}

/* 全部閉じてから、Z・Pico・lwIP に残りが無いか */
static void settle(const char *what)
{
  char name[128];
  run_ms(3000);
  snprintf(name, sizeof name, "%s: nothing left (Z %d, Pico %d, lwIP active pcbs %d)", what, open_socks(),
           world_pico_sockets(), world_tcp_pcbs());
  CHECK(name, open_socks() == 0 && world_pico_sockets() == 0);
}

static uint64_t t0;
static double secs(void) { return (world_us - t0) / 1e6; }

/* ---- 試験 ---- */

static void x_name_connect(void)
{
  long h, s, r;
  uint32_t a;
  char b[64];
  h = T1(_TI_gethostbyname, (long)"peer.test");
  CHECK("gethostbyname(peer.test) via Pico DNS -> 10.0.0.1", h && host_addr(h) == PICO_IP);
  CHECK("gethostbyname(nope.example) -> NULL (NXDOMAIN)", T1(_TI_gethostbyname, (long)"nope.example") == 0);
  CHECK("gethostbyname(localhost) stays local", (h = T1(_TI_gethostbyname, (long)"localhost")) && host_addr(h) == 0x7f000001);
  h = T1(_TI_gethostbyname, (long)"echo.test");
  a = host_addr(h);
  s = sock(0);
  t0 = world_us;
  r = conn(s, a, 7);
  CHECK("blocking connect to echo.test:7 -> 0, ESTABLISHED", r == 0 && !strcmp(st(s), "ESTABLISHED"));
  printf("  connect took %.0f ms virtual\n", secs() * 1000);
  {
    long l = 16;
    uint8_t sa[16];
    T(_TI_getpeername, s, (long)sa, (long)&l);
    CHECK("getpeername = 10.0.0.1:7", sa[2] == 0 && sa[3] == 7 && sa[4] == 10 && sa[7] == 1);
    T(_TI_getsockname, s, (long)sa, (long)&l);
    CHECK("getsockname = Pico's address, port chosen by Pico", sa[4] == 10 && sa[7] == 1 && (sa[2] << 8 | sa[3]) != 0);
  }
  t0 = world_us;
  CHECK("write 5", wr(s, "hello", 5) == 5);
  r = rd(s, b, sizeof b);
  CHECK("echo back (blocking read)", r == 5 && !memcmp(b, "hello", 5));
  printf("  echo round trip %.0f ms virtual\n", secs() * 1000);
  nb(s, 1);
  CHECK("nonblocking read empty -> -1, state E (EAGAIN)", rd(s, b, 1) == -1 && st(s)[0] == 'E');
  CHECK("close", cls(s) == 0 && st_null(s));
  CHECK("close again -> 0", cls(s) == 0);
  settle("name connect");
}

static uint8_t big[1 << 20];

static void x_bulk(int report)
{
  long s, r, k;
  uint64_t got = 0;
  uint32_t bad = 0;
  uint64_t x0;
  static uint8_t b[8192];
  /* Z -> LAN: 1MB を discard に (ブロッキング write 8KB ずつ、最後に close してすぐ戻る) */
  for (k = 0; k < (long)sizeof big; k++)
    big[k] = pat(k);
  memset(&dstat, 0, sizeof dstat);
  s = sock(0);
  conn(s, PICO_IP, 9);
  t0 = world_us;
  x0 = nx;
  for (k = 0; k < (long)sizeof big; k += r)
    if ((r = wr(s, big + k, sizeof big - k < 8192 ? sizeof big - k : 8192)) <= 0)
      break;
  cls(s);
  while (dstat.fins == 0)
    step();
  CHECK("Z -> LAN 1MB: all bytes, pattern ok, FIN after data (write; close)", dstat.bytes == sizeof big && dstat.bad == 0);
  if (report)
    printf("  Z -> LAN 1MB: %.2f s virtual, %.1f KB/s, %llu exchanges, %.0f payload bytes/exchange\n",
           (dstat.done_us - t0) / 1e6, sizeof big / 1024.0 / ((dstat.done_us - t0) / 1e6),
           (unsigned long long)(nx - x0), (double)sizeof big / (nx - x0));
  /* LAN -> Z: source から 1MB */
  s = sock(0);
  conn(s, PICO_IP, 19);
  wr(s, "1048576\n", 8);
  t0 = world_us;
  x0 = nx;
  while ((r = rd(s, b, sizeof b)) > 0) {
    for (k = 0; k < r; k++)
      if (b[k] != pat(got + k))
        bad++;
    got += r;
  }
  CHECK("LAN -> Z 1MB: all bytes, pattern ok, then EOF (read 0)", got == sizeof big && bad == 0 && r == 0);
  if (report)
    printf("  LAN -> Z 1MB: %.2f s virtual, %.1f KB/s, %llu exchanges, %.0f payload bytes/exchange\n",
           secs(), got / 1024.0 / secs(), (unsigned long long)(nx - x0), (double)got / (nx - x0));
  cls(s);
}

/* ノンブロッキングで n 本を echo に同時に流す (poll は socklen で) */
static int x_many(int n, uint32_t each)
{
  long s[32];
  uint32_t wpos[32] = { 0 }, rpos[32] = { 0 }, bad = 0, k;
  int i, alive;
  static uint8_t b[4096];
  for (i = 0; i < n; i++) {
    s[i] = sock(0);
    nb(s[i], 1);
    conn(s[i], PICO_IP, 7);             /* -1 + SYN SENT (EINPROGRESS) */
  }
  t0 = world_us;
  do {
    alive = 0;
    for (i = 0; i < n; i++) {
      long r;
      if (rpos[i] == each)
        continue;
      alive++;
      if (st(s[i])[0] != 'E')
        continue;
      if (wpos[i] < each && slen(s[i], 1) == 0) {
        uint32_t m = each - wpos[i] < 3000 ? each - wpos[i] : 3000;
        for (k = 0; k < m; k++)
          b[k] = pat(wpos[i] + k + i);
        r = wr(s[i], b, m);
        if (r > 0)
          wpos[i] += r;
      }
      if (slen(s[i], 0) > 0 && (r = rd(s[i], b, sizeof b)) > 0) {
        for (k = 0; k < (uint32_t)r; k++)
          if (b[k] != pat(rpos[i] + k + i))
            bad++;
        rpos[i] += r;
      }
    }
    step();
  } while (alive);
  for (i = 0; i < n; i++)
    cls(s[i]);
  return bad;
}

/* 1 本が大量に受けている間の、もう 1 本の打鍵の往復 (交換の回数) */
static void x_fair(void)
{
  long bulk, tty, r;
  static uint8_t b[8192];
  uint64_t got = 0, x0, worst = 0, sum = 0;
  int k, n = 0;
  char cnt[32];
  bulk = sock(0);
  conn(bulk, PICO_IP, 19);
  snprintf(cnt, sizeof cnt, "%d\n", 4 << 20);
  wr(bulk, cnt, strlen(cnt));
  tty = sock(0);
  conn(tty, PICO_IP, 7);
  nb(bulk, 1);
  nb(tty, 1);
  t0 = world_us;
  for (k = 0; k < 40; k++) {
    uint64_t sent_x;
    /* 200ms のあいだ bulk を読み続ける */
    uint64_t until = world_us + 200000;
    while (world_us < until) {
      while ((r = rd(bulk, b, sizeof b)) > 0)
        got += r;
      step();
    }
    wr(tty, "k", 1);
    sent_x = nx;
    x0 = world_us;
    for (;;) {
      while ((r = rd(bulk, b, sizeof b)) > 0)
        got += r;
      if (rd(tty, b, 1) == 1)
        break;
      step();
    }
    if (nx - sent_x > worst)
      worst = nx - sent_x;
    sum += world_us - x0;
    n++;
  }
  printf("  fairness: keystroke echo during 4MB download: avg %.1f ms, worst %llu exchanges; bulk got %llu bytes (%.1f KB/s)\n",
         sum / 1000.0 / n, (unsigned long long)worst, (unsigned long long)got, got / 1024.0 / secs());
  CHECK("fairness: interactive echo stays within 3 exchanges while a bulk socket is busy", worst <= 3);
  cls(bulk);
  cls(tty);
}

static void x_listen(void)
{
  long l, s[3], r;
  int c[3], k;
  char b[64];
  /* backlog 2: LAN から 3 本。3 本目は (accept の前は) 断られる */
  l = lsn(8000, 2);
  run_ms(200);
  CHECK("listen 8000 on the Pico: LISTEN", !strcmp(st(l), "LISTEN"));
  for (k = 0; k < 3; k++) {
    c[k] = client_start(8000, "from lan\n", 9, 0);
    run_ms(150);
  }
  CHECK("socklen(listener,0) = 2 queued", slen(l, 0) == 2);
  CHECK("third client refused/reset by the Pico (backlog full)", cl[c[2]].refused || cl[c[2]].reset || cl[c[2]].closed);
  for (k = 0; k < 2; k++) {
    long al = 16;
    uint8_t sa[16];
    s[k] = T(_TI_accept, l, (long)sa, (long)&al);
    CHECK("accept -> new fd, ESTABLISHED, peer address 10.0.0.1", s[k] >= 128 && !strcmp(st(s[k]), "ESTABLISHED") && sa[4] == 10);
    r = rd(s[k], b, sizeof b);
    CHECK("read what the LAN client sent", r == 9 && !memcmp(b, "from lan\n", 9));
    wr(s[k], "hi there", 8);
  }
  run_ms(300);
  CHECK("LAN clients got the answer", cl[c[0]].rxlen == 8 && cl[c[1]].rxlen == 8 && !memcmp(cl[c[0]].rx, "hi there", 8));
  client_close(c[0]);
  r = rd(s[0], b, sizeof b);
  CHECK("LAN client closes -> Z read 0, CLOSE WAIT", r == 0 && !strcmp(st(s[0]), "CLOSE WAIT"));
  cls(s[0]);
  cls(s[1]);
  run_ms(300);
  CHECK("Z closes -> LAN client sees FIN", cl[c[1]].closed);
  client_close(c[1]);
  client_close(c[2]);
  nb(l, 1);
  CHECK("nonblocking accept, empty -> -1 + LISTEN (EAGAIN)", acc(l) == -1 && st(l)[0] == 'L');
  cls(l);
  settle("listen/accept");
}

/* rcmd.x の流儀: listen(ls, 0)、sockstate が LISTEN でなくなるまで _CHANGE_PR、accept は同じ fd、
   ASCII の recvline、write、close、同じポートに bind し直す */
static void x_rcmd(void)
{
  int round, c, c2;
  for (round = 0; round < 3; round++) {
    long ls, a, r, k;
    char line[128], out[64];
    ls = lsn(2323, 0);
    c = client_start(2323, "echo hi\r\n", 9, 0);
    while (!strncmp(st(ls), "LISTEN", 6))
      _dos_change_pr();
    c2 = client_start(2323, "x\r\n", 3, 0);   /* 引き取られたあとの 2 本目は断られる */
    a = acc(ls);
    CHECK("rcmd: accept on backlog-0 listener returns the same fd", a == ls);
    T(_TI_sockmode, a, 1, 0);
    memset(line, 0, sizeof line);
    r = T(_TI_recvline, a, (long)line, sizeof line - 1);
    CHECK("rcmd: recvline ASCII -> \"echo hi\\n\"", r == 8 && !strcmp(line, "echo hi\n"));
    T(_TI_sockmode, a, 0, 0);
    snprintf(out, sizeof out, "hi round %d\r\n", round);
    for (k = 0; k < (long)strlen(out); k += r)
      if ((r = wr(a, out + k, strlen(out) - k)) <= 0)
        break;
    T1(_TI_usflush, a);
    cls(a);
    if (round == 1) {
      /* rcmd.x の本当の並び: close の直後に同じポートで bind / listen (前の接続はまだ Pico で閉じている途中。
         LISTEN が CLOSE より先に Pico に届く) */
      long ls2 = lsn(2323, 0);
      CHECK("rcmd: re-listen on 2323 right after close (old connection still draining) -> LISTEN", !strcmp(st(ls2), "LISTEN"));
      while (!cl[c].closed)
        step();
      cls(ls2);
    }
    run_ms(400);
    CHECK("rcmd: LAN client got the output and FIN", cl[c].closed && cl[c].rxlen == strlen(out) && !memcmp(cl[c].rx, out, cl[c].rxlen));
    CHECK("rcmd: second client while adopted was refused", cl[c2].refused || cl[c2].reset || cl[c2].closed);
    client_close(c);
    client_close(c2);
  }
  settle("rcmd");
}

/* dropbear (ssh.x) の流儀: 名前を引く、ブロッキング connect、ノンブロッキングにして poll (socklen)、
   バナーと KEXINIT、相手が閉じたら read 0、同じ fd を 2 回 close */
static void x_ssh(void)
{
  long h, s, r;
  static uint8_t b[2048], kex[304];
  uint32_t got = 0, k;
  sshd_got = 0;
  h = T1(_TI_gethostbyname, (long)"ssh.test");
  s = sock(0);
  r = conn(s, host_addr(h), 22);
  CHECK("ssh: blocking connect by name -> 0", r == 0);
  nb(s, 1);
  wr(s, "SSH-2.0-dropbear_2022.83\r\n", 26);
  for (k = 0; k < sizeof kex; k++)
    kex[k] = k;
  CHECK("ssh: write KEXINIT 304", wr(s, kex, sizeof kex) == sizeof kex);
  for (;;) {
    long l = slen(s, 0);
    if (l > 0) {
      r = rd(s, b + got, sizeof b - got);
      if (r > 0)
        got += r;
    } else if (l < 0) {
      r = rd(s, b, 1);
      break;
    }
    step();
  }
  CHECK("ssh: server banner + KEXINIT (21 + 304) then EOF", got == 21 + 304 && !memcmp(b, "SSH-2.0-OpenSSH_9.0\r\n", 21) && r == 0);
  CHECK("ssh: server got client banner + KEXINIT", sshd_got == 26 + 304);
  CHECK("ssh: close, close again -> 0, 0", cls(s) == 0 && cls(s) == 0);
  settle("ssh");
}

static void x_halfclose(void)
{
  long s, r, k;
  static uint8_t b[32768];
  uint32_t got = 0;
  /* shutdown(SHUT_WR): echo は残りを返してから閉じる */
  s = sock(0);
  conn(s, PICO_IP, 7);
  for (k = 0; k < 20000; k++)
    b[k] = pat(k);
  wr(s, b, 20000);
  T(_TI_shutdown, s, 1, 0);
  CHECK("shutdown WR: FIN WAIT 2", !strcmp(st(s), "FIN WAIT 2"));
  CHECK("write after shutdown -> -1, then sockstate NULL once (EIO)", wr(s, "x", 1) == -1 && st_null(s) && !st_null(s));
  while ((r = rd(s, b + got, sizeof b - got)) > 0)
    got += r;
  for (k = 0; k < (long)got && b[k] == pat(k); k++)
    ;
  CHECK("echo returns all 20000 bytes after our FIN, then EOF", got == 20000 && k == 20000 && r == 0);
  CHECK("state CLOSING after both FINs", !strcmp(st(s), "CLOSING"));
  cls(s);
  /* 相手の RST: reset (70) は何か受けたら切る */
  s = sock(0);
  conn(s, PICO_IP, 70);
  wr(s, "x", 1);
  r = rd(s, b, 10);
  CHECK("peer RST: blocking read -> 0 (EOF)", r == 0);
  r = wr(s, "y", 1);
  CHECK("peer RST: write -> -1, sockerr \"Connection reset\"", r == -1 && !strcmp(serr(s), "Connection reset") && st_null(s));
  cls(s);
  settle("half-close/reset");
}

static void x_errors(void)
{
  long s, r;
  s = sock(0);
  r = conn(s, PICO_IP, 1);
  CHECK("refused: blocking connect -> -1, CLOSED, \"Connection refused\"", r == -1 && !strcmp(st(s), "CLOSED") && !strcmp(serr(s), "Connection refused"));
  cls(s);
  s = sock(0);
  r = conn(s, NOROUTE_IP, 80);
  CHECK("unreachable (no route): -1, \"Network unreachable\"", r == -1 && !strcmp(serr(s), "Network unreachable"));
  cls(s);
  s = sock(0);
  nb(s, 1);
  r = conn(s, HOLE_IP, 80);
  CHECK("nonblocking connect into a black hole -> -1 + SYN SENT (EINPROGRESS)", r == -1 && !strcmp(st(s), "SYN SENT"));
  t0 = world_us;
  while (!strcmp(st(s), "SYN SENT"))
    step();
  CHECK("black hole: connect gives up, CLOSED, \"Connection timed out\"", !strcmp(st(s), "CLOSED") && !strcmp(serr(s), "Connection timed out"));
  printf("  black hole timeout after %.0f s virtual (lwIP SYN retries)\n", secs());
  CHECK("socklen(fd,1) after failed connect -> -1 (POLLERR)", slen(s, 1) == -1);
  cls(s);
  settle("errors");
}

static void x_udp(void)
{
  long s, r, l;
  uint8_t b[64], sa[16];
  s = sock(1);
  r = T6(_TI_sendto, s, (long)"ping", 4, 0, (long)sin_(PICO_IP, 7), 16);
  CHECK("udp sendto 10.0.0.1:7 -> 4", r == 4);
  l = 16;
  r = T6(_TI_recvfrom, s, (long)b, sizeof b, 0, (long)sa, (long)&l);
  CHECK("udp recvfrom: echo \"ping\" from 10.0.0.1:7", r == 4 && !memcmp(b, "ping", 4) && sa[3] == 7 && sa[4] == 10);
  cls(s);
  /* UDP の受け口: bind(5000) → LAN から 2 個 */
  s = sock(1);
  T(_TI_bind, s, (long)sin_(0, 5000), 16);
  run_ms(200);
  udp_peer_send(9999, 5000, "one", 3);
  udp_peer_send(9999, 5000, "second", 6);
  l = 16;
  r = T6(_TI_recvfrom, s, (long)b, sizeof b, 0, (long)sa, (long)&l);
  CHECK("udp server: first datagram \"one\"", r == 3 && !memcmp(b, "one", 3));
  r = T6(_TI_recvfrom, s, (long)b, sizeof b, 0, (long)sa, (long)&l);
  CHECK("udp server: second datagram \"second\" (boundaries kept), from port 9999", r == 6 && !memcmp(b, "second", 6) && (sa[2] << 8 | sa[3]) == 9999);
  r = T6(_TI_sendto, s, (long)"back", 4, 0, (long)sin_(PICO_IP, 9999), 16);
  run_ms(200);
  CHECK("udp server: reply reaches the LAN peer", udp_peer_got == 1);
  cls(s);
  settle("udp");
}

/* 交換の失敗と古い応答を混ぜても中身が壊れない */
static void x_loss(void)
{
  int bad;
  uint32_t dup0 = world_dup(), rs0 = pstat.resend, st0 = pstat.stale_rec;
  wloss = rloss = 0.05;
  stale = 0.05;
  bad = x_many(4, 65536);
  CHECK("5% write loss + 5% read loss + 5% stale responses: 4 x 64KB echo intact", bad == 0);
  printf("  resent %u, Pico answered duplicates %u, stale responses ignored %u\n", pstat.resend - rs0,
         world_dup() - dup0, pstat.stale_rec - st0);
  CHECK("loss: resends happened and the Pico answered them from its saved response", pstat.resend > rs0 && world_dup() > dup0);
  x_bulk(0);
  wloss = rloss = stale = 0;
  settle("loss");
}

static void x_reboot(void)
{
  long s, r;
  char b[16];
  s = sock(0);
  conn(s, PICO_IP, 7);
  wr(s, "a", 1);
  rd(s, b, 1);
  world_pico_reboot(0x5eed0002);
  r = rd(s, b, 1);
  CHECK("Pico reboot (new epoch): blocking read -> 0", r == 0);
  CHECK("Pico reboot: sockerr \"Network is down\", write -> -1", !strcmp(serr(s), "Network is down") && wr(s, "b", 1) == -1 && st_null(s));
  cls(s);
  s = sock(0);
  CHECK("after Pico reboot, new connections work", conn(s, PICO_IP, 7) == 0 && wr(s, "c", 1) == 1 && rd(s, b, 1) == 1 && b[0] == 'c');
  /* tis.x が常駐し直した (Z のリセット): 新しい zepoch → Pico は前のソケットを捨てる (相手に RST) */
  {
    uint32_t r0 = echo_resets;
    int i;
    tis_init();
    for (i = 0; i < NSOCK; i++) {
      static uint8_t pool[NSOCK][8192];
      ts[i].rx.buf = pool[i];
      ts[i].rx.size = 8192;
    }
    {
      static uint8_t txpool[NSOCK * 4096];
      pico_init(txpool, 4096, 0x2222);
    }
    run_ms(500);
    CHECK("Z restart (new zepoch): Pico aborts the old socket, peer sees RST", echo_resets == r0 + 1 && world_pico_sockets() == 0);
  }
  settle("reboot");
}

static int raw_got, raw_notsup;
static void raw_scan(const uint8_t *r, int l)
{
  int i;
  for (i = TSP_HDR; i + TSP_MHDR <= l; i += tsp_msize(tsp_get16(r + i + 4))) {
    if (r[i] == TSP_E_CONNECTED && r[i + 1] == 0)
      raw_got = 1;
    if (r[i] == TSP_E_ERROR && r[i + 1] == 1 && r[i + TSP_MHDR] == TSP_ERR_NOTSUP)
      raw_notsup = 1;
  }
}

/* Pico が TSP を話さない (今のファーム): 待ちが固まらずに "Network is down" で終わり、TSP が戻れば元どおり */
static void x_notsp(void)
{
  long s, r, l;
  uint64_t t;
  no_tsp_fw = 1;
  s = sock(0);
  t = world_us;
  r = conn(s, PICO_IP, 7);
  printf("  connect without TSP gave up after %.1f s virtual\n", (world_us - t) / 1e6);
  CHECK("no TSP on the Pico: blocking connect -> -1 \"Network is down\" within 5 s", r == -1 && !strcmp(serr(s), "Network is down") && world_us - t < 5000000);
  cls(s);
  s = sock(0);
  t = world_us;
  CHECK("no TSP: next connect fails at once", conn(s, PICO_IP, 7) == -1 && world_us - t < 100000);
  cls(s);
  CHECK("no TSP: gethostbyname -> NULL at once", T1(_TI_gethostbyname, (long)"peer.test") == 0);
  l = lsn(2324, 0);
  CHECK("no TSP: listen still works (loopback only)", l >= 128 && !strcmp(st(l), "LISTEN"));
  cls(l);
  no_tsp_fw = 0;
  run_ms(500);
  s = sock(0);
  CHECK("TSP back: connect works again", conn(s, PICO_IP, 7) == 0);
  cls(s);
  settle("no TSP");
}

/* 記録形式を直に: 名前でつなぐ (addr 0)、TLS は NOTSUP、同じ要求には同じ応答 */
static void x_raw(void)
{
  uint8_t q[256], r1[TSP_RES_MAX], r2[TSP_RES_MAX], *p;
  int n, l1, l2, more, k;
  uint32_t zep = 0x77777777;
  uint16_t seq = 100;
  for (k = 0; k < 2; k++) {
    q[0] = TSP_MARK; q[1] = TSP_VERSION; q[2] = q[3] = 0;
    tsp_put16(q + 4, seq++); tsp_put16(q + 6, 0); tsp_put32(q + 8, zep);
    n = TSP_HDR;
    p = tsp_msg(q + n, TSP_C_CONNECT, k, 1, 10 + 9);
    p[0] = TSP_PROTO_TCP; p[1] = TSP_CF_NAME | (k ? TSP_CF_TLS : 0);
    tsp_put16(p + 2, 0); tsp_put16(p + 4, 7); tsp_put32(p + 6, 0);
    memcpy(p + 10, "name.test", 9);
    n += tsp_msize(19);
    p = tsp_msg(q + n, TSP_C_WINDOW, k, 1, 8);
    tsp_put32(p, 8192); tsp_put32(p + 4, 0);
    n += tsp_msize(8);
    l1 = world_exchange(q, n, r1, TSP_RES_MAX, &more);
    l2 = world_exchange(q, n, r2, TSP_RES_MAX, &more);
    CHECK("raw: the same request twice -> identical response (not applied twice)", l1 == l2 && !memcmp(r1, r2, l1));
    raw_scan(r1, l1);
  }
  /* 答えが来るまで空の要求を回す */
  for (k = 0; k < 50 && !(raw_got && raw_notsup); k++) {
    world_us += 10000;
    world_poll();
    q[0] = TSP_MARK; q[1] = TSP_VERSION; q[2] = q[3] = 0;
    tsp_put16(q + 4, seq++); tsp_put16(q + 6, 0); tsp_put32(q + 8, zep);
    l1 = world_exchange(q, TSP_HDR, r1, TSP_RES_MAX, &more);
    raw_scan(r1, l1);
  }
  CHECK("raw: CONNECT by name (addr 0, Pico resolves) -> CONNECTED", raw_got);
  CHECK("raw: CONNECT with the TLS flag -> ERROR NOTSUP (TLS is not in this step)", raw_notsup);
  /* 後始末: 別の epoch の要求 (Z の tis.x に戻す) で Pico は全部捨てる */
}

static void throughput(void)
{
  static const uint32_t xc[] = { 2500, 5000, 10000 };
  static const uint32_t cp[] = { 0, 600, 3000 };
  unsigned i, j;
  verbose = 0;
  printf("\nthroughput (virtual time, 1MB each way, 60ms idle poll):\n");
  for (j = 0; j < 3; j++)
    for (i = 0; i < 3; i++) {
      xcost_us = xc[i];
      zcopy_ns = cp[j];
      printf(" exchange %.1f ms, Z copy %u ns/byte:\n", xc[i] / 1000.0, cp[j]);
      x_bulk(1);
    }
  xcost_us = 5000;
  zcopy_ns = 0;
  verbose = 1;
}

int main(int argc, char **argv)
{
  int i, quick = 0;
  static uint8_t pool[NSOCK][8192], txpool[NSOCK * 4096];
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-x") && i + 1 < argc)
      xcost_us = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-c") && i + 1 < argc)
      zcopy_ns = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-q"))
      quick = 1;
  }
  setvbuf(stdout, 0, _IONBF, 0);
  srandom(1);
  extern int tsp_log_on;
  tsp_log_on = 1;
  world_init(0x5eed0001);
  tis_init();
  for (i = 0; i < NSOCK; i++) {
    ts[i].rx.buf = pool[i];
    ts[i].rx.size = sizeof pool[i];
  }
  pico_init(txpool, 4096, 0x1234);
  be = &be_pico;
  deadline = world_us + 3600ull * 1000000;

  x_name_connect();
  x_bulk(1);
  settle("bulk");
  {
    int bad = x_many(16, 32768);
    CHECK("16 sockets at once, 32KB echo each (nonblocking, poll by socklen): intact", bad == 0);
    printf("  16 x 32KB echo: %.2f s virtual\n", secs());
    settle("many");
  }
  x_fair();
  settle("fairness");
  x_listen();
  x_rcmd();
  x_ssh();
  x_halfclose();
  x_errors();
  x_udp();
  x_loss();
  CHECK("tcp_write never failed (CREDIT kept within lwIP's send buffer and queue)", world_errmem() == 0);
  printf("  Pico held at most %u bytes of received data not yet given to the Z, %u bytes in lwIP send queues\n",
         world_held_peak(), world_sndq_peak());
  world_stat();
  printf("  Z: exchanges %u (with data %llu) resend %u stale %u/%u netdown %u overrun %u\n", pstat.xchg,
         (unsigned long long)nx_data, pstat.resend, pstat.stale_rec, pstat.stale, pstat.netdown, pstat.overrun);
  CHECK("Z ring never overran (WINDOW respected)", pstat.overrun == 0);
  x_reboot();
  x_notsp();
  x_raw();
  if (!quick)
    throughput();
  printf("done: %d pass, %d fail\n", passes, fails);
  return fails != 0;
}
