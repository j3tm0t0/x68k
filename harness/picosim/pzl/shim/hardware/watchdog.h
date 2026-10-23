/* picosim --pzl のホスト用: pico-sdk の hardware/watchdog.h の代わり (再起動は picosim に知らせるだけ) */
#ifndef PZL_SHIM_WATCHDOG_H
#define PZL_SHIM_WATCHDOG_H
#include <stdint.h>
#include <stdbool.h>
void pzl_host_reboot(void);
static inline void watchdog_enable(uint32_t ms, bool pause) { (void)ms; (void)pause; pzl_host_reboot(); }
#endif
