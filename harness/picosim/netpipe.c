/*
 * netpipe.c -- picosim の netpipe (SCSI ID 6 の LBA 帯で Z とパケットを運ぶ)。x68kzremotedrv の netpipe ファーム
 *              (net/x68kzremotedrv-netpipe の patch 0001-0011 の src/netpipe.c) のブリッジと同じ決まりで答える
 *
 * Z は 1 回の交換で NP_WLBA に要求 (見出し 16 バイト + 送信データ) を書き、NP_RLBA 帯のどこかから応答を読む。
 *   見出し: magic(4) seq(4) len(2) rxmax(2 応答では残り) stat(4 要求では Z の IP、応答では統計)
 *   "X6NR": レコードで運ぶ。Z へ [長さ 2][空き 14][IP][詰め物] / Z から [長さ 2][IP][詰め物]
 *   "X6NP": SLIP のバイト列 (slipd.x -z a.b.c.d のブリッジ)。UDP モード (stat = 0) は扱わない
 * ファームと同じにしてあるところ:
 *   - 応答は「最後の要求に対するもの」1 個だけ。帯のどの LBA を読んでも同じ (lba % 8 のセクタ)
 *   - Z へのリングは 16KB。溢れたパケットは捨てる (stat の下位 16 ビット)
 *   - 中身の先頭が 0xA5 のレコードは暗号アクセラレータ (net/accel の accel_ops.c をそのまま使う)。計算中は BUSY、
 *     同じ要求の出し直しには前の結果。結果は RP2040 の所要時間 (見積り、-a で倍率) が経ってから次の交換で返す
 *   - Z 宛ての ping (echo request) は Z に渡さず代わりに返す (ファームの代返)
 *   - 登録したサービス (picosim_register_svc、0xA6 など) のレコードはその exchange に渡し、応答を先頭側に載せる
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picosim.h"
#include "../../net/accel/accel_proto.h"
#include "../../net/accel/accel_ops.h"

#define MAGIC_P         0x58364e50      /* "X6NP" */
#define MAGIC_R         0x58364e52      /* "X6NR" */
#define REC_HDR         16
#define RXRING          16384
#define MTU             1500
#define SLIP_END        0xc0
#define SLIP_ESC        0xdb
#define SLIP_ESC_END    0xdc
#define SLIP_ESC_ESC    0xdd

double np_accel_scale = 1.0;            /* -a: アクセラレータの所要時間の倍率 (0 = すぐ) */

static uint8_t reqbuf[NP_SECTS * 512], resbuf[NP_SECTS * 512];
static int nsect;
static uint8_t rxring[RXRING];
static uint32_t rx_head, rx_tail;
static uint32_t st_tozf, st_drop, st_xchg, st_tx, st_rx, st_accel, st_svc, st_bad;
static uint32_t zip;                    /* Z の IP (ネットワークバイトオーダーのまま。0 = まだ) */
static int raw;
static uint8_t echo_pkt[MTU];
static int echo_len;

static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void p16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }

/* ---- サービス ---- */
#define SVC_MAX 4
static const struct picosim_svc *svcs[SVC_MAX];
static int nsvc;

void picosim_register_svc(const struct picosim_svc *svc)
{
  if (nsvc < SVC_MAX)
    svcs[nsvc++] = svc;
}

static const struct picosim_svc *svc_for(uint8_t mark)
{
  for (int i = 0; i < nsvc; i++)
    if (svcs[i]->mark == mark)
      return svcs[i];
  return NULL;
}

/* ---- 暗号アクセラレータ (ファームの accel.c と同じ受け渡し、計算は即、渡すのは見積りの時間のあと) ---- */
static uint8_t a_last_req[ACCEL_REQ_MAX], a_res[ACCEL_RES_MAX], a_busy[ACCEL_RES_HDR];
static int a_last_req_len, a_res_len, a_busy_len, a_pending;
static uint64_t a_ready_at;

static uint32_t accel_cost_us(int op)
{
  /* RP2040 125MHz の Monocypher の見積り (net/accel/README.md、Unicorn で数えた値) */
  switch (op) {
  case ACCEL_OP_X25519:
  case ACCEL_OP_X25519_BASE:    return 100000;
  case ACCEL_OP_ED25519_VERIFY: return 170000;
  case ACCEL_OP_ED25519_SIGN:   return 50000;
  default:                      return 100;
  }
}

static void accel_submit(const uint8_t *p, int len)
{
  if (len < ACCEL_REQ_HDR || len > ACCEL_REQ_MAX)
    return;
  st_accel++;
  if (!a_pending && len == a_last_req_len && memcmp(p, a_last_req, len) == 0) {
    /* 出し直し: 前の結果をすぐ返す */
    a_pending = 1;
    a_ready_at = 0;
    return;
  }
  if (a_pending) {
    a_busy_len = accel_res_build(a_busy, p[1], accel_get16(p + 2), ACCEL_ST_BUSY, 0, 0, NULL, 0);
    return;
  }
  uint32_t cost = (uint32_t)(accel_cost_us(p[1]) * np_accel_scale);
  a_res_len = accel_compute(p, len, a_res, NULL);
  accel_put32(a_res + 6, cost);         /* usec: Pico で掛かったはずの時間 */
  memcpy(a_last_req, p, len);
  a_last_req_len = len;
  a_pending = 1;
  a_ready_at = picosim_now_us() + cost;
  picosim_log("accel: op %d id %u status %d (%u us modelled)\n", p[1], accel_get16(p + 2), a_res[4], cost);
}

static int accel_result(uint8_t *dst)
{
  if (a_busy_len) {
    int n = a_busy_len;
    memcpy(dst, a_busy, n);
    a_busy_len = 0;
    return n;
  }
  if (a_pending && picosim_now_us() >= a_ready_at) {
    memcpy(dst, a_res, a_res_len);
    a_pending = 0;
    return a_res_len;
  }
  return 0;
}

/* ---- Z へのリング ---- */
static int ring_room(uint32_t n) { return n <= RXRING - (rx_head - rx_tail); }

static void ring_put_rec(const uint8_t *ip, int len)
{
  uint32_t rl = REC_HDR + ((len + 1) & ~1);
  if (!ring_room(rl)) {
    st_drop++;
    return;
  }
  uint32_t h = rx_head;
  rxring[h++ % RXRING] = len >> 8;
  rxring[h++ % RXRING] = len;
  for (int i = 2; i < REC_HDR; i++)
    rxring[h++ % RXRING] = 0;
  for (int i = 0; i < len; i++)
    rxring[h++ % RXRING] = ip[i];
  if (len & 1)
    rxring[h++ % RXRING] = 0;
  rx_head = h;
  st_tozf++;
}

static void ring_put_slip(const uint8_t *ip, int len)
{
  if (!ring_room(2 * len + 2)) {
    st_drop++;
    return;
  }
  uint32_t h = rx_head;
  rxring[h++ % RXRING] = SLIP_END;
  for (int i = 0; i < len; i++) {
    uint8_t c = ip[i];
    if (c == SLIP_END) {
      rxring[h++ % RXRING] = SLIP_ESC;
      rxring[h++ % RXRING] = SLIP_ESC_END;
    } else if (c == SLIP_ESC) {
      rxring[h++ % RXRING] = SLIP_ESC;
      rxring[h++ % RXRING] = SLIP_ESC_ESC;
    } else {
      rxring[h++ % RXRING] = c;
    }
  }
  rxring[h++ % RXRING] = SLIP_END;
  rx_head = h;
  st_tozf++;
}

static uint16_t cksum(const uint8_t *b, int n)
{
  uint32_t s = 0;
  for (int i = 0; i + 1 < n; i += 2)
    s += (b[i] << 8) | b[i + 1];
  if (n & 1)
    s += b[n - 1] << 8;
  while (s >> 16)
    s = (s & 0xffff) + (s >> 16);
  return ~s & 0xffff;
}

/* LAN から Z 宛ての IPv4 パケット */
void np_to_z(const uint8_t *ip, int len)
{
  if (zip == 0 || len < 20)
    return;
  int hl = (ip[0] & 15) * 4, tot = (ip[2] << 8) | ip[3];
  if (tot < 20 || tot > len) {
    st_drop++;
    return;
  }
  if (ip[9] == 1 && tot >= hl + 8 && ip[hl] == 8) {
    /* ping は Z に渡さず、次の交換で代わりに返す (ファームの代返) */
    if (echo_len == 0 && tot <= MTU) {
      memcpy(echo_pkt, ip, tot);
      echo_len = tot;
    } else {
      st_drop++;
    }
    return;
  }
  st_rx += tot;
  if (raw)
    ring_put_rec(ip, tot);
  else
    ring_put_slip(ip, tot);
}

static void echo_reply(void)
{
  int tot = echo_len, hl = (echo_pkt[0] & 15) * 4;
  uint8_t *ip = echo_pkt, t[4];
  if (tot == 0)
    return;
  if (hl >= 20 && tot >= hl + 8) {
    memcpy(t, ip + 12, 4);
    memcpy(ip + 12, ip + 16, 4);
    memcpy(ip + 16, t, 4);
    ip[8] = 64;
    ip[10] = ip[11] = 0;
    p16(ip + 10, cksum(ip, hl));
    ip[hl] = 0;
    ip[hl + 2] = ip[hl + 3] = 0;
    p16(ip + hl + 2, cksum(ip + hl, tot - hl));
    lan_from_z(ip, tot);
  }
  echo_len = 0;
}

/* ---- Z から ---- */
static uint8_t svc_res[NP_SECTS * 512];
static int svc_res_len;                 /* この交換で載せるサービスの応答 (レコードの形で並べたもの) */
static int svc_more;

static void svc_record(const struct picosim_svc *s, const uint8_t *p, int len)
{
  int room = (int)sizeof svc_res - svc_res_len - REC_HDR, more = 0, n;
  if (room <= 0)
    return;
  st_svc++;
  n = s->exchange(p, len, svc_res + svc_res_len + REC_HDR, room > NP_DATAMAX ? NP_DATAMAX : room, &more);
  svc_more |= more;
  if (n > 0) {
    p16(svc_res + svc_res_len, n);
    memset(svc_res + svc_res_len + 2, 0, REC_HDR - 2);
    if (n & 1)
      svc_res[svc_res_len + REC_HDR + n] = 0;
    svc_res_len += REC_HDR + ((n + 1) & ~1);
  }
}

static void out_rec(const uint8_t *s, int n)
{
  while (n >= 2) {
    int len = (s[0] << 8) | s[1], rl = 2 + ((len + 1) & ~1);
    const uint8_t *pkt = s + 2;
    const struct picosim_svc *sv;
    if (len == 0 || rl > n)
      break;
    int tot = len >= 20 ? (pkt[2] << 8) | pkt[3] : 0;
    if (pkt[0] == ACCEL_MARK)
      accel_submit(pkt, len);
    else if ((sv = svc_for(pkt[0])) != NULL)
      svc_record(sv, pkt, len);
    else if ((pkt[0] >> 4) == 4 && tot >= 20 && tot <= len) {
      st_tx += tot;
      lan_from_z(pkt, tot);             /* xip.x の詰め物 (Ethernet の最小長) は全長で切る */
    } else
      st_bad++;
    s += rl;
    n -= rl;
  }
}

static void out_slip(const uint8_t *s, int n)
{
  static uint8_t pkt[MTU];
  static int pos, esc, bad;
  for (int i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == SLIP_END) {
      int tot = pos >= 20 ? (pkt[2] << 8) | pkt[3] : 0;
      if (pos >= 20 && !bad && (pkt[0] >> 4) == 4 && tot >= 20 && tot <= pos) {
        st_tx += tot;
        lan_from_z(pkt, tot);
      }
      pos = esc = bad = 0;
      continue;
    }
    if (c == SLIP_ESC) {
      esc = 1;
      continue;
    }
    if (esc) {
      c = c == SLIP_ESC_END ? SLIP_END : c == SLIP_ESC_ESC ? SLIP_ESC : c;
      esc = 0;
    }
    if (pos < MTU)
      pkt[pos++] = c;
    else
      bad = 1;
  }
}

static void exchange(void)
{
  uint32_t magic = g32(reqbuf), seq = g32(reqbuf + 4), z;
  int txlen = (reqbuf[8] << 8) | reqbuf[9], rxmax = (reqbuf[10] << 8) | reqbuf[11], n = 0;
  memcpy(&z, reqbuf + 12, 4);
  int r = z != 0 && magic == MAGIC_R;
  st_xchg++;
  if (z != zip || r != raw) {
    zip = z;
    raw = r;
    rx_tail = rx_head;                  /* 運び方が変わったら残りは捨てる */
    lan_set_zip(z);
    if (z != 0) {
      const uint8_t *b = (const uint8_t *)&z;
      picosim_log("netpipe: bridge %d.%d.%d.%d%s\n", b[0], b[1], b[2], b[3], r ? " (records)" : " (SLIP)");
    } else
      picosim_log("netpipe: UDP mode (stat = 0) is not simulated; the request is dropped\n");
  }
  svc_res_len = 0;
  svc_more = 0;
  if (z != 0) {
    echo_reply();
    if (txlen > 0 && raw)
      out_rec(reqbuf + 16, txlen);
    else if (txlen > 0)
      out_slip(reqbuf + 16, txlen);
  }
  if (rxmax > NP_DATAMAX)
    rxmax = NP_DATAMAX;
  uint32_t t = rx_tail, avail = rx_head - t;
  if (raw) {
    int al = accel_result(resbuf + 16 + REC_HDR);
    if (al > 0) {
      p16(resbuf + 16, al);
      memset(resbuf + 18, 0, REC_HDR - 2);
      n = REC_HDR + ((al + 1) & ~1);
    }
    if (svc_res_len > 0 && n + svc_res_len <= rxmax) {
      memcpy(resbuf + 16 + n, svc_res, svc_res_len);
      n += svc_res_len;
    } else if (svc_res_len > 0)
      picosim_log("netpipe: service reply (%d bytes) does not fit, dropped\n", svc_res_len);
    while (avail >= REC_HDR) {
      int len = (rxring[t % RXRING] << 8) | rxring[(t + 1) % RXRING];
      uint32_t rl = REC_HDR + ((len + 1) & ~1);
      if (rl > avail || n + rl > (uint32_t)rxmax)
        break;
      for (uint32_t i = 0; i < rl; i++)
        resbuf[16 + n + i] = rxring[t++ % RXRING];
      n += rl;
      avail -= rl;
    }
  } else {
    n = avail < (uint32_t)rxmax ? (int)avail : rxmax;
    for (int i = 0; i < n; i++)
      resbuf[16 + i] = rxring[t++ % RXRING];
  }
  rx_tail = t;
  p32(resbuf, magic);
  p32(resbuf + 4, seq);
  p16(resbuf + 8, n);
  /* more: リングの残り (ファームと同じ)。サービスがまだ渡すものを持っていれば 1 足す (tsp_svc.h の決まり) */
  p16(resbuf + 10, (rx_head - rx_tail) + (svc_more ? 1 : 0));
  p32(resbuf + 12, (st_tozf << 16) | (st_drop & 0xffff));
}

int np_write(uint32_t lba, const uint8_t *buf)
{
  int page = (int)(lba - NP_WLBA);
  if (page < 0 || page >= NP_SECTS)
    return -1;
  if (page == 0) {
    uint32_t m = g32(buf);
    int len = (buf[8] << 8) | buf[9];
    if ((m != MAGIC_P && m != MAGIC_R) || len > NP_DATAMAX) {
      nsect = 0;
      return -1;
    }
    nsect = (16 + len + 511) / 512;
  }
  memcpy(reqbuf + page * 512, buf, 512);
  if (page == nsect - 1)
    exchange();
  return 0;
}

int np_read(uint32_t lba, uint8_t *buf)
{
  if (lba < NP_RLBA || lba >= NP_REND)
    return -1;
  memcpy(buf, resbuf + (lba % NP_SECTS) * 512, 512);
  return 0;
}

void np_init(void)
{
  for (int i = 0; i < nsvc; i++) {
    picosim_log("service: %s (records 0x%02x)\n", svcs[i]->name, svcs[i]->mark);
    if (svcs[i]->init)
      svcs[i]->init();
  }
}

void np_poll(void)
{
  for (int i = 0; i < nsvc; i++)
    if (svcs[i]->poll)
      svcs[i]->poll();
}

void np_stats(void)
{
  fprintf(stderr, "netpipe: xchg %u  Z->LAN %u B  LAN->Z %u B (%u pkts, %u dropped)  accel %u  svc %u  bad %u\n",
          st_xchg, st_tx, st_rx, st_tozf, st_drop, st_accel, st_svc, st_bad);
}
