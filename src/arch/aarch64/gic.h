#pragma once

#include <stdint.h>

void gic_init(uint64_t hhdm_offset);
void gic_enable(uint32_t intid, uint8_t priority);
uint64_t gic_acknowledge(void);
void gic_eoi(uint64_t iar);
