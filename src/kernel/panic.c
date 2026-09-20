#include <stdint.h>

#include "kprint.h"
#include "panic.h"

void panic_dump(void)
{
    uint64_t esr, elr, far;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    kprint("PANIC esr=%lx elr=%lx far=%lx\n", esr, elr, far);
}
