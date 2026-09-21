#pragma once

#include <stdint.h>

void timer_init(void);
void timer_tick_irq(void);
uint64_t timer_ticks(void);
