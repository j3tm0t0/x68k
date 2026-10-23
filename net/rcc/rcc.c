/*
 * rcc.x -- C のソースを LAN の向こうの rccd.py (elf2x68k の gcc) に送ってコンパイル・リンクし、できた .x を受け取る
 *
 *   rcc.x [gcc の引数とソース ...]        例: rcc.x -O2 hello.c -o hello.x
 *
 * 相手は環境変数 RCC_HOST (名前か a.b.c.d、":ポート" も可。既定のポートは 7790)。TCP は xip.x でも tis.x -p でも動く。
 * 送るもの: 引数のうち .c / .s / .h のファイル (名前だけにして送る) と、そのファイルと同じディレクトリの *.h 全部。
 * つながらない (RCC_HOST が無い・相手が居ない・途中で切れた) ときは Z の gcc に戻す: 環境変数 RCC_LOCAL
 * (既定 "gcc.x %s -lgcc2"、%s に引数をそのまま入れる。空にすると戻さずに 255 で終わる) を system() で動かす。
 * 戻り値: gcc の終了コード
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <x68k/dos.h>

#define UNREACHABLE 255
#define MAXF        64

static char *fname[MAXF];                   /* 送るファイルのパス */
static int nf;
static int sock = -1;

static const char *base(const char *p)
{
  const char *b = p;
  for (; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':')
      b = p + 1;
  return b;
}

static int has_ext(const char *p, const char *ext)
{
  size_t n = strlen(p), m = strlen(ext);
  return n > m && strcasecmp(p + n - m, ext) == 0;
}

static void add_file(const char *p)
{
  for (int i = 0; i < nf; i++)
    if (strcasecmp(base(fname[i]), base(p)) == 0)
      return;
  if (nf < MAXF)
    fname[nf++] = strdup(p);
}

/* ソースと同じディレクトリの *.h を足す (#include "x.h" のため) */
static void add_headers(const char *src)
{
  char pat[256];
  struct dos_filbuf fb;
  size_t dl = base(src) - src;
  if (dl >= sizeof pat - 8)
    return;
  memcpy(pat, src, dl);
  strcpy(pat + dl, "*.h");
  if (_dos_files(&fb, pat, 0x20) < 0)
    return;
  do {
    char p[256];
    memcpy(p, src, dl);
    strcpy(p + dl, fb.name);
    add_file(p);
  } while (_dos_nfiles(&fb) >= 0);
}

static int sendall(const void *b, int n)
{
  const char *p = b;
  while (n > 0) {
    int r = send(sock, p, n, 0);
    if (r <= 0)
      return -1;
    p += r;
    n -= r;
  }
  return 0;
}

static int sendhdr(const char *kw, long n, const char *name)
{
  char h[300];
  int l = name ? sprintf(h, "%s %ld %s\n", kw, n, name) : sprintf(h, "%s %ld\n", kw, n);
  return sendall(h, l);
}

static int recvall(void *b, int n)
{
  char *p = b;
  while (n > 0) {
    int r = recv(sock, p, n, 0);
    if (r <= 0)
      return -1;
    p += r;
    n -= r;
  }
  return 0;
}

static int recvline(char *b, int size)
{
  int n = 0;
  while (n < size - 1) {
    if (recv(sock, b + n, 1, 0) != 1)
      return -1;
    if (b[n++] == '\n')
      break;
  }
  b[n] = 0;
  return n;
}

static int dial(void)
{
  char host[128], *c;
  const char *e = getenv("RCC_HOST");
  int port = 7790;
  struct sockaddr_in sa;
  struct hostent *h;
  if (!e || !*e)
    return -1;
  snprintf(host, sizeof host, "%s", e);
  if ((c = strchr(host, ':')) != NULL) {
    *c = 0;
    port = atoi(c + 1);
  }
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if ((sa.sin_addr.s_addr = inet_addr(host)) == (in_addr_t)-1) {
    if (!(h = gethostbyname(host)))
      return -1;
    memcpy(&sa.sin_addr, h->h_addr, 4);
  }
  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return -1;
  if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
    close(sock);
    sock = -1;
    return -1;
  }
  return 0;
}

static int sendfile_(const char *path)
{
  static char buf[8192];
  int fd = open(path, O_RDONLY | O_BINARY);
  long size;
  if (fd < 0) {
    fprintf(stderr, "rcc: %s: cannot open\n", path);
    return -1;
  }
  size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (sendhdr("FILE", size, base(path)) < 0)
    return -1;
  while (size > 0) {
    int n = read(fd, buf, size < (long)sizeof buf ? size : (long)sizeof buf);
    if (n <= 0 || sendall(buf, n) < 0) {
      close(fd);
      return -1;
    }
    size -= n;
  }
  close(fd);
  return 0;
}

/* つながらないとき: Z の gcc で同じ引数のまま */
static int local(int argc, char **argv)
{
  static char cmd[1024], a[900];
  const char *fmt = getenv("RCC_LOCAL");
  int n = 0;
  if (!fmt)
    fmt = "gcc.x %s -lgcc2";
  if (!*fmt)
    return UNREACHABLE;
  for (int i = 1; i < argc && n < (int)sizeof a - 2; i++)
    n += snprintf(a + n, sizeof a - n, "%s%s", i > 1 ? " " : "", argv[i]);
  snprintf(cmd, sizeof cmd, fmt, a);
  fprintf(stderr, "rcc: compiling here: %s\n", cmd);
  return system(cmd);
}

int main(int argc, char **argv)
{
  const char *out = "a.x";
  char line[128];
  int rc, loglen, outlen, fd;
  static char buf[8192];

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
      out = argv[++i];
    else if (argv[i][0] != '-' && (has_ext(argv[i], ".c") || has_ext(argv[i], ".s") || has_ext(argv[i], ".h"))) {
      add_file(argv[i]);
      add_headers(argv[i]);
    }
  }
  if (dial() < 0) {
    fprintf(stderr, "rcc: cannot reach RCC_HOST=%s\n", getenv("RCC_HOST") ? getenv("RCC_HOST") : "");
    return local(argc, argv);
  }
  if (sendall("RCC1\n", 5) < 0)
    goto lost;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-o") == 0 && i + 1 < argc) {
      if (sendhdr("ARG", 2, NULL) < 0 || sendall("-o", 2) < 0)
        goto lost;
      a = base(argv[++i]);
    } else if (a[0] != '-' && (has_ext(a, ".c") || has_ext(a, ".s") || has_ext(a, ".h")))
      a = base(a);
    if (sendhdr("ARG", strlen(a), NULL) < 0 || sendall(a, strlen(a)) < 0)
      goto lost;
  }
  for (int i = 0; i < nf; i++)
    if (sendfile_(fname[i]) < 0)
      goto lost;
  if (sendall("END\n", 4) < 0)
    goto lost;

  if (recvline(line, sizeof line) < 0 || sscanf(line, "RCC1 %d %d %d", &rc, &loglen, &outlen) != 3)
    goto lost;
  while (loglen > 0) {                      /* gcc のメッセージ (UTF-8 のまま。英語なので読める) */
    int n = loglen < (int)sizeof buf ? loglen : (int)sizeof buf;
    if (recvall(buf, n) < 0)
      goto lost;
    fwrite(buf, 1, n, stderr);
    loglen -= n;
  }
  if (rc == 0 && outlen > 0) {
    if ((fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0755)) < 0) {
      fprintf(stderr, "rcc: %s: cannot create\n", out);
      return 1;
    }
    while (outlen > 0) {
      int n = outlen < (int)sizeof buf ? outlen : (int)sizeof buf;
      if (recvall(buf, n) < 0 || write(fd, buf, n) != n) {
        close(fd);
        unlink(out);
        goto lost;
      }
      outlen -= n;
    }
    close(fd);
  }
  close(sock);
  return rc;

lost:
  fprintf(stderr, "rcc: connection lost\n");
  if (sock >= 0)
    close(sock);
  return local(argc, argv);
}
