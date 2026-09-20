#include "frame.h"
#include "kprint.h"
#include "pmm.h"
#include "thread.h"
#include "uart.h"
#include "vmm.h"

#define EC_SVC64 0x15

enum { SYS_WRITE, SYS_EXIT, SYS_YIELD };

/* Copy a user buffer to the console, page by page. */
static uint64_t sys_write(struct trap_frame *f)
{
    uint64_t buf = f->x[1];
    uint64_t len = f->x[2];
    uint64_t pgd = cur_pgd();

    if (len > 1 << 20)
        return (uint64_t)-1;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t pa = vmm_translate(pgd, buf + i);
        if (pa == ~0ULL)
            return (uint64_t)-1;
        uart_putc(*(volatile char *)(uintptr_t)(pa + pmm_hhdm()));
    }
    return len;
}

void sync_lower(struct trap_frame *f)
{
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    uint64_t ec = (esr >> 26) & 0x3f;

    if (ec == EC_SVC64) {
        uint64_t nr = f->x[8];
        uint64_t ret;
        switch (nr) {
        case SYS_WRITE:
            ret = sys_write(f);
            break;
        case SYS_EXIT:
            kprint("[proc exited code=%lu]\n", f->x[0]);
            thread_exit();      /* does not return */
            ret = 0;
            break;
        case SYS_YIELD:
            yield();
            ret = 0;
            break;
        default:
            ret = (uint64_t)-1;
            break;
        }
        f->x[0] = ret;
        return;
    }

    /* Any other lower-EL sync exception is a user fault — kill it. */
    uint64_t far, elr;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    kprint("user fault: ec=%lx esr=%lx far=%lx elr=%lx\n", ec, esr, far, elr);
    thread_exit();
}
