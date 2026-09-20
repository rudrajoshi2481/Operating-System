#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "assert.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "mmio.h"
#include "panic.h"
#include "pmm.h"
#include "uart.h"
#include "vmm.h"
#include "gic.h"
#include "irq.h"
#include "thread.h"
#include "timer.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request addr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

extern char _kernel_end[];

static void heap_selftest(void)
{
    char *a = kmalloc(24);
    char *b = kmalloc(3000);
    char *c = kmalloc(600);
    assert(a && b && c);
    memset(a, 0xab, 24);
    memset(b, 0xcd, 3000);
    assert(a[23] == (char)0xab && b[2999] == (char)0xcd);
    kfree(b);
    kfree(a);
    kfree(c);
    kprint("heap: selftest ok\n");
}

/* Busy loops with no yield() — interleaving proves timer preemption. */
static void spinner(void *arg)
{
    const char *name = arg;
    for (;;) {
        kprint("%s", name);
        for (volatile int i = 0; i < 80000000; i++)
            ;
    }
}

static void sleeper(void *arg)
{
    (void)arg;
    for (;;) {
        ksleep(3);
        kprint("[slept 3 ticks]\n");
    }
}

void kernel_main(void)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        hcf();
    if (!hhdm_request.response || !memmap_request.response ||
        !addr_request.response)
        hcf();

    uint64_t hhdm = hhdm_request.response->offset;
    int mmio_rc = mmio_map_low(hhdm);
    uart_init(hhdm);
    if (mmio_rc != 0)
        kpanic("mmio_map_low failed: %d", mmio_rc);

    kprint("BIOOS_OK hello kernel (limine base rev %lu)\n",
           limine_base_revision[2]);

    int rc = pmm_init(memmap_request.response, hhdm);
    if (rc != 0)
        kpanic("pmm_init failed: %d", rc);
    heap_init();
    vmm_init(hhdm, addr_request.response->physical_base,
             addr_request.response->virtual_base,
             (uint64_t)_kernel_end - addr_request.response->virtual_base,
             memmap_request.response);
    pmm_release_reclaimable(addr_request.response->physical_base,
                            (uint64_t)_kernel_end -
                                addr_request.response->virtual_base);

    kprint("vmm: own page tables, pmm: %lu free frames (%lu MiB)\n",
           pmm_free_frames(), pmm_free_frames() * 4096 / (1024 * 1024));

    heap_selftest();

    sched_init();
    thread_create(spinner, "A");
    thread_create(spinner, "B");
    thread_create(sleeper, 0);

    gic_init(hhdm);
    timer_init();
    irq_unmask();
    kprint("interrupts armed\n");

    /* thread 0 becomes the idle loop */
    for (;;)
        __asm__ volatile("wfi");
}
