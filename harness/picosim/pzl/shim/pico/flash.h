/* picosim --pzl のホスト用 */
#ifndef PZL_SHIM_PICO_FLASH_H
#define PZL_SHIM_PICO_FLASH_H
#define PICO_OK 0
static inline int flash_safe_execute(void (*fn)(void *), void *p, unsigned t) { (void)t; fn(p); return PICO_OK; }
#endif
