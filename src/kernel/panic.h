#pragma once

static inline void hcf(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

void panic_dump(void);
void kpanic(const char *fmt, ...);
