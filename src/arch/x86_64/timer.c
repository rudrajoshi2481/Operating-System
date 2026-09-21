/* timer.c — x86_64 PIT (8254) channel 0, IRQ0 at 100 Hz. */
#include "timer.h"
#include "io.h"
#include "thread.h"

static uint64_t ticks;

void timer_init(void)
{
    uint16_t div = 11932;               /* 1193182 Hz / 100 */
    outb(0x43, 0x36);                   /* ch0, lo+hi, mode 3 */
    outb(0x40, div & 0xff);
    outb(0x40, div >> 8);
}

/* called from irq_dispatch with interrupts masked, EOI already sent */
void timer_tick_irq(void)
{
    ticks++;
    sched_tick();
}

uint64_t timer_ticks(void)
{
    return ticks;
}
