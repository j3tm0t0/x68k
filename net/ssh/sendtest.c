/*
 * sendtest.x -- Z から大量に「送る」試験 (xip / libsocket の送信が並びどおりに届くか)。
 *
 * ssh.x -Z のヘルパ転送 (Z から 256 KB を送る) で sshd が "Bad packet length" を出した。それまで Z は
 * 打鍵しか送っていなかったので、送信の大きな流れは初めて。dropbear と同じやり方 (ブロッキング connect の後
 * fcntl O_NONBLOCK、poll で書けるのを待ち、write の戻り値・EAGAIN に従う) で既知の並びを送り、相手 (Pi の
 * sendsink.py) が 1 バイトずつ照合する。
 *
 * usage: sendtest.x host port total chunk [nb|b] [clobber] [slow]
 *   total    送るバイト数
 *   chunk    1 回の write の上限 (dropbear の 1 パケットは最大 ~32 KB)
 *   nb / b   ノンブロッキング (dropbear と同じ) / ブロッキング
 *   clobber  write が返ったら送った部分のバッファを 0xAA で潰す (xip が送信前に利用者のメモリを参照していないか)
 *   slow     write のたびに _dos_change_pr を挟む
 *   capN     1 回の write を N バイトまでにする (chunk はバッファ 1 つ = dropbear の 1 パケットの大きさ。ssh.x の __wrap_write と同じ)
 *   odd      バッファを奇数番地から置く
 *   rx       送りながら相手からの受信も読む (相手が echo するとき)
 * 並び: バイト i = (i * 2654435761) >> 24 の下位 8 ビット ^ (i >> 16)。位置ごとに違うので欠け・重複・入れ替わりが分かる。
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

static long cs(void) { return _iocs_ontime().sec; }
static long el(long t0) { long d = cs() - t0; if (d < 0) d += 24L * 60 * 60 * 100; return d; }
static unsigned char pat(unsigned long i) { return (unsigned char)(((i * 2654435761UL) >> 24) ^ (i >> 16)); }

int main(int argc, char **argv)
{
  const char *host;
  int port, chunk, nb = 1, clobber = 0, slow = 0, cap = 0, odd = 0, rx = 0, s, r, i;
  long nrx = 0;
  long total, sent = 0, nwrite = 0, nshort = 0, neagain = 0, npoll0 = 0, t0;
  struct sockaddr_in sin;
  static unsigned char bufm[65536 + 2];
  unsigned char *buf;
  long bufoff = -1, buflen = 0;   /* buf に入っている並びの範囲 [bufoff, bufoff+buflen) */

  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 5) { printf("usage: sendtest.x host port total chunk [nb|b] [clobber] [slow]\n"); return 2; }
  host = argv[1]; port = atoi(argv[2]); total = atol(argv[3]); chunk = atoi(argv[4]);
  if (chunk < 1 || chunk > 65536) chunk = 65536;
  for (i = 5; i < argc; i++) {
    if (!strcmp(argv[i], "b")) nb = 0;
    else if (!strcmp(argv[i], "nb")) nb = 1;
    else if (!strcmp(argv[i], "clobber")) clobber = 1;
    else if (!strcmp(argv[i], "slow")) slow = 1;
    else if (!strncmp(argv[i], "cap", 3)) cap = atoi(argv[i] + 3);
    else if (!strcmp(argv[i], "odd")) odd = 1;
    else if (!strcmp(argv[i], "rx")) rx = 1;
  }
  buf = bufm + (odd ? 1 : 0);
  printf("sendtest: %s:%d total %ld chunk %d cap %d %s%s%s%s\n", host, port, total, chunk, cap, nb ? "nonblock" : "block",
         clobber ? " clobber" : "", slow ? " slow" : "", odd ? " odd" : "");
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { printf("FAIL socket errno=%d\n", errno); return 1; }
  memset(&sin, 0, sizeof sin);
  sin.sin_family = AF_INET; sin.sin_port = htons(port); sin.sin_addr.s_addr = inet_addr(host);
  if (connect(s, (struct sockaddr *)&sin, sizeof sin) < 0) { printf("FAIL connect errno=%d\n", errno); return 1; }
  if (nb && fcntl(s, F_SETFL, O_NONBLOCK) < 0) printf("WARN fcntl O_NONBLOCK errno=%d\n", errno);
  t0 = cs();
  while (sent < total) {
    long want = total - sent, k;
    if (want > chunk) want = chunk;
    /* dropbear と同じく、送り切るまで同じ内容のバッファを渡し直す */
    if (bufoff < 0 || sent >= bufoff + buflen) {
      for (k = 0; k < want; k++) buf[k] = pat((unsigned long)(sent + k));
      bufoff = sent; buflen = want;
    }
    if (nb) {
      struct pollfd p;
      p.fd = s; p.events = POLLOUT | (rx ? POLLIN : 0); p.revents = 0;
      r = poll(&p, 1, 5000);
      if (r > 0 && (p.revents & POLLIN)) {   /* 相手からの小さな送信 (ssh の WINDOW_ADJUST の代わり) を読む */
        char tmp[512];
        int k = (int)read(s, tmp, sizeof tmp);
        if (k > 0) nrx += k;
        if (!(p.revents & POLLOUT)) continue;
      }
      if (r == 0) { npoll0++; if (npoll0 > 20) { printf("FAIL poll timeout x20 at %ld\n", sent); break; } continue; }
      if (r < 0) { printf("FAIL poll errno=%d at %ld\n", errno, sent); break; }
    }
    {
      long ask = bufoff + buflen - sent;
      if (cap > 0 && ask > cap) ask = cap;
      r = (int)write(s, buf + (sent - bufoff), (size_t)ask);
      if (r > ask) { printf("FAIL write returned %d > asked %ld at %ld\n", r, ask, sent); break; }
      if (r >= 0 && r < ask) nshort++;
    }
    nwrite++;
    if (r < 0) {
      if (errno == EAGAIN || errno == EINTR) { neagain++; if (slow) _dos_change_pr(); continue; }
      printf("FAIL write errno=%d at %ld\n", errno, sent);
      break;
    }
    if (r == 0) { printf("FAIL write returned 0 at %ld\n", sent); break; }
    sent += r;
    if (clobber && sent == bufoff + buflen) memset(buf, 0xAA, (size_t)buflen);   /* パケットを送り切ったら捨てる (dropbear は buf_free して再利用) */
    if (slow) _dos_change_pr();
  }
  {
    long t = el(t0);
    printf("sent %ld bytes in %ld.%02ld s (%ld B/s), writes %ld, short %ld, eagain %ld, rx %ld\n",
           sent, t / 100, t % 100, t > 0 ? sent * 100 / t : 0, nwrite, nshort, neagain, nrx);
  }
  /* 相手が読み終えるのを待ってから閉じる (xip の close が未送信を捨てないか分からないので) */
  {
    struct pollfd p; char c;
    p.fd = s; p.events = POLLIN; p.revents = 0;
    for (;;) {
      r = poll(&p, 1, 30000);
      if (r <= 0) break;
      r = (int)read(s, &c, 1);
      if (r <= 0 || c == '!') break;   /* rx の小さな送信の残りは読み捨てる */
    }
    if (r > 0 || r == 0) { printf("peer closed/replied (%d)\n", r); }
    else printf("no reply from peer within 30 s\n");
  }
  close(s);
  return 0;
}
