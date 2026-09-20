#include "frame.h"
#include "kprint.h"
#include "pmm.h"
#include "thread.h"
#include "uart.h"
#include "uri.h"
#include "vmm.h"

#define EC_SVC64 0x15

enum { SYS_WRITE, SYS_EXIT, SYS_YIELD, SYS_URI };

/* byte-wise copy through the user page table */
static int ucopy(void *dst, const void *src, uint64_t len, int to_user)
{
    uint64_t pgd = cur_pgd();
    for (uint64_t i = 0; i < len; i++) {
        uint64_t uva = to_user ? (uint64_t)dst + i : (uint64_t)src + i;
        uint64_t pa  = vmm_translate(pgd, uva);
        if (pa == ~0ULL)
            return -1;
        volatile char *kp = (volatile char *)(uintptr_t)(pa + pmm_hhdm());
        if (to_user)
            *kp = ((const char *)src)[i];
        else
            ((char *)dst)[i] = *kp;
    }
    return 0;
}

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

/* sys_uri(uri_ptr, buf, len): read through the URI gate into user buf */
static uint64_t sys_uri(struct trap_frame *f)
{
    char uri[128];
    uint64_t uptr = f->x[0];
    uint32_t i = 0;
    for (; i < sizeof(uri) - 1; i++) {
        if (ucopy(&uri[i], (const void *)(uintptr_t)(uptr + i), 1, 0))
            return (uint64_t)-1;
        if (uri[i] == 0)
            break;
    }
    uri[i] = 0;

    static char kbuf[2048];
    int n = uri_read(uri, kbuf, sizeof(kbuf));
    if (n < 0)
        return (uint64_t)-1;
    uint32_t cap = (uint32_t)f->x[2];
    if ((uint32_t)n > cap)
        n = (int)cap;
    if (ucopy((void *)(uintptr_t)f->x[1], kbuf, (uint32_t)n, 1))
        return (uint64_t)-1;
    return (uint64_t)n;
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
        case SYS_URI:
            ret = sys_uri(f);
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
