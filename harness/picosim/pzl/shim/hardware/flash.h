/* picosim --pzl のホスト用: hardware/flash.h の代わり。書き込みは捨てる (設定はホストでは config.txt の形で渡す) */
#ifndef PZL_SHIM_FLASH_H
#define PZL_SHIM_FLASH_H
#include <stdint.h>
#include <stddef.h>
#define FLASH_SECTOR_SIZE 4096
void pzl_host_flash_erase(uint32_t off, size_t len);
void pzl_host_flash_program(uint32_t off, const uint8_t *data, size_t len);
static inline void flash_range_erase(uint32_t off, size_t len) { pzl_host_flash_erase(off, len); }
static inline void flash_range_program(uint32_t off, const uint8_t *data, size_t len) { pzl_host_flash_program(off, data, len); }
#endif
