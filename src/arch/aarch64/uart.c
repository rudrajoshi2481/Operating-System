#include "uart.h"

#define UART0_PHYS 0x09000000UL
#define UART_DR    0x00
#define UART_FR    0x18
#define UART_FR_TXFF (1u << 5)

static volatile uint32_t *uart;

volatile int uart_ready;

void uart_init(uint64_t hhdm_offset)
{
    uart = (volatile uint32_t *)(UART0_PHYS + hhdm_offset);
    uart_ready = 1;
}

void uart_putc(char c)
{
    while (uart[UART_FR / 4] & UART_FR_TXFF)
        ;
    uart[UART_DR / 4] = (uint32_t)c;
}

void uart_write(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
