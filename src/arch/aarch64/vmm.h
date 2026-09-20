#pragma once

#include <stdint.h>
#include "limine.h"

#define VMM_WRITE  (1u << 0)
#define VMM_EXEC   (1u << 1)
#define VMM_DEVICE (1u << 2)

void vmm_init(uint64_t hhdm_offset, uint64_t kphys, uint64_t kvirt,
              uint64_t ksize, struct limine_memmap_response *map);
