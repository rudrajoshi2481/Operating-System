#pragma once

#include <stdint.h>

int              gpu_init(uint64_t hhdm);
int              gpu_ok(void);
volatile uint32_t *gpu_fb(uint32_t *w, uint32_t *h, uint32_t *pitch);
void             gpu_flush(void);
