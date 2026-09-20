#pragma once

#include <stdint.h>
#include "limine.h"

#define PMM_MAX_ORDER 10

int      pmm_init(struct limine_memmap_response *map, uint64_t hhdm_offset);
void     pmm_release_reclaimable(uint64_t kphys, uint64_t ksize);
uint64_t pmm_alloc(unsigned order);
void     pmm_free(uint64_t phys, unsigned order);
uint64_t pmm_free_frames(void);
void    *pmm_to_virt(uint64_t phys);
uint64_t pmm_hhdm(void);
