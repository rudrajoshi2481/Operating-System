#pragma once

#include <stdint.h>

extern volatile int uart_ready;

void uart_init(uint64_t hhdm_offset);
void uart_putc(char c);
void uart_write(const char *s);
int  uart_getc(void);
void uart_irq_init(void);
