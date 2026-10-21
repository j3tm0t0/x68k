/*
 * x68k_shim.c -- dbclient (dropbear) を Human68k / elf2x68k newlib + libsocket に
 * 載せるための補完。dropbear 本体には手を入れず、ここで足りない関数を供給する。
 *
 *  - getuid/geteuid/getpwuid/getpwnam : newlib に実装が無い。固定ユーザを返す。
 *  - ioctl                            : 無い。-1 を返す (端末サイズ取得は失敗=既定値)。
 *  - select                           : libsocket の poll() で実装。dropbear の
 *                                       session_loop が使う。ソケット fd は 128..159
 *                                       なので FD_SETSIZE を広げてある (x68k_compat.h)。
 *  - pipe/fsync/execv/setsid          : シグナル self-pipe・ローカル実行等は使わない。スタブ。
 *  - basename/dirname                 : libgen のシンボルが無いので最小実装。
 *  - getaddrinfo/freeaddrinfo         : libsocket 版は数値 IP でも gethostbyname を
 *                                       呼び xip の resolver が返らないことがあるので、
 *                                       inet_addr で数値 IP を直接解釈する版で上書き。
 *  - x68k_connect                     : xip は非ブロッキング connect が不可 (EIO) なので、
 *                                       ブロッキング connect した接続済み fd を返す。
 *                                       cli-main.c (DROPBEAR_X68K) から呼ぶ。
 *  - x68k_get_entropy                 : /dev/urandom が無いので各種タイマを撹拌する種。
 *                                       dbrandom.c (DROPBEAR_X68K) から呼ぶ。**弱い**。
 *  - x68k_tty_setup/cleanup           : -t (pty) のとき VT100 エミュレータ (term/vt100) で画面を
 *                                       占有する / 戻す。cli-chansession.c の cli_tty_setup/cleanup から呼ぶ。
 *  - x68k_chan_write/x68k_chan_read   : チャネルの stdout/stderr 書き込みと stdin 読み出し。VT が
 *                                       有効なら vt_write / キーバッファに振り向け、無効なら素の write/read。
 *                                       common-channel.c (DROPBEAR_X68K) から呼ぶ。
 */
#include <sys/types.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x68k/iocs.h>
#include <x68k/dos.h>

/* 位相ごとのコンソール直接出力デバッグ。X68K_SSH_DEBUG を定義するとテキスト画面に
   「[  12.34] 印」 (最初の印からの経過秒, IOCS _ONTIME の 1/100 秒) を出す
   (rcmd のファイル redirect を迂回し contee.sys 経由で Pi の /tmp/x68con.log に残る)。
   既定では no-op。dropbear の -v (TRACE) は Z でバスエラーになった (原因未確認、%f 書式の疑い) ので使わない。 */
void x68dbg(const char *m)
{
#ifdef X68K_SSH_DEBUG
  static int t0 = -1;
  struct iocs_time t = _iocs_ontime();
  char b[24];
  int d;
  if (t0 < 0) t0 = t.sec;
  d = t.sec - t0;
  if (d < 0) d += 24 * 3600 * 100;
  sprintf(b, "[%4d.%02d] ", d / 100, d % 100);
  _iocs_b_print(b);
  _iocs_b_print(m);
  _iocs_b_print("\r\n");
#else
  (void)m;
#endif
}

/* --- ユーザ情報 --- */
uid_t getuid(void)  { return 1000; }
uid_t geteuid(void) { return 1000; }
gid_t getgid(void)  { return 1000; }
gid_t getegid(void) { return 1000; }

static struct passwd pw;
static char pw_name[32], pw_dir[128];
static struct passwd *fill_pw(void)
{
  const char *u = getenv("USER");
  const char *h = getenv("HOME");
  if (!u) u = "moto";
  if (!h) h = "B:/home";
  strncpy(pw_name, u, sizeof pw_name - 1); pw_name[sizeof pw_name - 1] = 0;
  strncpy(pw_dir, h, sizeof pw_dir - 1);   pw_dir[sizeof pw_dir - 1] = 0;
  memset(&pw, 0, sizeof pw);
  pw.pw_name = pw_name; pw.pw_passwd = ""; pw.pw_uid = 1000; pw.pw_gid = 1000;
  pw.pw_dir = pw_dir; pw.pw_shell = "";
  return &pw;
}
struct passwd *getpwuid(uid_t uid) { (void)uid; return fill_pw(); }
struct passwd *getpwnam(const char *name) { (void)name; return fill_pw(); }

/* --- VT100 端末 (term/vt100) --- */
#include "vt100.h"
#include <stdarg.h>
#include <errno.h>
#include <sys/ioctl.h>
static int vt_active;                 /* vt_init 済み (vt_init〜vt_fini の間は DOS コンソールに触らない) */
static unsigned char kbuf[256];       /* vt_key が吐いたバイト列 (ホストへ送るまで溜める) */
static int kbuf_len;

/* キーボードを非ブロッキングで読んでバッファに足す。VT が無効でも IOCS のキー読みは害が無いので
   同じ経路を使う (コマンドモードで stdin を要求されたときも同じ)。 */
static void kbuf_fill(void)
{
  if (kbuf_len < (int)sizeof kbuf)
    kbuf_len += vt_key(kbuf + kbuf_len, (int)sizeof kbuf - kbuf_len);
}

/* --- ioctl: TIOCGWINSZ だけ VT の寸法 (96x32) を返す。他は -1 --- */
int ioctl(int fd, unsigned long request, ...)
{
  (void)fd;
  if (request == TIOCGWINSZ) {
    va_list ap; struct winsize *ws; int c, r;
    va_start(ap, request); ws = va_arg(ap, struct winsize *); va_end(ap);
    vt_get_size(&c, &r);           /* vt_init 前でも既定の 96x32 が返る */
    if (ws) { ws->ws_col = (unsigned short)c; ws->ws_row = (unsigned short)r; ws->ws_xpixel = (unsigned short)(c * 8); ws->ws_ypixel = (unsigned short)(r * 16); }
    return 0;
  }
  return -1;
}

/* cli_tty_setup (pty 要求後、シェル要求の後) から: 画面を VT100 に切り替える */
void x68k_tty_setup(void)
{
  if (vt_active) return;
  vt_init(0, 0);
  vt_active = 1;
}

/* cli_tty_cleanup (チャネル close / 終了処理) から: コンソールを戻す。
   画面は消さない (直前の表示を残す)。IOCS のカーソルを VT の最終カーソル行の次に置き、
   続く "Connection ... closed." がその下に出るようにする。 */
void x68k_tty_cleanup(void)
{
  int x, y, cols, rows;
  if (!vt_active) return;
  vt_get_cursor(&x, &y);
  vt_get_size(&cols, &rows);
  vt_fini();
  vt_active = 0;
  if (x > 0) y++;
  if (y >= rows) { _iocs_b_locate(0, rows - 1); _iocs_b_print("\r\n"); }
  else _iocs_b_locate(0, y);
}

/* チャネルの fd 書き込み (writechannel_fallback から)。VT 有効時の stdout/stderr は画面へ */
int x68k_chan_write(int fd, const void *p, unsigned int n)
{
  if (vt_active && fd < X68K_SOCKFD_MIN) { vt_write((const unsigned char *)p, (int)n); return (int)n; }
  return (int)write(fd, p, n);
}

/* チャネルの fd 読み出し (send_msg_channel_data から)。stdin はキーバッファから。
   0 や EAGAIN を返すと dropbear は EOF 扱いで stdin を閉じるので、空なら -1/EINTR。 */
int x68k_chan_read(int fd, void *p, unsigned int n)
{
  if (fd < X68K_SOCKFD_MIN) {
    unsigned int k;
    kbuf_fill();
    if (kbuf_len == 0) { errno = EINTR; return -1; }
    k = (unsigned int)kbuf_len < n ? (unsigned int)kbuf_len : n;
    memcpy(p, kbuf, k);
    memmove(kbuf, kbuf + k, kbuf_len - k);
    kbuf_len -= k;
    return (int)k;
  }
  return (int)read(fd, p, n);
}

/* --- select() を libsocket の poll() で実装 ---
   ソケット以外の fd (標準入出力など、X68K_SOCKFD_MIN 未満) は poll に渡せないので
   「書き込みは常に可」として扱う (stdout への出力は同期書き)。
   stdin (fd 0) の読み取りはキーボード: poll を短い刻み (KEY_SLICE_MS) で回し、刻みの間に
   vt_key で IOCS のキーを読んでバッファに溜め、溜まっていれば fd 0 を「読める」と返す。
   libsocket の poll は内部で _CHANGE_PR を呼んで xip に CPU を譲るので、ここで自前に待たない。
   nfds==0 の poll は EINVAL なので、ソケットが無いとき (fd_read_pending(0) など) は poll を飛ばす。 */
#include <sys/select.h>
#include <poll.h>
#define KEY_SLICE_MS 50
int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *tv)
{
  static struct pollfd pfd[FD_SETSIZE];
  static int map[FD_SETSIZE];
  static fd_set wready;
  int i, n = 0, r = 0, count = 0, timeout, nw = 0, want_kbd = 0;
  long t0 = 0;
  FD_ZERO(&wready);
  for (i = 0; i < nfds && n < FD_SETSIZE; i++) {
    short ev = 0;
    if (i < X68K_SOCKFD_MIN) {
      if (wfds && FD_ISSET(i, wfds)) { FD_SET(i, &wready); nw++; }
      if (rfds && FD_ISSET(i, rfds) && i == 0) want_kbd = 1;
      continue;
    }
    if (rfds && FD_ISSET(i, rfds)) ev |= POLLIN;
    if (wfds && FD_ISSET(i, wfds)) ev |= POLLOUT;
    if (ev) { pfd[n].fd = i; pfd[n].events = ev; pfd[n].revents = 0; map[n] = i; n++; }
  }
  timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
  if (nw) timeout = 0;   /* 書けるものがあるので待たない */
  if (want_kbd) {
    /* キーが溜まるかソケットが動くか時間切れまで、刻みで回す */
    t0 = _dos_time_pr();
    for (;;) {
      int slice = timeout;
      kbuf_fill();
      if (kbuf_len > 0) slice = 0;
      else if (timeout < 0 || timeout > KEY_SLICE_MS) slice = KEY_SLICE_MS;
      if (n > 0) {
        r = poll(pfd, n, slice);
        if (r != 0) break;                     /* ソケットが動いた / エラー */
      } else if (slice > 0) {
        _dos_change_pr();                      /* ソケット無し: 譲るだけ */
      }
      kbuf_fill();
      if (kbuf_len > 0) break;
      if (timeout >= 0 && _dos_time_pr() - t0 >= timeout) break;
    }
  } else if (n > 0) {
    r = poll(pfd, n, timeout);
  }
  if (rfds) FD_ZERO(rfds);
  if (wfds) FD_ZERO(wfds);
  if (efds) FD_ZERO(efds);
  if (r < 0) return r;
  if (nw) { for (i = 0; i < nfds; i++) if (FD_ISSET(i, &wready)) FD_SET(i, wfds); count = nw; }
  if (want_kbd && kbuf_len > 0 && rfds) { FD_SET(0, rfds); count++; }
  if (r == 0) return count;
  for (i = 0; i < n; i++) {
    if (rfds && (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(map[i], rfds); count++; }
    if (wfds && (pfd[i].revents & POLLOUT)) { FD_SET(map[i], wfds); count++; }
  }
  return count;
}

/* --- Human68k に無い POSIX 関数のスタブ --- */
int pipe(int fds[2])        { (void)fds; return -1; }
int fsync(int fd)           { (void)fd; return 0; }
int setsid(void)            { return 0; }
int execv(const char *p, char *const a[]) { (void)p; (void)a; return -1; }

/* basename / dirname (libgen)。newlib にシンボルが無いので最小実装。 */
#undef basename
#undef dirname
char *basename(char *p)
{
  char *b;
  if (!p || !*p) return (char *)".";
  b = p;
  { char *q; for (q = p; *q; q++) if (*q == '/' || *q == '\\' || *q == ':') b = q + 1; }
  return b;
}
char *dirname(char *p)
{
  char *last = 0, *q;
  if (!p || !*p) return (char *)".";
  for (q = p; *q; q++) if (*q == '/' || *q == '\\') last = q;
  if (!last) return (char *)".";
  *last = '\0';
  return p;
}

/* --- getaddrinfo を数値 IP 専用で上書き (xip resolver のハング回避) --- */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
  struct addrinfo *ai;
  struct sockaddr_in *sa;
  unsigned long ip;
  if (!node) return EAI_NONAME;
  ip = inet_addr(node);
  if (ip == 0xffffffffUL && strcmp(node, "255.255.255.255") != 0)
    return EAI_NONAME;
  ai = (struct addrinfo *)calloc(1, sizeof *ai);
  sa = (struct sockaddr_in *)calloc(1, sizeof *sa);
  if (!ai || !sa) { free(ai); free(sa); return EAI_MEMORY; }
  sa->sin_family = AF_INET;
  sa->sin_port = htons((unsigned short)(service ? atoi(service) : 0));
  sa->sin_addr.s_addr = ip;
  ai->ai_family = AF_INET;
  ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
  ai->ai_protocol = hints ? hints->ai_protocol : 0;
  ai->ai_addrlen = sizeof *sa;
  ai->ai_addr = (struct sockaddr *)sa;
  *res = ai;
  return 0;
}
void freeaddrinfo(struct addrinfo *res)
{
  while (res) { struct addrinfo *n = res->ai_next; free(res->ai_addr); free(res); res = n; }
}

/* --- 同期ブロッキング connect (xip 用) --- */
#include <sys/socket.h>
int x68k_connect(const char *host, const char *port)
{
  struct sockaddr_in sin;
  int s, r;
  x68dbg("connect-start");
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons((unsigned short)atoi(port ? port : "22"));
  sin.sin_addr.s_addr = inet_addr(host);
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);   /* ブロッキング */
  if (r < 0) { close(s); return -1; }
  x68dbg("connected");
  return s;
}

/* --- エントロピー (弱い) --- */
/* MFP (MC68901) タイマのデータレジスタ C=$E88025 / D=$E88027 を IOCS _B_BPEEK で読む */
#define MFP_TCDR ((const void *)0xE88025)
#define MFP_TDDR ((const void *)0xE88027)
void x68k_get_entropy(unsigned char *buf, int len)
{
  int i;
  volatile int onstack;
  void *heap = malloc(1);
  unsigned long acc;
  acc = (unsigned long)&onstack ^ ((unsigned long)heap << 3);
  acc ^= ((unsigned long)_dos_getdate() << 16) ^ (unsigned long)_dos_gettim2();
  free(heap);
  for (i = 0; i < len; i++) {
    struct iocs_time ot = _iocs_ontime();
    unsigned b = (unsigned char)_iocs_b_bpeek(MFP_TCDR);
    b ^= (unsigned char)_iocs_b_bpeek(MFP_TDDR);
    b ^= (unsigned)ot.sec ^ (unsigned)(ot.sec >> 7);
    acc ^= b + 0x9e3779b9u + (acc << 6) + (acc >> 2);
    buf[i] = (unsigned char)(acc ^ (acc >> 11) ^ (acc >> 19));
    if ((i & 7) == 0)
      _dos_change_pr();   /* 間に xip スレッドを挟んでタイマ位相をずらす */
  }
}
