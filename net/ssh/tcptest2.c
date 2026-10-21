/*
 * tcptest2.x -- libsocket のソケット可読検出まわりを詳しく調べる
 *
 * tcptest.x で poll(POLLIN) がバナーを検出できなかった (サーバはバナー送信済み)。
 * ここでは getsockopt の SO_SOCKSTATE / SO_SOCKLENRECV で状態とバッファ量を見ながら
 * _dos_change_pr() で譲り、何秒で可読になるか、poll とどう食い違うかを調べる。
 * 最後に素のブロッキング read() がバナーを返すかも試す。
 *
 * usage: tcptest2.x [host [port]]
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
#include <x68k/iocs.h>
#include <x68k/dos.h>

/* inetd.x 専用 getsockopt コード (sys/socket.h より) */
#ifndef SO_SOCKLENRECV
#define SO_SOCKLENRECV 102
#endif
#ifndef SO_SOCKSTATE
#define SO_SOCKSTATE 107
#endif

static long centisec(void) { return _iocs_ontime().sec; }
static long elapsed(long t0) { long d = centisec() - t0; if (d < 0) d += 24L*60*60*100; return d; }

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "192.168.1.108";
  int port = argc > 2 ? atoi(argv[2]) : 22;
  struct sockaddr_in sin;
  struct pollfd pfd;
  char buf[512];
  int s, n, r;
  long t0, lastlen = -999;
  socklen_t ol;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tcptest2: connect %s:%d\n", host, port);

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { printf("FAIL socket errno=%d\n", errno); return 1; }
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr(host);
  t0 = centisec();
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);
  printf("connect -> %d errno=%d %ld.%02lds\n", r, errno, elapsed(t0)/100, elapsed(t0)%100);
  if (r < 0) { close(s); return 1; }

  /* getsockopt で状態と受信バッファ量を監視しながら最大 15 秒譲る */
  t0 = centisec();
  for (;;) {
    char *st = NULL;
    long len = -1;
    ol = sizeof st;
    if (getsockopt(s, SOL_SOCKET, SO_SOCKSTATE, &st, &ol) == 0 && st)
      ; /* st は状態文字列 (char*) */
    ol = sizeof len;
    getsockopt(s, SOL_SOCKET, SO_SOCKLENRECV, &len, &ol);
    pfd.fd = s; pfd.events = POLLIN; pfd.revents = 0;
    r = poll(&pfd, 1, 0);   /* タイムアウト 0: 即時 */
    if (len != lastlen) {
      unsigned long a = (unsigned long)st;
      printf("  t=%ld.%02lds state=[%s] recvlen=%ld poll=%d revents=0x%x\n",
             elapsed(t0)/100, elapsed(t0)%100,
             (a >= 0x1000 && a < 0xc00000) ? st : "(na)", len, r, pfd.revents);
      lastlen = len;
    }
    if (len > 0 || (r > 0 && (pfd.revents & POLLIN)))
      break;
    if (elapsed(t0) > 1500) { printf("  giveup after 15s\n"); break; }
    _dos_change_pr();
  }

  /* 素のブロッキング read でバナーが取れるか */
  errno = 0;
  n = read(s, buf, sizeof buf - 1);
  printf("read -> %d errno=%d\n", n, errno);
  if (n > 0) {
    int i;
    for (i = 0; i < n; i++) if (buf[i]=='\r'||buf[i]=='\n'){buf[i]='\0';break;}
    buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = '\0';
    printf("banner: [%s]\n", buf);
    write(s, "SSH-2.0-x68ktest\r\n", 18);
  }
  close(s);
  printf("tcptest2: done\n");
  return 0;
}
