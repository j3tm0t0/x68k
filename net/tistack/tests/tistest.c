/*
 * tistest.x -- tis.x (ループバック) を libsocket から試す。1 本のプログラムの中で、ノンブロッキングの
 * ソケットを交互に動かして両端を受け持つ (スレッドを使わない。スタックの不具合とスレッドの不具合を分けるため)
 *
 *   tistest.x [-q]     結果を 1 行ずつ "PASS ..." / "FAIL ..." で出し、最後に "done: N pass, M fail"
 *
 * libsocket は xip.x でも tis.x でも同じに使えるが、ここに並べた振る舞い (ノンブロッキング connect、
 * 4096 バイトを超える write の部分書き込み、EOF の通知) は tis.x の約束 (README.md)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <x68k/dos.h>

static int npass, nfail;

static void check(int ok, const char *what)
{
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (ok) npass++; else nfail++;
}

static struct sockaddr_in lo(int port)
{
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  return a;
}

static void nonblock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int listener(int port, int backlog)
{
  struct sockaddr_in a = lo(port);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  a.sin_addr.s_addr = 0;
  if (s < 0)
    return -1;
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, backlog) < 0) {
    close(s);
    return -1;
  }
  return s;
}

/* ノンブロッキングの connect と accept で 1 本つなぐ。*cl = クライアント側、戻り値 = サーバ側 */
static int pair(int ls, int port, int *cl)
{
  struct sockaddr_in a = lo(port);
  struct pollfd p[2];
  int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), s, r, err = -1;
  socklen_t el = sizeof err;
  *cl = -1;
  if (c < 0)
    return -1;
  r = connect(c, (struct sockaddr *)&a, sizeof a);
  if (!(r == 0 || errno == EINPROGRESS))
    return -1;
  /* listen ソケットの poll は 1 回だけ: libsocket の poll は accept を先に済ませて取っておき、
     取ってある間は listen ソケットに POLLIN を出さない */
  p[0].fd = c;  p[0].events = POLLOUT;
  p[1].fd = ls; p[1].events = POLLIN;
  if (poll(p, 1, 2000) != 1 || !(p[0].revents & POLLOUT))
    return -1;
  getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
  if (err != 0)
    return -1;
  if (poll(p + 1, 1, 2000) != 1)
    return -1;
  s = accept(ls, 0, 0);
  if (s < 0)
    return -1;
  nonblock(s);
  *cl = c;
  return s;
}

/* 決まった並び (LCG) で buf を埋める / 確かめる */
static unsigned long pat(unsigned long *st)
{
  *st = *st * 1103515245u + 12345u;
  return *st >> 16;
}

/* 位置 i の中身 (向きごとに seed を変える)。68000 は 32 ビットの掛け算が遅いので足し算とシフトだけ */
static unsigned char bpat(long i, int seed)
{
  return (unsigned char)(i + (i >> 8) * 3 + (i >> 16) * 5 + seed);
}

/* a→b に na バイト、b→a に nb バイトを同時に流し、中身を確かめる。chunk = 1 回の write の最大 */
static int bulk(int a, int b, long na, long nb, int chunk, long *maxw)
{
  static unsigned char buf[16384], rb[16384];
  long wa = 0, wb = 0, ga = 0, gb = 0;
  long t0 = _dos_time_pr();
  int k;
  while (ga < na || gb < nb) {
    int n;
    if (_dos_time_pr() - t0 > 120000)
      return -1;
    /* 書く (ノンブロッキング。書けた分だけ進める) */
    if (wa < na) {
      int m = na - wa < chunk ? na - wa : chunk;
      for (k = 0; k < m; k++) buf[k] = bpat(wa + k, 1);
      n = write(a, buf, m);
      if (n > 0) {
        if (n > *maxw) *maxw = n;
        wa += n;
      } else if (n < 0 && errno != EAGAIN)
        return -2;
    }
    if (wb < nb) {
      int m = nb - wb < chunk ? nb - wb : chunk;
      for (k = 0; k < m; k++) buf[k] = bpat(wb + k, 2);
      n = write(b, buf, m);
      if (n > 0)
        wb += n;
      else if (n < 0 && errno != EAGAIN)
        return -3;
    }
    /* 読む */
    n = read(b, rb, sizeof rb);
    if (n > 0) {
      for (k = 0; k < n; k++)
        if (rb[k] != bpat(ga + k, 1))
          return -4;
      ga += n;
    } else if (n == 0 && ga < na)
      return -5;
    n = read(a, rb, sizeof rb);
    if (n > 0) {
      for (k = 0; k < n; k++)
        if (rb[k] != bpat(gb + k, 2))
          return -6;
      gb += n;
    } else if (n == 0 && gb < nb)
      return -7;
  }
  return 0;
}

static int read_all(int fd, char *buf, int max, int ms)
{
  long t0 = _dos_time_pr();
  int got = 0;
  struct pollfd p;
  p.fd = fd; p.events = POLLIN;
  while (got < max && _dos_time_pr() - t0 < ms) {
    int n;
    if (poll(&p, 1, 100) <= 0)
      continue;
    n = read(fd, buf + got, max - got);
    if (n == 0)
      break;
    if (n > 0)
      got += n;
  }
  return got;
}

int main(int argc, char **argv)
{
  int ls, s, c, r, i;
  char buf[256], msg[128];
  long maxw = 0;
  setvbuf(stdout, NULL, _IONBF, 0);

  /* 名前 */
  {
    struct hostent *h = gethostbyname("localhost");
    check(h && h->h_length == 4 && ((unsigned char *)h->h_addr_list[0])[0] == 127, "gethostbyname localhost");
    struct servent *se = getservbyname("ssh", "tcp");
    check(se && se->s_port == 22, "getservbyname ssh");
  }

  /* つなぐ (ノンブロッキング connect → poll POLLOUT → SO_ERROR → poll POLLIN の listen → accept) */
  ls = listener(5000, 5);
  check(ls >= 128, "socket/bind/listen 5000");
  {
    struct sockaddr_in a = lo(5000);
    struct pollfd p;
    int err = -1;
    socklen_t el = sizeof err;
    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    r = connect(c, (struct sockaddr *)&a, sizeof a);
    check(r == -1 && errno == EINPROGRESS, "nonblocking connect -> EINPROGRESS");
    p.fd = c; p.events = POLLOUT;
    check(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT), "poll POLLOUT after connect");
    getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
    check(err == 0, "SO_ERROR == 0");
    p.fd = ls; p.events = POLLIN;
    check(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "poll POLLIN on listener");
    {
      struct sockaddr_in pa;
      socklen_t pl = sizeof pa;
      s = accept(ls, (struct sockaddr *)&pa, &pl);
      check(s >= 128 && s != ls && pa.sin_addr.s_addr == htonl(0x7f000001), "accept (peer 127.0.0.1)");
    }
    {
      struct sockaddr_in na, pa;
      socklen_t l1 = sizeof na, l2 = sizeof pa;
      getsockname(c, (struct sockaddr *)&na, &l1);
      getpeername(s, (struct sockaddr *)&pa, &l2);
      check(na.sin_port == pa.sin_port && ntohs(na.sin_port) >= 1024, "getsockname(client) == getpeername(server)");
    }
  }

  /* エコー (サーバ側はブロッキングのまま: accept の fd は libsocket がブロッキングにする) */
  r = write(c, "hello\n", 6);
  check(r == 6, "write 6");
  r = read(s, buf, sizeof buf);     /* ブロッキング read。データがあるので即戻る */
  check(r == 6 && !memcmp(buf, "hello\n", 6), "blocking read gets data");
  r = write(s, buf, r);
  check(r == 6, "echo back");
  r = read_all(c, buf, 6, 2000);
  check(r == 6 && !memcmp(buf, "hello\n", 6), "client reads echo");
  r = read(c, buf, sizeof buf);
  check(r == -1 && errno == EAGAIN, "nonblocking read empty -> EAGAIN");

  /* 4096 バイトを超える write: 部分書き込みでその数を返す (xip.x は -1/EAGAIN を返していた) */
  {
    static char big[20000];
    int sl = -9;
    socklen_t ol = sizeof sl;
    memset(big, 'x', sizeof big);
    r = write(c, big, sizeof big);
    sprintf(msg, "nonblocking write 20000 -> partial %d (>4096, <20000)", r);
    check(r > 4096 && r < 20000, msg);
    i = write(c, big, sizeof big);
    check(i == -1 && errno == EAGAIN, "write when full -> EAGAIN");
    {
      struct pollfd p;
      p.fd = c; p.events = POLLOUT;
      check(poll(&p, 1, 100) == 0, "no POLLOUT when peer buffer full");
    }
    getsockopt(s, SOL_SOCKET, SO_SOCKLENRECV, &sl, &ol);
    check(sl == r, "SO_SOCKLENRECV == bytes queued");
    nonblock(s);
    i = read_all(s, big, sizeof big, 2000);
    check(i == r, "drain");
  }

  /* 大量の小さい write (1〜7 バイトを 5000 回) */
  {
    unsigned long st = 7, rs = 7;
    long sent = 0, got = 0;
    int bad = 0, k, n;
    static unsigned char rb[4096];
    for (i = 0; i < 5000; i++) {
      unsigned char w[8];
      int m = 1 + i % 7;
      for (k = 0; k < m; k++) w[k] = pat(&st);
      if (write(c, w, m) != m) { bad = 1; break; }
      sent += m;
      if (i % 100 == 99)
        while ((n = read(s, rb, sizeof rb)) > 0) {
          for (k = 0; k < n; k++) if (rb[k] != (unsigned char)pat(&rs)) bad = 2;
          got += n;
        }
    }
    while ((n = read(s, rb, sizeof rb)) > 0) {
      for (k = 0; k < n; k++) if (rb[k] != (unsigned char)pat(&rs)) bad = 2;
      got += n;
    }
    sprintf(msg, "5000 small writes (%ld bytes) bad=%d", sent, bad);
    check(!bad && got == sent, msg);
  }

  /* 300KB を両方向に同時に (中身を確かめる) */
  {
    long t0 = _dos_time_pr(), dt;
    r = bulk(c, s, 300000, 300000, 8192, &maxw);
    dt = _dos_time_pr() - t0;
    sprintf(msg, "bulk 300000 both ways r=%d in %ld ms (max write %ld)", r, dt, maxw);
    check(r == 0, msg);
  }

  /* poll に stdin とソケット: 何も無ければ時間切れ、相手が書いたらソケットだけ */
  {
    struct pollfd p[2];
    long t0 = _dos_time_pr(), dt;
    p[0].fd = 0; p[0].events = POLLIN;
    p[1].fd = c; p[1].events = POLLIN;
    r = poll(p, 2, 300);
    dt = _dos_time_pr() - t0;
    sprintf(msg, "poll stdin+sock timeout r=%d dt=%ld", r, dt);
    check(r == 0 && dt >= 300, msg);
    write(s, "ping", 4);
    r = poll(p, 2, 1000);
    check(r == 1 && (p[1].revents & POLLIN) && !(p[0].revents & POLLIN), "poll stdin+sock -> sock POLLIN");
    read(c, buf, sizeof buf);
  }

  /* 片側を閉じる (shutdown SHUT_WR): 相手は残りを読んでから EOF、逆向きはまだ使える */
  {
    struct pollfd p;
    char st[32];
    socklen_t sl = sizeof st;
    write(c, "last", 4);
    shutdown(c, SHUT_WR);
    r = read_all(s, buf, sizeof buf, 1000);
    check(r == 4 && !memcmp(buf, "last", 4), "data before FIN");
    r = read(s, buf, sizeof buf);
    check(r == 0, "read -> 0 (EOF) after peer shutdown");
    p.fd = s; p.events = POLLIN;
    check(poll(&p, 1, 100) == 1, "poll wakes at EOF");
    getsockopt(s, SOL_SOCKET, SO_SOCKSTATE, st, &sl);
    sprintf(msg, "server state after FIN = [%s]", st);
    check(!strcmp(st, "CLOSE WAIT"), msg);
    r = write(s, "bye", 3);
    check(r == 3, "half-closed: reverse direction still writes");
    r = read_all(c, buf, 3, 1000);
    check(r == 3 && !memcmp(buf, "bye", 3), "half-closed: client reads");
    close(s);
    r = read(c, buf, sizeof buf);
    check(r == 0, "read -> 0 after peer close");
    r = write(c, "x", 1);
    check(r == -1 && errno == EIO, "write after peer close -> EIO");
    close(c);
  }

  /* ブロッキング read が EOF で 0 を返す (xip.x では固まった道) */
  s = pair(ls, 5000, &c);
  check(s >= 0, "second connection");
  {
    int fl = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    close(c);
    r = read(s, buf, sizeof buf);
    check(r == 0, "blocking read -> 0 at EOF");
    close(s);
  }

  /* つながらない: listen の無いポート */
  {
    struct sockaddr_in a = lo(5999);
    int x = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(x, (struct sockaddr *)&a, sizeof a);
    check(r == -1 && errno == EIO, "connect refused -> -1 (EIO)");
    close(x);
    a.sin_addr.s_addr = htonl(0x0a000001);
    x = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(x, (struct sockaddr *)&a, sizeof a);
    check(r == -1, "connect 10.0.0.1 -> -1 (loopback only)");
    close(x);
  }

  /* ブロッキング connect が 0 を返す */
  {
    struct sockaddr_in a = lo(5000);
    int x = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(x, (struct sockaddr *)&a, sizeof a);
    check(r == 0, "blocking connect -> 0");
    s = accept(ls, 0, 0);
    check(s >= 128, "blocking accept");
    close(x);
    close(s);
  }

  /* 閉じたポートにすぐ bind し直せる (rcmd.x は 1 接続ごとに listen し直す) */
  close(ls);
  ls = listener(5000, 0);
  check(ls >= 128, "re-bind 5000 right after close");
  {
    int ls2 = listener(5000, 1);
    check(ls2 < 0, "bind to a listening port fails");
    if (ls2 >= 0) close(ls2);
  }

  /* backlog 0 (rcmd.x の流儀): listen ソケット自身が接続になり、accept は同じ番号 */
  {
    struct sockaddr_in a = lo(5000);
    char st[32];
    socklen_t sl = sizeof st;
    int x = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(x, (struct sockaddr *)&a, sizeof a);
    getsockopt(ls, SOL_SOCKET, SO_SOCKSTATE, st, &sl);
    sprintf(msg, "backlog 0: listener state [%s]", st);
    check(r == 0 && !strcmp(st, "ESTABLISHED"), msg);
    s = accept(ls, 0, 0);
    check(s == ls, "backlog 0: accept returns the listener itself");
    write(x, "abc", 3);
    r = read(s, buf, sizeof buf);
    check(r == 3, "backlog 0: data");
    close(x);
    close(s);
  }

  /* UDP */
  {
    struct sockaddr_in a = lo(5353), from;
    socklen_t fl = sizeof from;
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_addr.s_addr = 0;
    bind(u1, (struct sockaddr *)&a, sizeof a);
    a = lo(5353);
    r = sendto(u2, "dgram1", 6, 0, (struct sockaddr *)&a, sizeof a);
    sendto(u2, "dg2", 3, 0, (struct sockaddr *)&a, sizeof a);
    check(r == 6, "udp sendto");
    r = recvfrom(u1, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    check(r == 6 && !memcmp(buf, "dgram1", 6) && from.sin_addr.s_addr == htonl(0x7f000001), "udp recvfrom 1");
    r = recvfrom(u1, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    check(r == 3 && !memcmp(buf, "dg2", 3), "udp recvfrom 2 (boundaries kept)");
    close(u1);
    close(u2);
  }

  /* 開いたままのソケットが残らない */
  {
    int k, n = 0;
    for (k = 0; k < 32; k++) {
      char st[32];
      socklen_t sl = sizeof st;
      if (getsockopt(128 + k, SOL_SOCKET, SO_SOCKSTATE, st, &sl) == 0)
        n++;
    }
    sprintf(msg, "no sockets left open (%d)", n);
    check(n == 0, msg);
  }
  (void)argc; (void)argv;
  printf("done: %d pass, %d fail\n", npass, nfail);
  return nfail ? 1 : 0;
}
