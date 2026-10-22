/*
 * tcptest4.x -- 接続後すぐにクライアントバナーを送ってから受信する (write-first)
 *
 * tcptest.x では、接続→受信待ちの間にサーバのバナーが届いた瞬間に xip が RST を返した
 * (Pi 側 tcpdump で確認)。finger.c は接続後すぐ write_s してから読む。
 * SSH クライアントも識別文字列を先に送ってよいので、ここでは connect 直後に
 * "SSH-2.0-..." を送ってからサーバのバナーを受信できるか試す。
 * 受信検出は socklen のみ。データがある時だけ read するのでブロッキング無限ループは起きない。
 *
 * usage: tcptest4.x [host [port]]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <x68k/iocs.h>
#include <x68k/dos.h>

#ifndef SO_SOCKLENRECV
#define SO_SOCKLENRECV 102
#endif

extern char *__socket_sockstate(int);
static const char *safe_state(int s){char*st=__socket_sockstate(s);unsigned long a=(unsigned long)st;return (a<0x1000||a>=0xc00000)?"?":st;}
static long centisec(void){return _iocs_ontime().sec;}
static long elapsed(long t0){long d=centisec()-t0;if(d<0)d+=24L*60*60*100;return d;}

static int sock_recvlen(int s){int len=-1;socklen_t ol=sizeof len;getsockopt(s,SOL_SOCKET,SO_SOCKLENRECV,&len,&ol);return len;}

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "192.168.1.108";
  int port = argc > 2 ? atoi(argv[2]) : 22;
  struct sockaddr_in sin;
  char buf[512];
  int s, n, r, len;
  long t0;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tcptest4: connect %s:%d (write-first)\n", host, port);

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { printf("FAIL socket errno=%d\n", errno); return 1; }
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr(host);
  t0 = centisec();
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);
  printf("connect -> %d errno=%d state=[%s] %ld.%02lds\n", r, errno, safe_state(s), elapsed(t0)/100, elapsed(t0)%100);
  if (r < 0 && errno != EINPROGRESS) { close(s); return 1; }

  /* ESTABLISHED になるまで少し待つ (socklen 送信側が 0 になるのを目安にしてもよいが状態で) */
  t0 = centisec();
  while (safe_state(s)[0] != 'E' && elapsed(t0) < 500) _dos_change_pr();
  printf("after wait state=[%s] %ld.%02lds\n", safe_state(s), elapsed(t0)/100, elapsed(t0)%100);

  /* ★ 先にクライアントバナーを送る */
  n = write(s, "SSH-2.0-x68ktest\r\n", 18);
  printf("wrote client banner -> %d errno=%d state=[%s]\n", n, errno, safe_state(s));

  /* サーバのバナー受信を待つ (socklen のみ、最大 15 秒) */
  t0 = centisec();
  for (;;) {
    len = sock_recvlen(s);
    if (len > 0) break;
    if (len < 0) { printf("recvlen error=%d state=[%s]\n", len, safe_state(s)); close(s); return 1; }
    if (elapsed(t0) > 1500) { printf("GIVEUP recvlen 0 for 15s state=[%s]\n", safe_state(s)); close(s); return 1; }
    _dos_change_pr();
  }
  printf("recvlen=%d after %ld.%02lds\n", len, elapsed(t0)/100, elapsed(t0)%100);
  n = read(s, buf, len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1);
  printf("read -> %d errno=%d\n", n, errno);
  if (n > 0) {
    int i; buf[n]='\0';
    for (i=0;i<n;i++) if(buf[i]=='\r'||buf[i]=='\n'){buf[i]='\0';break;}
    printf("BANNER OK: [%s]\n", buf);
  }
  close(s);
  printf("tcptest4: done\n");
  return 0;
}
