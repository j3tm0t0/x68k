/* picosim --pzl のホスト用: core 1 はスレッド (accel.c の core1_main) */
#ifndef PZL_SHIM_MULTICORE_H
#define PZL_SHIM_MULTICORE_H
void multicore_launch_core1(void (*entry)(void));
static inline void multicore_lockout_victim_init(void) {}
#endif
