#include <stdint.h>

#include "timer.h"
#include "gic.h"
#include "irq.h"
#include "frame.h"
#include "kprint.h"
#include "thread.h"

#define TIMER_INTID 30

static uint64_t freq;
static volatile uint64_t ticks;

static void timer_rearm(void)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(freq));
}

uint64_t timer_ticks(void)
{
    return ticks;
}

static void on_tick(struct trap_frame *frame)
{
    (void)frame;
    ticks++;
    timer_rearm();
    kprint("tick %lu\n", ticks);
    sched_tick();
}

void timer_init(void)
{
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    gic_enable(TIMER_INTID, 0x90);
    irq_register(TIMER_INTID, on_tick);

    timer_rearm();
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));
}
