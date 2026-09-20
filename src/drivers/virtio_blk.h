#pragma once

#include <stdint.h>

/* Probe virtio-mmio slots, init the first block device found. 0 = ok. */
int      blk_init(uint64_t hhdm);
uint64_t blk_capacity(void);            /* total sectors */
uint32_t blk_sector_size(void);         /* bytes per sector */
int      blk_read(uint64_t lba, void *buf, uint32_t nbytes);
int      blk_write(uint64_t lba, const void *buf, uint32_t nbytes);
void     blk_selftest(void);
