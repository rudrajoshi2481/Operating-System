#include <stdarg.h>
#include <stdint.h>

#include "kprint.h"
#include "panic.h"

void panic_dump(void)
{
#ifdef __aarch64__
    uint64_t esr, elr, far;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    kprint("PANIC esr=%lx elr=%lx far=%lx\n", esr, elr, far);
#else
    kprint("PANIC (x86_64)\n");
#endif
}

void kpanic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kprint("PANIC: ");
    kvprint(fmt, ap);
    kprint("\n");
    va_end(ap);
    hcf();
}
