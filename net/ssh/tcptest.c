/*
 * tcptest.x -- elf2x68k libsocket.a が xip.x 相手に素で動くかの煙試験
 *
 * dropbear 移植の前提確認。以下を実機 (X68000 Z) で一つずつ試す:
 *   1. socket()/connect() で SSH サーバ (既定 192.168.1.108:22) につなぐ
 *   2. poll() で読めるまで待ち (CPU を譲る)、SSH バナーを read()、
 *      偽のクライアントバナーを write()、所要時間を _iocs_ontime で測る
 *   3. fcntl(O_NONBLOCK) 後、何も来ていない read() が -1 / EAGAIN を返すか
 *   4. poll() に {fd0(キーボード), socket} をまとめて渡したとき、
 *      キー入力が POLLIN として見えるか (stdin の渡し方を決めるため)
 *
 * usage: tcptest.x [host [port]]
 * build: Makefile 参照 (m68k-xelf-gcc ... -lsocket)
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

/* _iocs_ontime().sec は当日の 1/100 秒カウント。日付をまたいでも負にならないよう補正する */
static long centisec(void)
{
  return _iocs_ontime().sec;
}
static long elapsed(long t0)
{
  long d = centisec() - t0;
  if (d < 0)
    d += 24L * 60 * 60 * 100;
  return d;
}

int main(int argc, char **argv)
{
  const char *host = argc > 1 ? argv[1] : "192.168.1.108";
  int port = argc > 2 ? atoi(argv[2]) : 22;
  struct sockaddr_in sin;
  struct pollfd pfd[2];
  char buf[512];
  int s, n, r, flags;
  long t0;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tcptest: connect %s:%d\n", host, port);

  /* --- 1. socket/connect --- */
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    printf("FAIL socket: %d (errno=%d) -- xip.x/inetd.x 常駐してる?\n", s, errno);
    return 1;
  }
  printf("ok socket fd=%d\n", s);

  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr(host);

  t0 = centisec();
  r = connect(s, (struct sockaddr *)&sin, sizeof sin);
  printf("connect -> %d (errno=%d) %ld.%02lds\n", r, errno, elapsed(t0) / 100, elapsed(t0) % 100);
  if (r < 0) {
    printf("FAIL connect\n");
    close(s);
    return 1;
  }

  /* --- 2. poll で読めるまで待ってバナー受信 --- */
  t0 = centisec();
  pfd[0].fd = s;
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;
  r = poll(pfd, 1, 10000);   /* 最大 10 秒 */
  printf("poll(sock,10s) -> %d revents=0x%x %ld.%02lds\n", r, pfd[0].revents,
         elapsed(t0) / 100, elapsed(t0) % 100);
  if (r <= 0) {
    printf("FAIL poll (no banner)\n");
    close(s);
    return 1;
  }
  n = read(s, buf, sizeof buf - 1);
  if (n <= 0) {
    printf("FAIL read banner n=%d errno=%d\n", n, errno);
    close(s);
    return 1;
  }
  buf[n] = '\0';
  /* バナー行だけ表示 (CR/LF を削る) */
  {
    int i;
    for (i = 0; i < n; i++)
      if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = '\0'; break; }
  }
  printf("ok banner (%d bytes): [%s]\n", n, buf);

  n = write(s, "SSH-2.0-x68ktest\r\n", 18);
  printf("write client banner -> %d (errno=%d)\n", n, errno);

  /* --- 3. 非ブロッキング read で EAGAIN が返るか --- */
  flags = fcntl(s, F_GETFL, 0);
  r = fcntl(s, F_SETFL, flags | O_NONBLOCK);
  printf("fcntl O_NONBLOCK -> %d (flags was 0x%x)\n", r, flags);
  errno = 0;
  n = read(s, buf, sizeof buf);
  printf("nonblock read (nothing pending) -> %d errno=%d %s\n", n, errno,
         (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? "[EAGAIN OK]"
         : (n == 0 ? "[EOF]" : "[got data or other]"));

  /* --- 4. poll に キーボード(fd0) と socket をまとめて --- */
  printf("poll {stdin, sock} 3s: キーを押すと stdin が見えるか試す...\n");
  pfd[0].fd = 0;
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;
  pfd[1].fd = s;
  pfd[1].events = POLLIN;
  pfd[1].revents = 0;
  r = poll(pfd, 2, 3000);
  printf("poll2 -> %d stdin.revents=0x%x sock.revents=0x%x\n", r, pfd[0].revents, pfd[1].revents);

  close(s);
  printf("tcptest: done\n");
  return 0;
}
