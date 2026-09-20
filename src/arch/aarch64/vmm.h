#pragma once

#include <stdint.h>
#include "limine.h"

#define VMM_WRITE  (1u << 0)
#define VMM_EXEC   (1u << 1)
#define VMM_DEVICE (1u << 2)
#define VMM_USER   (1u << 3)

void     vmm_init(uint64_t hhdm_offset, uint64_t kphys, uint64_t kvirt,
                  uint64_t ksize, struct limine_memmap_response *map);
uint64_t vmm_new_pgd(void);                     /* zeroed L0 table, phys addr */
int      vmm_map_pages(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t len,
                       unsigned flags);
uint64_t vmm_translate(uint64_t pgd, uint64_t va);  /* ~0 if unmapped */
