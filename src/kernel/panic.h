#pragma once

static inline void hcf(void)
{
    for (;;) {
#ifdef __aarch64__
        __asm__ volatile("wfi");
#else
        __asm__ volatile("cli; hlt");
#endif
    }
}

void panic_dump(void);
void kpanic(const char *fmt, ...);
