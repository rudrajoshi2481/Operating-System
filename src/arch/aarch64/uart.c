#include "uart.h"
#include "irq.h"
#include "gic.h"
#include "frame.h"

#define UART0_PHYS 0x09000000UL
#define UART_INTID 33               /* PL011 uart0 on the virt machine */
#define UART_DR    0x00
#define UART_FR    0x18
#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)
#define UART_IMSC  0x38
#define UART_MIS   0x40
#define UART_ICR   0x44
#define UART_INT_RX (1u << 4)
#define UART_INT_RT (1u << 6)

#define RX_RING 2048

static volatile uint32_t *uart;

volatile int uart_ready;

/* ISR -> shell ring buffer; the PL011 FIFO is only 16 deep, so RX must
 * be drained at interrupt time — a polled shell thread loses bursts. */
static uint8_t          rx_ring[RX_RING];
static volatile uint32_t rx_head, rx_tail;

static void uart_irq(struct trap_frame *f)
{
    (void)f;
    uint32_t mis = uart[UART_MIS / 4];
    uart[UART_ICR / 4] = mis;
    while (!(uart[UART_FR / 4] & UART_FR_RXFE)) {
        uint32_t n = (rx_head + 1) % RX_RING;
        if (n == rx_tail)
            break;                  /* ring full: drop */
        rx_ring[rx_head] = (uint8_t)uart[UART_DR / 4];
        rx_head = n;
    }
}

void uart_init(uint64_t hhdm_offset)
{
    uart = (volatile uint32_t *)(UART0_PHYS + hhdm_offset);
    uart_ready = 1;
}

void uart_irq_init(void)
{
    irq_register(UART_INTID, uart_irq);
    uart[UART_IMSC / 4] = UART_INT_RX | UART_INT_RT;
    gic_enable(UART_INTID, 0x90);
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

int uart_getc(void)
{
    if (!uart_ready)
        return -1;
    if (rx_tail == rx_head)
        return -1;
    int c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_RING;
    return c;
}
