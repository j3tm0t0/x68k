/* echosvc.c -- 試験用のサービス: 先頭 0xA6 のレコードをそのまま返す (picosim_register_svc の口の試験) */
#include <string.h>
#include "../picosim.h"

static int echo_exchange(const uint8_t *req, int reqlen, uint8_t *res, int resmax, int *more)
{
  int n = reqlen < resmax ? reqlen : resmax;
  memcpy(res, req, n);
  *more = reqlen >= 2 && req[1] == 'M';     /* 2 バイト目が 'M' なら more を立てる */
  return n;
}

static const struct picosim_svc echo = { "echo (test)", 0xa6, NULL, echo_exchange, NULL };

__attribute__((constructor)) static void reg(void)
{
  picosim_register_svc(&echo);
}
