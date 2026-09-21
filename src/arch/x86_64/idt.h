#pragma once

#include <stdint.h>
#include "irq.h"

void x86_irq_init(void);                /* IDT + PIC */
