/*
 * tcptest5.x -- connect 直後に sockmode を設定してから受信してみる
 *
 * rcmd は accept 後すぐ sockmode(SOCK_ASCII/BINARY) を呼んでいる。能動オープンの
 * ソケットは受信バッファ (usock.ibuf) が初期化されておらず、データ到着で RST される
 * のかもしれない。connect 直後に SO_SOCKMODE を設定して RST が消えるか試す。
 *
 * usage: tcptest5.x [host [port]]
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

#ifndef SO_SOCKMODE
#define SO_SOCKMODE 101
#endif
#ifndef SO_SOCKLENRECV
#define SO_SOCKLENRECV 102
#endif
#ifndef SOCK_BINARY
#define SOCK_BINARY 0
#endif

extern char *__socket_sockstate(int);
static const char *ss(int s){char*p=__socket_sockstate(s);unsigned long a=(unsigned long)p;return(a<0x1000||a>=0xc00000)?"?":p;}
static long cs(void){return _iocs_ontime().sec;}
static long el(long t0){long d=cs()-t0;if(d<0)d+=24L*60*60*100;return d;}
static int rlen(int s){int v=-1;socklen_t o=sizeof v;getsockopt(s,SOL_SOCKET,SO_SOCKLENRECV,&v,&o);return v;}

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "192.168.1.108";
  int port = argc > 2 ? atoi(argv[2]) : 22;
  struct sockaddr_in sin; char buf[512]; int s, n, r, len; long t0;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tcptest5: connect %s:%d, set SOCKMODE then recv\n", host, port);
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { printf("FAIL socket errno=%d\n", errno); return 1; }
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET; sin.sin_port = htons(port); sin.sin_addr.s_addr = inet_addr(host);
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);
  printf("connect -> %d errno=%d state=[%s]\n", r, errno, ss(s));
  if (r < 0 && errno != EINPROGRESS) { close(s); return 1; }

  /* ★ sockmode を BINARY に設定 (受信側を arm する狙い) */
  { int mode = SOCK_BINARY; r = setsockopt(s, SOL_SOCKET, SO_SOCKMODE, &mode, sizeof mode);
    printf("setsockopt SO_SOCKMODE=BINARY -> %d errno=%d state=[%s]\n", r, errno, ss(s)); }

  t0 = cs();
  for (;;) {
    len = rlen(s);
    if (len > 0) break;
    if (len < 0) { printf("recvlen err state=[%s] after %ld.%02lds\n", ss(s), el(t0)/100, el(t0)%100); close(s); return 1; }
    if (el(t0) > 1500) { printf("GIVEUP recvlen 0 state=[%s]\n", ss(s)); close(s); return 1; }
    _dos_change_pr();
  }
  printf("recvlen=%d state=[%s]\n", len, ss(s));
  n = read(s, buf, len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1);
  printf("read -> %d\n", n);
  if (n > 0) { int i; buf[n]='\0'; for(i=0;i<n;i++) if(buf[i]=='\r'||buf[i]=='\n'){buf[i]='\0';break;} printf("BANNER OK: [%s]\n", buf); }
  close(s);
  printf("tcptest5: done\n");
  return 0;
}
