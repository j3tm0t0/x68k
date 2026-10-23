/* picosim --pzl のホスト用: pico/rand.h の代わり */
#ifndef PZL_SHIM_RAND_H
#define PZL_SHIM_RAND_H
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t get_rand_32(void) { return (uint32_t)random(); }
static inline uint64_t get_rand_64(void) { return ((uint64_t)random() << 32) | (uint32_t)random(); }
#endif
