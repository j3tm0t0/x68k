/* picosim --pzl のホスト用: hardware/sync.h の代わり */
#ifndef PZL_SHIM_SYNC_H
#define PZL_SHIM_SYNC_H
#include <stdint.h>
#include <sched.h>
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }
static inline void __dmb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void __wfe(void) { sched_yield(); }
static inline void __sev(void) {}
#endif
