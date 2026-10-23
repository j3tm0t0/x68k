/* picosim --pzl のホスト用: cyw43 (Wi-Fi) の代わり。netif は picosim の LAN の 1 台 (pzl_host.c) */
#ifndef PZL_SHIM_CYW43_ARCH_H
#define PZL_SHIM_CYW43_ARCH_H
#include <stdint.h>
#include "pico/stdlib.h"
#include "lwip/netif.h"
#define CYW43_ITF_STA 0
#define CYW43_ITF_AP 1
#define CYW43_LINK_UP 3
#define CYW43_WL_GPIO_LED_PIN 0
typedef struct { struct netif netif[2]; } cyw43_t;
extern cyw43_t cyw43_state;
typedef struct { uint32_t version; } cyw43_wifi_scan_options_t;
typedef struct {
  uint32_t _0[5];
  uint8_t bssid[6];
  uint16_t _1;
  uint8_t ssid_len;
  uint8_t ssid[32];
  uint32_t _2[5];
  uint16_t channel;
  uint16_t _3;
  uint8_t auth_mode;
  int16_t rssi;
} cyw43_ev_scan_result_t;
static inline void cyw43_arch_lwip_begin(void) {}
static inline void cyw43_arch_lwip_end(void) {}
void pzlh_net_poll(void);
static inline void cyw43_arch_poll(void) { pzlh_net_poll(); }
static inline void cyw43_arch_wait_for_work_until(uint64_t t) { (void)t; }
static inline int cyw43_wifi_scan(cyw43_t *s, cyw43_wifi_scan_options_t *o, void *env, int (*cb)(void *, const cyw43_ev_scan_result_t *))
{ (void)s; (void)o; (void)env; (void)cb; return -1; }   /* ホストでは Wi-Fi を探さない */
static inline int cyw43_wifi_scan_active(cyw43_t *s) { (void)s; return 0; }
static inline void cyw43_arch_gpio_put(int pin, int v) { (void)pin; (void)v; }
#endif
