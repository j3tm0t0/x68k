/*
 * tcptest3.x -- 非ブロッキングソケットで、接続後に受信バッファが埋まるかを追う
 *
 * ブロッキング read は ESTABLISHED + データ無しで CHANGE_PR せず無限ループになる
 * (libsocket read.c の仕様) ので絶対に使わない。
 * SOCK_NONBLOCK で socket を開き、sockstate と socklen(recv) を監視しながら
 * _dos_change_pr() で譲り、何秒でバナーが受信バッファに乗るかを見る。
 * ハードな時間上限 (15 秒) で必ず抜ける。
 *
 * usage: tcptest3.x [host [port]]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <x68k/iocs.h>
#include <x68k/dos.h>

#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x800
#endif
#ifndef SO_SOCKLENRECV
#define SO_SOCKLENRECV 102
#endif

/* libsocket 内部。_TI_sockstate は無効なソケットで非ポインタ (エラー番号) を返すので、
   RAM の範囲でなければ "?" とみなす (rcmd.c と同じ用心) */
extern char *__socket_sockstate(int sockfd);
static const char *safe_state(int s)
{
  char *st = __socket_sockstate(s);
  unsigned long a = (unsigned long)st;
  if (a < 0x1000 || a >= 0xc00000)
    return "?";
  return st;
}

static long centisec(void) { return _iocs_ontime().sec; }
static long elapsed(long t0) { long d = centisec() - t0; if (d < 0) d += 24L*60*60*100; return d; }

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "192.168.1.108";
  int port = argc > 2 ? atoi(argv[2]) : 22;
  struct sockaddr_in sin;
  char buf[512];
  int s, n, r;
  long t0;
  char laststate[40] = "";
  int lastlen = -1;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tcptest3: connect %s:%d (nonblock)\n", host, port);

  s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (s < 0) { printf("FAIL socket errno=%d\n", errno); return 1; }
  printf("ok socket fd=%d state=[%s]\n", s, safe_state(s));

  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr(host);
  t0 = centisec();
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);
  printf("connect -> %d errno=%d state=[%s] %ld.%02lds\n", r, errno, safe_state(s),
         elapsed(t0)/100, elapsed(t0)%100);

  /* 受信バッファが埋まるまで最大 15 秒。状態か長さが変わるたびに表示 */
  t0 = centisec();
  for (;;) {
    const char *st = safe_state(s);
    int len = -1;
    { socklen_t ol = sizeof len; getsockopt(s, SOL_SOCKET, SO_SOCKLENRECV, &len, &ol); }
    if (strcmp(st, laststate) != 0 || len != lastlen) {
      printf("  t=%ld.%02lds state=[%s] recvlen=%d\n", elapsed(t0)/100, elapsed(t0)%100, st, len);
      strncpy(laststate, st, sizeof laststate - 1);
      lastlen = len;
    }
    if (len > 0)
      break;
    if (elapsed(t0) > 1500) { printf("  GIVEUP: recvlen stayed 0 for 15s\n"); close(s); return 1; }
    _dos_change_pr();
  }

  /* 非ブロッキング recv でバナーを取る */
  errno = 0;
  n = recv(s, buf, sizeof buf - 1, 0);
  printf("recv -> %d errno=%d\n", n, errno);
  if (n > 0) {
    int i;
    buf[n] = '\0';
    for (i = 0; i < n; i++) if (buf[i]=='\r'||buf[i]=='\n'){buf[i]='\0';break;}
    printf("BANNER OK: [%s]\n", buf);
    n = write(s, "SSH-2.0-x68ktest\r\n", 18);
    printf("sent client banner -> %d\n", n);
  }
  close(s);
  printf("tcptest3: done\n");
  return 0;
}
