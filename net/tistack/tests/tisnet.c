/*
 * tisnet.x -- 本物のネットワーク越しの試験 (libsocket。tis.x -p でも xip.x でも動く)
 *
 *   tisnet.x resolve 名前...          gethostbyname
 *   tisnet.x put ホスト ポート バイト数  "PUT n\n" + 型の n バイトを送り、相手の "OK n bad b" を受ける
 *   tisnet.x get ホスト ポート バイト数  "GET n\n" を送り、型の n バイトを受けて照合する (相手が閉じるまで)
 *   tisnet.x echo ホスト ポート 回数     "ECHO\n" のあと 1 バイトずつ往復させて時間を測る
 *   tisnet.x sink / src ホスト ポート バイト数   中身を見ない送り / 受け (速さだけ。型を作る・照合する Z の手間を除く)
 *
 * 相手は tests/tisnet_peer.py (Mac)。型は picosim の試験と同じ pat(k)。結果は PASS / FAIL の行
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <x68k/dos.h>

static int fails;
#define CHECK(name, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); fflush(stdout); if (!ok_) fails++; } while (0)

static unsigned char pat(unsigned long k) { return (unsigned char)(k * 7 + (k >> 8) + (k >> 16)); }

static int dial(const char *host, int port)
{
  struct sockaddr_in sa;
  struct hostent *h;
  int s;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_aton(host, &sa.sin_addr) == 0) {
    if (!(h = gethostbyname(host)))
      return -1;
    memcpy(&sa.sin_addr, h->h_addr_list[0], 4);
  }
  if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return -1;
  if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
    close(s);
    return -1;
  }
  return s;
}

static int wall(int s, const void *b, int n)
{
  int k, r;
  for (k = 0; k < n; k += r)
    if ((r = write(s, (const char *)b + k, n - k)) <= 0)
      return -1;
  return n;
}

static int readline(int s, char *b, int max)
{
  int n = 0;
  while (n < max - 1 && read(s, b + n, 1) == 1 && b[n] != '\n')
    n++;
  b[n] = 0;
  return n;
}

static char buf[8192];

int main(int argc, char **argv)
{
  char msg[160];
  int i;
  if (argc < 2)
    return 1;
  if (!strcmp(argv[1], "resolve")) {
    for (i = 2; i < argc; i++) {
      long t0 = _dos_time_pr();
      struct hostent *h = gethostbyname(argv[i]);
      if (h) {
        struct in_addr a;
        memcpy(&a, h->h_addr_list[0], 4);
        sprintf(msg, "resolve %s -> %s (%ld ms)", argv[i], inet_ntoa(a), _dos_time_pr() - t0);
      } else
        sprintf(msg, "resolve %s -> (none) (%ld ms)", argv[i], _dos_time_pr() - t0);
      CHECK(msg, h != 0);
    }
  } else if (argc >= 5 && !strcmp(argv[1], "put")) {
    unsigned long n = strtoul(argv[4], 0, 10), k;
    long t0 = _dos_time_pr(), dt;
    int s = dial(argv[2], atoi(argv[3]));
    char line[80] = "";
    sprintf(msg, "connect %s:%s", argv[2], argv[3]);
    CHECK(msg, s >= 0);
    if (s < 0)
      return 1;
    sprintf(line, "PUT %lu\n", n);
    wall(s, line, strlen(line));
    t0 = _dos_time_pr();
    for (k = 0; k < n; ) {
      int m = n - k < sizeof buf ? n - k : sizeof buf, j;
      for (j = 0; j < m; j++)
        buf[j] = pat(k + j);
      if (wall(s, buf, m) < 0)
        break;
      k += m;
    }
    readline(s, line, sizeof line);
    dt = _dos_time_pr() - t0;
    close(s);
    sprintf(msg, "put %lu bytes: peer says [%s], %ld ms, %ld B/s", n, line, dt, dt ? (long)(n * 1000.0 / dt) : 0);
    {
      char ok[40];
      sprintf(ok, "OK %lu bad 0", n);
      CHECK(msg, k == n && !strcmp(line, ok));
    }
  } else if (argc >= 5 && !strcmp(argv[1], "get")) {
    unsigned long n = strtoul(argv[4], 0, 10), got = 0, bad = 0;
    long t0, dt;
    int s = dial(argv[2], atoi(argv[3])), r, j;
    char line[40];
    sprintf(msg, "connect %s:%s", argv[2], argv[3]);
    CHECK(msg, s >= 0);
    if (s < 0)
      return 1;
    sprintf(line, "GET %lu\n", n);
    t0 = _dos_time_pr();
    wall(s, line, strlen(line));
    while ((r = read(s, buf, sizeof buf)) > 0) {
      for (j = 0; j < r; j++)
        if ((unsigned char)buf[j] != pat(got + j))
          bad++;
      got += r;
    }
    dt = _dos_time_pr() - t0;
    close(s);
    sprintf(msg, "get %lu bytes: got %lu bad %lu, %ld ms, %ld B/s", n, got, bad, dt, dt ? (long)(got * 1000.0 / dt) : 0);
    CHECK(msg, got == n && bad == 0 && r == 0);
  } else if (argc >= 5 && (!strcmp(argv[1], "sink") || !strcmp(argv[1], "src"))) {
    unsigned long n = strtoul(argv[4], 0, 10), k = 0;
    int s = dial(argv[2], atoi(argv[3])), r, src = argv[1][1] == 'r';
    long t0, dt;
    char line[40];
    if (s < 0) {
      CHECK("connect", 0);
      return 1;
    }
    sprintf(line, src ? "SRC %lu\n" : "SINK %lu\n", n);
    wall(s, line, strlen(line));
    t0 = _dos_time_pr();
    if (src) {
      while ((r = read(s, buf, sizeof buf)) > 0)
        k += r;
      line[0] = 0;
    } else {
      for (; k < n; k += r)
        if ((r = write(s, buf, n - k < sizeof buf ? n - k : sizeof buf)) <= 0)
          break;
      readline(s, line, sizeof line);
    }
    dt = _dos_time_pr() - t0;
    close(s);
    sprintf(msg, "%s %lu bytes (not checked): %lu, %ld ms, %ld B/s %s", argv[1], n, k, dt, dt ? (long)(k * 1000.0 / dt) : 0, line);
    CHECK(msg, k == n);
  } else if (argc >= 5 && !strcmp(argv[1], "echo")) {
    int n = atoi(argv[4]), s = dial(argv[2], atoi(argv[3])), k, ok = 0;
    long t0, dt;
    sprintf(msg, "connect %s:%s", argv[2], argv[3]);
    CHECK(msg, s >= 0);
    if (s < 0)
      return 1;
    wall(s, "ECHO\n", 5);
    t0 = _dos_time_pr();
    for (k = 0; k < n; k++) {
      char c = 'a' + k % 26, d = 0;
      write(s, &c, 1);
      if (read(s, &d, 1) == 1 && d == c)
        ok++;
    }
    dt = _dos_time_pr() - t0;
    close(s);
    sprintf(msg, "echo %d x 1 byte: %d ok, %ld ms avg round trip", n, ok, n ? dt / n : 0);
    CHECK(msg, ok == n);
  }
  return fails != 0;
}
