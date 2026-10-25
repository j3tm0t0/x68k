/*
 * zss_mock.c -- picosim の 0xA7 (端末のセッション、net/npd/zss.h) の Pico 側のまね
 *
 * 本物の Pico では zsshd (SSH サーバー) が認証まで済ませてから端末のバイト列を 0xA7 で Z に渡す (x68k-zos docs/pico-sshd-study.md §6.4)。
 * ここでは SSH を話さず、平文の TCP の口 (ZSS_PORT、既定 2322、127.0.0.1) を ssh の代わりにする:
 *   接続 = OPEN (80x24、user "test"、TERM "xterm"、UTF-8)、受けたバイト = INPUT、OUTPUT は接続に書く、切れたら CLOSED、
 *   EXIT で接続を閉じ (閉じる前に "\r\n[exit N]\r\n" を書く)、続けて CLOSED "exit" を送る (本物の Pico と同じ)。REJECT は理由を書いて閉じる。
 * 試験の台本からの命令は、流れの中の 0xFF で始まる行: "\xffW 100 30\n" (WINCH)、"\xffS INT\n" (SIGNAL)、"\xffE\n" (EOF)。
 * CREDIT (Z が送ってよい量、絶対値) は ZSS_CREDIT (既定 4096、本物の Pico の上限と同じ)。開いたときと、Z の OUTPUT を接続に
 * 書いたあとにだけ送る (変わったときだけ)。確認応答は TSP と同じ (同じ zseq には同じ応答)。ログは picosim_log の "zss:"。
 * x68k-zos の kernel/tests/sshd_test.sh が使う。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "picosim.h"
#include "../../net/npd/zss.h"

#define INBUF 4096
struct ses {
  int fd;                          /* -1 = 空き */
  int gen;
  int opened, accepted, closed_sent, want_open, want_closed;
  uint32_t win;                    /* Z が受けられる残り (WINDOW の絶対値から送った分を引いたもの) */
  uint8_t in[INBUF]; int inn;      /* 接続から読んだ、まだ Z に送っていないもの */
  char cmd[64]; int cmdn, incmd;   /* 0xFF の命令の途中 */
  int winch, cols, rows, eof; char sig[16];
  int credit_dirty;                /* CREDIT を送る (開いたとき、OUTPUT を書いたあと) */
  uint64_t credit_at;              /* 最後に CREDIT を送った時刻 (us) */
  char why[16];                    /* CLOSED の理由 */
};
static struct ses S[ZSS_NSESS];
static int lfd = -1;
static uint32_t pepoch, zepoch_seen;
static unsigned last_zseq = 0xffffffff;
static uint8_t last_res[ZSS_RES_MAX];
static int last_len, last_more;
static unsigned credit = 4096;
static unsigned hold_ms;           /* ZSS_CREDIT_HOLD: CREDIT を出し直すまで待つ (試験用。Z の出力を溜めて大きい要求を作らせる) */
static unsigned long st_in, st_out, st_open, st_resend;
static int st_maxreq;

static void ses_close(struct ses *s)
{
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
}

static void zss_init(void)
{
  const char *p = getenv("ZSS_PORT"), *c = getenv("ZSS_CREDIT");
  struct sockaddr_in a = { 0 };
  int on = 1;
  for (int i = 0; i < ZSS_NSESS; i++) S[i].fd = -1;
  if (c) credit = (unsigned)atoi(c);
  if (getenv("ZSS_CREDIT_HOLD")) hold_ms = (unsigned)atoi(getenv("ZSS_CREDIT_HOLD"));
  pepoch = (uint32_t)time(NULL) ^ 0xa7a7a7a7u;
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
  a.sin_family = AF_INET;
  a.sin_port = htons(p ? atoi(p) : 2322);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 4) < 0) {
    picosim_log("zss: listen %d: %s\n", ntohs(a.sin_port), strerror(errno));
    close(lfd); lfd = -1; return;
  }
  fcntl(lfd, F_SETFL, O_NONBLOCK);
  picosim_log("zss: 0xA7 mock on 127.0.0.1:%d (credit %u)\n", ntohs(a.sin_port), credit);
}

/* 接続から読む (0xFF の命令は取り除いて覚える) */
static void ses_read(struct ses *s)
{
  uint8_t b[512];
  ssize_t n;
  if (s->fd < 0 || s->inn > INBUF - (int)sizeof b) return;
  n = recv(s->fd, b, sizeof b, MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
    ses_close(s);
    if (s->opened) { s->want_closed = 1; strcpy(s->why, "disconnected"); }
    return;
  }
  for (ssize_t i = 0; i < n; i++) {
    if (s->incmd) {
      if (b[i] == '\n' || s->cmdn >= (int)sizeof s->cmd - 1) {
        s->cmd[s->cmdn] = 0; s->incmd = 0;
        if (s->cmd[0] == 'W') { sscanf(s->cmd + 1, "%d %d", &s->cols, &s->rows); s->winch = 1; }
        else if (s->cmd[0] == 'S') snprintf(s->sig, sizeof s->sig, "%s", s->cmd + 2);
        else if (s->cmd[0] == 'E') s->eof = 1;
      } else s->cmd[s->cmdn++] = b[i];
    } else if (b[i] == 0xff) { s->incmd = 1; s->cmdn = 0; }
    else s->in[s->inn++] = b[i];
  }
}

static void zss_poll(void)
{
  if (lfd < 0) return;
  for (;;) {
    int fd = accept(lfd, NULL, NULL), i;
    if (fd < 0) break;
    for (i = 0; i < ZSS_NSESS && S[i].fd >= 0; i++) ;
    if (i == ZSS_NSESS) { const char m[] = "zss mock: too many sessions\r\n"; (void)!write(fd, m, sizeof m - 1); close(fd); continue; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    { int g = S[i].gen + 1; memset(&S[i], 0, sizeof S[i]); S[i].gen = g & 0xff; }
    S[i].fd = fd; S[i].want_open = 1; S[i].cols = 80; S[i].rows = 24; S[i].credit_dirty = 1;
    st_open++;
    picosim_log("zss: connection -> sess %d gen %d\n", i, S[i].gen);
  }
  for (int i = 0; i < ZSS_NSESS; i++) ses_read(&S[i]);
}

static uint8_t *put_msg(uint8_t *w, uint8_t *end, int type, int sess, int gen, const void *b, unsigned len)
{
  if (w + zss_msize(len) > end) return NULL;
  uint8_t *p = zss_msg(w, type, sess, gen, len);
  if (len) memcpy(p, b, len);
  if (len & 1) p[len] = 0;
  return w + zss_msize(len);
}

static void on_msg(int type, int i, int gen, const uint8_t *b, unsigned len)
{
  struct ses *s;
  if (i < 0 || i >= ZSS_NSESS) return;
  s = &S[i];
  if (gen != s->gen) return;
  switch (type) {
  case ZSS_C_ACCEPT: s->accepted = 1; if (len >= 4) s->win = zss_get32(b); break;
  case ZSS_C_WINDOW: if (len >= 4) s->win = zss_get32(b); break;
  case ZSS_C_OUTPUT: if (s->fd >= 0) (void)!write(s->fd, b, len); st_out += len; s->credit_dirty = 1; break;
  case ZSS_C_REJECT:
    if (s->fd >= 0) { (void)!write(s->fd, "zss mock: rejected: ", 20); (void)!write(s->fd, b + 1, len > 1 ? b[0] : 0); (void)!write(s->fd, "\r\n", 2); }
    ses_close(s); s->opened = 0;
    break;
  case ZSS_C_EXIT:
    if (s->fd >= 0 && len >= 4) { char m[32]; int n = snprintf(m, sizeof m, "\r\n[exit %u]\r\n", (unsigned)zss_get32(b)); (void)!write(s->fd, m, n); }
    picosim_log("zss: sess %d exit\n", i);
    ses_close(s); s->want_closed = 1; strcpy(s->why, "exit");   /* 本物の Pico と同じく、EXIT の後にも CLOSED */
    break;
  }
}

static int zss_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  unsigned zseq;
  uint32_t ze;
  uint8_t *w, *end;
  if (reqlen < ZSS_HDR || req[1] != ZSS_VERSION) return 0;
  zseq = zss_get16(req + 4);
  if (reqlen > st_maxreq) { st_maxreq = reqlen; if (reqlen >= 1024) picosim_log("zss: largest 0xA7 request %d bytes\n", reqlen); }
  ze = zss_get32(req + 8);
  if (zseq == last_zseq && ze == zepoch_seen && last_len <= resmax) {   /* 送り直し: 同じ応答 */
    memcpy(res, last_res, last_len);
    *more = last_more;
    st_resend++;
    return last_len;
  }
  if (ze != zepoch_seen) {         /* zos.x が始め直した: セッションを全部切る */
    if (zepoch_seen)
      for (int i = 0; i < ZSS_NSESS; i++) if (S[i].opened) {
        if (S[i].fd >= 0) (void)!write(S[i].fd, "\r\nZ-OS restarted\r\n", 18);
        ses_close(&S[i]); S[i].opened = 0;
      }
    zepoch_seen = ze;
  }
  for (const uint8_t *m = req + ZSS_HDR, *e = req + reqlen; m + ZSS_MHDR <= e; ) {
    unsigned l = zss_get16(m + 4);
    if (m + ZSS_MHDR + l > e) break;
    on_msg(m[0], m[1], m[2], m + ZSS_MHDR, l);
    m += zss_msize(l);
  }
  /* 応答 */
  if (resmax > ZSS_RES_MAX) resmax = ZSS_RES_MAX;
  w = res + ZSS_HDR; end = res + resmax;
  *more = 0;
  for (int i = 0; i < ZSS_NSESS; i++) {
    struct ses *s = &S[i];
    uint8_t b[64], *nw;
    if (s->want_open) {
      int k = 0;
      zss_put16(b, s->cols); zss_put16(b + 2, s->rows); zss_put32(b + 4, 0x7f000001); zss_put16(b + 8, 0);
      k = 10; b[k++] = 4; memcpy(b + k, "test", 4); k += 4; b[k++] = 5; memcpy(b + k, "xterm", 5); k += 5; b[k++] = ZSS_OF_UTF8;
      if (!(nw = put_msg(w, end, ZSS_E_OPEN, i, s->gen, b, k))) { *more = 1; continue; }
      w = nw; s->want_open = 0; s->opened = 1;
    }
    if (!s->opened) continue;
    if (s->want_closed && !s->closed_sent) {
      int k = strlen(s->why);
      b[0] = k; memcpy(b + 1, s->why, k);
      if ((nw = put_msg(w, end, ZSS_E_CLOSED, i, s->gen, b, 1 + k))) { w = nw; s->closed_sent = 1; s->opened = 0; }
      continue;
    }
    if (s->credit_dirty && picosim_now_us() - s->credit_at >= (uint64_t)hold_ms * 1000) {
      zss_put32(b, credit);
      if ((nw = put_msg(w, end, ZSS_E_CREDIT, i, s->gen, b, 4))) { w = nw; s->credit_dirty = 0; s->credit_at = picosim_now_us(); }
    }
    if (s->accepted && s->inn && s->win) {
      unsigned n = s->inn, room = end - w - ZSS_MHDR - 1;
      if (n > s->win) n = s->win;
      if (n > room) n = room;
      if (n && (nw = put_msg(w, end, ZSS_E_INPUT, i, s->gen, s->in, n))) {
        w = nw; s->win -= n; st_in += n;
        memmove(s->in, s->in + n, s->inn - n); s->inn -= n;
      }
      if (s->inn) *more = 1;
    }
    if (s->accepted && s->winch) {
      zss_put16(b, s->cols); zss_put16(b + 2, s->rows);
      if ((nw = put_msg(w, end, ZSS_E_WINCH, i, s->gen, b, 4))) { w = nw; s->winch = 0; }
    }
    if (s->accepted && s->sig[0] && !s->inn) {
      if ((nw = put_msg(w, end, ZSS_E_SIGNAL, i, s->gen, s->sig, strlen(s->sig)))) { w = nw; s->sig[0] = 0; }
    }
    if (s->accepted && s->eof && !s->inn) {
      if ((nw = put_msg(w, end, ZSS_E_EOF, i, s->gen, NULL, 0))) { w = nw; s->eof = 0; }
    }
  }
  res[0] = ZSS_MARK; res[1] = ZSS_VERSION; res[2] = *more ? ZSS_PF_MORE : 0; res[3] = 0;
  zss_put16(res + 4, zseq); zss_put16(res + 6, 0); zss_put32(res + 8, pepoch);
  last_zseq = zseq;
  last_len = w - res;
  last_more = *more;
  memcpy(last_res, res, last_len);
  return last_len;
}

static const struct picosim_svc zss_svc = { "zss", ZSS_MARK, zss_init, zss_exchange, zss_poll };
__attribute__((constructor)) static void zss_reg(void) { picosim_register_svc(&zss_svc); }
