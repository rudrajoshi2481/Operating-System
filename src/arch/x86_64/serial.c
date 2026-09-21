/* serial.c — COM1 (16550) UART, polling. Same uart_* API as aarch64. */
#include "uart.h"
#include "io.h"

#define COM1 0x3f8

volatile int uart_ready;

void uart_init(uint64_t hhdm_offset)
{
    (void)hhdm_offset;
    outb(COM1 + 1, 0x00);               /* disable interrupts */
    outb(COM1 + 3, 0x80);               /* DLAB */
    outb(COM1 + 0, 0x01);               /* divisor 1 = 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);               /* 8n1 */
    outb(COM1 + 2, 0xc7);               /* FIFO on, 14B trigger */
    outb(COM1 + 4, 0x0b);               /* DTR+RTS+OUT2 */
    uart_ready = 1;
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void uart_write(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_getc(void)
{
    if (!(inb(COM1 + 5) & 0x01))
        return -1;
    return inb(COM1);
}

void uart_irq_init(void)
{
    /* polled input: the shell's yield loop is enough on x86 */
}
