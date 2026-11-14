/*
 * zl.c -- picosim -Z PORT: BlueSCSI の「zmux の線を SCSI ID6 に通す」まね (net/zmux/x68zl_proto.h、DESIGN.md §9)
 *
 * ID6 の READ 0x230000 / WRITE 0x220000 のバイトの列を、PORT で待ち受けた TCP (zmuxd --tcp 127.0.0.1:PORT) に渡す。
 * つながるのは 1 本で、新しい接続が来たら古いものを切る。送りの輪はファームと同じ 1KB。
 * 環境変数 ZL_DROP=N: N 回に 1 回、X68000 に渡す READ のデータを捨てる (SCSI で落ちたことにする。zmux の送り直しの試験)
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "picosim.h"
#include "../../net/zmux/x68zl_proto.h"

#define TXRING 1024
#define RXBUF  65536

extern int zl_port;                     /* picosim.c (-Z) */
extern uint32_t picosim_xfer_n;         /* picosim.c: 今の READ / WRITE のセクタ数 */
static int lfd = -1, cfd = -1;
static uint8_t tx[TXRING];
static int txn;                          /* tx に溜まっていて TCP にまだ出していないバイト */
static uint8_t rx[RXBUF];
static int rxn;
static uint8_t ans[X68ZL_SECTORS * 512], wbuf[X68ZL_SECTORS * 512];
static int drop_every, reads;
static unsigned long st_in, st_out, st_dropped;

static void p16(uint8_t *p, int v) { p[0] = v >> 8; p[1] = v; }
static int g16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static void zl_close(void)
{
  if (cfd >= 0) {
    close(cfd);
    cfd = -1;
    picosim_log("zl: zmuxd disconnected (in %lu out %lu)\n", st_in, st_out);
  }
  txn = rxn = 0;
}

void zl_start(void)
{
  struct sockaddr_in a;
  int one = 1;
  if (!zl_port)
    return;
  if (getenv("ZL_DROP"))
    drop_every = atoi(getenv("ZL_DROP"));
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(zl_port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 1) < 0) {
    perror("picosim -Z");
    exit(1);
  }
  fcntl(lfd, F_SETFL, O_NONBLOCK);
  picosim_log("zl: zmux line on SCSI ID6, TCP 127.0.0.1:%d\n", zl_port);
}

/* 接続を受け、TCP を読み、溜まった送りを出す (READ / WRITE のたびと picosim の回るたびに呼ぶ) */
void zl_poll(void)
{
  int fd, n, one = 1;
  if (lfd < 0)
    return;
  if ((fd = accept(lfd, NULL, NULL)) >= 0) {
    zl_close();
    cfd = fd;
    fcntl(cfd, F_SETFL, O_NONBLOCK);
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    picosim_log("zl: zmuxd connected\n");
  }
  if (cfd < 0)
    return;
  while (rxn < RXBUF) {
    n = recv(cfd, rx + rxn, RXBUF - rxn, 0);
    if (n > 0) {
      rxn += n;
      st_in += n;
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      zl_close();
      return;
    }
    break;
  }
  if (txn > 0) {
    n = send(cfd, tx, txn, 0);
    if (n > 0) {
      memmove(tx, tx + n, txn - n);
      txn -= n;
      st_out += n;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      zl_close();
  }
}

int zl_range(uint32_t lba)
{
  return lfd >= 0 && ((lba >= X68ZL_WLBA && lba < X68ZL_WLBA + X68ZL_SECTORS) || (lba >= X68ZL_RLBA && lba < X68ZL_RLBA + X68ZL_SECTORS));
}

int zl_read(uint32_t lba, uint8_t *buf)
{
  if (!zl_range(lba))
    return -1;
  if (lba < X68ZL_RLBA) {
    memset(buf, 0, 512);
    return 0;
  }
  if (lba == X68ZL_RLBA) {               /* 最初のセクタで、このコマンドのセクタ数分の応答を作る */
    uint32_t sect = picosim_xfer_n ? picosim_xfer_n : 1;
    int len;
    zl_poll();
    if (sect > X68ZL_SECTORS) sect = X68ZL_SECTORS;
    len = rxn < (int)(sect * 512 - X68ZL_HDR) ? rxn : (int)(sect * 512 - X68ZL_HDR);
    memset(ans, 0, sizeof ans);
    memcpy(ans, "X6ZR", 4);
    p16(ans + 4, len);
    p16(ans + 6, cfd >= 0 ? TXRING - txn : TXRING);
    p16(ans + 8, rxn - len > 65535 ? 65535 : rxn - len);
    ans[10] = X68ZL_ST_ON | (cfd >= 0 ? X68ZL_ST_CONN : 0);
    memcpy(ans + X68ZL_HDR, rx, len);
    memmove(rx, rx + len, rxn - len);
    rxn -= len;
    if (drop_every && len > 0 && ++reads % drop_every == 0) {   /* 落ちたことにする: 中身を捨てた応答を返す */
      p16(ans + 4, 0);
      st_dropped += len;
    }
  }
  memcpy(buf, ans + (lba - X68ZL_RLBA) * 512, 512);
  return 0;
}

int zl_write(uint32_t lba, const uint8_t *buf)
{
  uint32_t k;
  if (!zl_range(lba))
    return -1;
  if (lba >= X68ZL_RLBA)
    return 0;
  k = lba - X68ZL_WLBA;
  memcpy(wbuf + k * 512, buf, 512);
  if (k + 1 == (picosim_xfer_n ? picosim_xfer_n : 1)) {   /* 最後のセクタで中身を渡す */
    int len = g16(wbuf + 4), room;
    if (memcmp(wbuf, "X6ZW", 4) != 0)
      return 0;
    if (len > (int)((k + 1) * 512 - X68ZL_HDR))
      len = (k + 1) * 512 - X68ZL_HDR;
    if (g16(wbuf + 6) & X68ZL_F_START) {
      picosim_log("zl: X68000 started (drops %d bytes not sent)\n", txn);
      txn = 0;
    }
    if (cfd < 0)
      return 0;                         /* 線の先に誰もいない: 捨てる */
    room = TXRING - txn;
    if (len > room) {
      picosim_log("zl: X68000 wrote %d bytes over txfree (%d)\n", len - room, room);
      len = room;
    }
    memcpy(tx + txn, wbuf + X68ZL_HDR, len);
    txn += len;
    zl_poll();
  }
  return 0;
}

void zl_stats(void)
{
  if (lfd >= 0)
    picosim_log("zl: %s, from zmuxd %lu, to zmuxd %lu, dropped on purpose %lu\n", cfd >= 0 ? "connected" : "no zmuxd", st_in, st_out, st_dropped);
}
