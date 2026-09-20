#pragma once

#include <stdint.h>

struct trap_frame;

typedef void (*irq_fn)(struct trap_frame *);

void irq_register(uint32_t intid, irq_fn fn);
void irq_dispatch(struct trap_frame *frame);
void irq_unmask(void);
void irq_mask(void);
