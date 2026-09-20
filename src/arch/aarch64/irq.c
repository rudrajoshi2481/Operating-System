#include "irq.h"
#include "gic.h"
#include "frame.h"
#include "kprint.h"

static irq_fn handlers[1024];

void irq_register(uint32_t intid, irq_fn fn)
{
    if (intid < 1024)
        handlers[intid] = fn;
}

void irq_dispatch(struct trap_frame *frame)
{
    uint64_t iar = gic_acknowledge();
    uint32_t intid = iar & 0xffffff;

    if (intid >= 1020)
        return;

    if (handlers[intid])
        handlers[intid](frame);
    else
        kprint("irq: unhandled intid %u\n", intid);

    gic_eoi(iar);
}

void irq_unmask(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

void irq_mask(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}
