/* picosim --pzl のホスト用: pico/time.h の代わり (時計は picosim の起動からの µs) */
#ifndef PZL_SHIM_PICO_TIME_H
#define PZL_SHIM_PICO_TIME_H
#include <stdint.h>
#include <stdbool.h>
uint64_t pzlh_now_us(void);
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return pzlh_now_us(); }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return pzlh_now_us() + (uint64_t)ms * 1000; }
static inline int64_t absolute_time_diff_us(absolute_time_t a, absolute_time_t b) { return (int64_t)(b - a); }
static inline bool time_reached(absolute_time_t t) { return pzlh_now_us() >= t; }
static inline uint32_t time_us_32(void) { return (uint32_t)pzlh_now_us(); }
static inline uint64_t time_us_64(void) { return pzlh_now_us(); }
void sleep_ms(uint32_t ms);
#endif
