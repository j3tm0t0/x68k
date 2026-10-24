/*
 * rcc.x -- gcc の前置き: 入力のファイルを LAN の向こうの rccd.py (elf2x68k の gcc) に送ってコンパイルし、結果を Z の gcc と
 * 同じ場所に書く。引数は Z の gcc と同じ (-O、-o、-c、-S、-E、-I、-D、-U、-l、-L、-g、-W など)
 *
 *   rcc.x -O hello.c -o hello.x          zosh の gcc() から呼ぶ (/etc/zoshrc)
 *
 * 相手は環境変数 RCC_HOST (名前か a.b.c.d、":ポート" も可。既定のポートは 7790)。TCP は xip.x でも tis.x -p でも動く。
 * 送るもの: 引数の入力 (.c .cc .cpp .s .S .o .a)、-include のファイル、今のディレクトリ・ソースのディレクトリ・-I の
 * ディレクトリの *.h。どのファイルも Z で書いたとおりのパスで送り、メッセージの中のパスも Z の書き方で返ってくる。
 * つながらない (RCC_HOST が無い・相手が居ない・途中で切れた) ときは 1 行だけ知らせて Z の gcc に戻す:
 * 環境変数 RCC_LOCAL (既定 "gcc.x %s -lgcc2"、%s に引数をそのまま入れる。空にすると戻さずに 255 で終わる) を system() で。
 * 戻り値: コンパイラの終了コード
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
#define MAXF        128

static char *fname[MAXF];                   /* 送るファイル (Z のパスのまま) */
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

static int is_input(const char *p)
{
  static const char *ext[] = { ".c", ".cc", ".cpp", ".s", ".o", ".a", ".h", NULL };
  for (int i = 0; ext[i]; i++)
    if (has_ext(p, ext[i]))
      return 1;
  return 0;
}

static void add_file(const char *p)
{
  for (int i = 0; i < nf; i++)
    if (strcasecmp(fname[i], p) == 0)
      return;
  if (nf < MAXF)
    fname[nf++] = strdup(p);
}

/* dir の先頭 dl バイト ("" なら今のディレクトリ。末尾の区切りは有っても無くてもよい) の *.h を足す */
static void add_headers(const char *dir, size_t dl)
{
  char pat[256], p[256];
  struct dos_filbuf fb;
  if (dl >= sizeof pat - 8)
    return;
  memcpy(pat, dir, dl);
  if (dl > 0 && pat[dl - 1] != '\\' && pat[dl - 1] != '/' && pat[dl - 1] != ':')
    pat[dl++] = '\\';
  strcpy(pat + dl, "*.h");
  if (_dos_files(&fb, pat, 0x20) < 0)
    return;
  do {
    memcpy(p, pat, dl);
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

static int sendstr(const char *kw, const char *s)
{
  return sendhdr(kw, strlen(s), NULL) < 0 || sendall(s, strlen(s)) < 0 ? -1 : 0;
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
  if (fd < 0)
    return 0;                               /* 無いファイルは送らない (gcc が「無い」と言う) */
  size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  if (sendhdr("FILE", size, path) < 0)
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

/* n バイトを受けて fp に写す (fp が NULL なら捨てる) */
static int copyout(FILE *fp, long n)
{
  static char buf[4096];
  while (n > 0) {
    int k = n < (long)sizeof buf ? n : (long)sizeof buf;
    if (recvall(buf, k) < 0)
      return -1;
    if (fp)
      fwrite(buf, 1, k, fp);
    n -= k;
  }
  return 0;
}

static int recvfile(const char *path, long n)
{
  static char buf[8192];
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0755);
  if (fd < 0) {
    fprintf(stderr, "rcc: %s: cannot create\n", path);
    return copyout(NULL, n);
  }
  while (n > 0) {
    int k = n < (long)sizeof buf ? n : (long)sizeof buf;
    if (recvall(buf, k) < 0 || write(fd, buf, k) != k) {
      close(fd);
      unlink(path);
      return -1;
    }
    n -= k;
  }
  close(fd);
  return 0;
}

/* 受け手が作った .o (ELF) か。Z の HLK.X はリンクできない */
static int is_elf(const char *path)
{
  unsigned char m[4];
  int fd, n;
  if (!has_ext(path, ".o"))
    return 0;
  if ((fd = open(path, O_RDONLY | O_BINARY)) < 0)
    return 0;
  n = read(fd, m, 4);
  close(fd);
  return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
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
  fprintf(stderr, "rcc: no compile server, compiling here\n");
  for (int i = 1; i < argc; i++)
    if (is_elf(argv[i]))
      fprintf(stderr, "rcc: %s was made by the compile server (ELF); HLK can't link it, rebuild it here\n", argv[i]);
  return system(cmd);
}

int main(int argc, char **argv)
{
  char line[300], cwd[256];
  const char *e;
  int rc, nout;
  long outlen, errlen;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if ((strcmp(a, "-I") == 0 || strcmp(a, "-include") == 0) && i + 1 < argc) {
      if (a[2] == 0)
        add_headers(argv[i + 1], strlen(argv[i + 1]));
      else
        add_file(argv[i + 1]);
      i++;
    } else if (strncmp(a, "-I", 2) == 0) {
      add_headers(a + 2, strlen(a + 2));
    } else if (a[0] == '-') {
      if ((strcmp(a, "-o") == 0 || strcmp(a, "-D") == 0 || strcmp(a, "-U") == 0 || strcmp(a, "-L") == 0 ||
           strcmp(a, "-x") == 0) && i + 1 < argc)
        i++;
    } else if (is_input(a)) {
      add_file(a);
      add_headers(a, base(a) - a);
    }
  }
  add_headers("", 0);

  if (dial() < 0)
    return local(argc, argv);
  if (!getcwd(cwd, sizeof cwd))
    cwd[0] = 0;
  if (sendall("RCC2\n", 5) < 0 || sendstr("CWD", cwd) < 0)
    goto lost;
  if ((e = getenv("RCC_SYS")) && *e) {
    snprintf(line, sizeof line, "SYS %s\n", e);
    if (sendall(line, strlen(line)) < 0)
      goto lost;
  }
  for (int i = 1; i < argc; i++)
    if (sendstr("ARG", argv[i]) < 0)
      goto lost;
  for (int i = 0; i < nf; i++)
    if (sendfile_(fname[i]) < 0)
      goto lost;
  if (sendall("END\n", 4) < 0)
    goto lost;

  if (recvline(line, sizeof line) < 0 || sscanf(line, "RCC2 %d %ld %ld %d", &rc, &outlen, &errlen, &nout) != 4)
    goto lost;
  if (copyout(stdout, outlen) < 0 || copyout(stderr, errlen) < 0)
    goto lost;
  fflush(stdout);
  for (int i = 0; i < nout; i++) {
    long n;
    char *p;
    if (recvline(line, sizeof line) < 0 || strncmp(line, "OUT ", 4) != 0)
      goto lost;
    n = strtol(line + 4, &p, 10);
    if (*p == ' ')
      p++;
    p[strcspn(p, "\n")] = 0;
    if (recvfile(p, n) < 0)
      goto lost;
  }
  close(sock);
  return rc;

lost:
  if (sock >= 0)
    close(sock);
  return local(argc, argv);
}
