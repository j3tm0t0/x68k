/* echosvc.c -- 試験用のサービス: 先頭 0xA6 のレコードをそのまま返す (picosim_register_svc の口の試験) */
#include <string.h>
#include "../picosim.h"

/* LAN の口 (lan_attach) からの DNS: 'D' の要求で、口 (192.168.1.118) からゲートウェイの 53 番に localhost を聞き、
   届いた答えの先頭 (rcode と答えの数) を返す */
static const uint8_t mymac[6] = { 0x02, 0, 0, 0, 0, 0x76 };
static int dns_rcode = -1, dns_an = -1;

static void att_rx(const uint8_t *f, int len, void *arg)
{
  (void)arg;
  if (len >= 14 + 28 + 12 && f[23] == 17 && f[34] == 0 && f[35] == 53 && f[42] == 0x44 && f[43] == 0x44) {
    dns_rcode = f[45] & 15;
    dns_an = f[48] << 8 | f[49];
  }
}

static void echo_init(void)
{
  lan_attach(mymac, lan_cfg.net | (118u << 24), att_rx, NULL);   /* .118 (ネットワークバイトオーダー、リトルエンディアンの Mac) */
}

static void send_dns(void)
{
  static const uint8_t q[] = { 0x44, 0x44, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0, 0, 1, 0, 1 };
  uint8_t f[14 + 28 + sizeof q];
  uint32_t me = lan_cfg.net | (118u << 24);
  int ul = 8 + sizeof q, tot = 20 + ul;
  memset(f, 0, sizeof f);
  memset(f, 0xff, 6);
  memcpy(f + 6, mymac, 6);
  f[12] = 0x08;
  f[14] = 0x45;
  f[16] = tot >> 8;
  f[17] = tot;
  f[22] = 64;
  f[23] = 17;
  memcpy(f + 26, &me, 4);
  memcpy(f + 30, &lan_cfg.gw, 4);
  f[34] = 0x10;                         /* 送り元 4096 */
  f[37] = 53;
  f[38] = ul >> 8;
  f[39] = ul;
  memcpy(f + 42, q, sizeof q);
  dns_rcode = dns_an = -1;
  lan_send(f, sizeof f);
}

static int echo_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  if (reqlen >= 2 && req[1] == 'D') {
    send_dns();
    res[0] = 0xa6;
    res[1] = 'D';
    res[2] = (uint8_t)dns_rcode;
    res[3] = (uint8_t)dns_an;
    *more = 0;
    return 4;
  }
  int n = reqlen < resmax ? reqlen : resmax;
  memcpy(res, req, n);
  *more = reqlen >= 2 && req[1] == 'M';     /* 2 バイト目が 'M' なら more を立てる */
  return n;
}

static const struct picosim_svc echo = { "echo (test)", 0xa6, echo_init, echo_exchange, NULL };

__attribute__((constructor)) static void reg(void)
{
  picosim_register_svc(&echo);
}
