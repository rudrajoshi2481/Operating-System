#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "assert.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#ifdef __aarch64__
#include "mmio.h"
#endif
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "shell.h"
#include "uart.h"
#ifdef __aarch64__
#include "vmm.h"
#include "gic.h"
#endif
#include "irq.h"
#include "thread.h"
#include "timer.h"
#include "virtio_blk.h"
#include "virtio_console.h"
#include "hostlink.h"
#include "objstore.h"
#include "prov.h"
#include "instr.h"
#include "fb.h"
#include "ui.h"
#ifdef __x86_64__
#include "idt.h"
#endif

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
#ifdef __aarch64__
extern char _user_hello_start[];
#endif

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

void kernel_main(void)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        hcf();
    if (!hhdm_request.response || !memmap_request.response ||
        !addr_request.response)
        hcf();

    uint64_t hhdm = hhdm_request.response->offset;
#ifdef __aarch64__
    int mmio_rc = mmio_map_low(hhdm);
#endif
    uart_init(hhdm);
#ifdef __aarch64__
    if (mmio_rc != 0)
        kpanic("mmio_map_low failed: %d", mmio_rc);
#endif

    kprint("BIOOS_OK hello kernel (limine base rev %lu)\n",
           limine_base_revision[2]);

#ifdef __x86_64__
    x86_irq_init();             /* early: exceptions dump instead of
                                   triple-faulting during device init */
#endif

    int rc = pmm_init(memmap_request.response, hhdm);
    if (rc != 0)
        kpanic("pmm_init failed: %d", rc);
    heap_init();
#ifdef __aarch64__
    vmm_init(hhdm, addr_request.response->physical_base,
             addr_request.response->virtual_base,
             (uint64_t)_kernel_end - addr_request.response->virtual_base,
             memmap_request.response);
    pmm_release_reclaimable(addr_request.response->physical_base,
                            (uint64_t)_kernel_end -
                                addr_request.response->virtual_base);
#endif
    /* x86_64: keep Limine's page tables; reclaimable regions stay
     * reserved so the bootloader-owned structures are never freed. */

    kprint("vmm: own page tables, pmm: %lu free frames (%lu MiB)\n",
           pmm_free_frames(), pmm_free_frames() * 4096 / (1024 * 1024));

    heap_selftest();
    fb_init(hhdm);

    if (blk_init(hhdm) == 0)
        kprint("virtio-blk: not found\n");
    else if (blk_esp())
        blk_selftest(blk_esp());

    if (blk_store()) {
        if (obj_mount(blk_store()) == 0) {
            prov_rebuild();
            instr_init();
            obj_selftest();
        } else {
            kprint("objstore: mount failed\n");
        }
    } else {
        kprint("objstore: no store disk\n");
    }

    sched_init();
#ifdef __aarch64__
    proc_exec(_user_hello_start);
#endif
    thread_create(shell_main, 0);
    thread_create(hostlink_main, 0);
    ui_start();                         /* auto-render if fb exists */

#ifdef __aarch64__
    gic_init(hhdm);
    uart_irq_init();
#endif
    if (con_init(hhdm) != 0)
        kprint("virtio-console: not found\n");
    timer_init();
    irq_unmask();
    kprint("interrupts armed\n");

    /* thread 0 becomes the idle loop */
    for (;;) {
#ifdef __aarch64__
        __asm__ volatile("wfi");
#else
        __asm__ volatile("sti; hlt");   /* hlt until the next IRQ */
#endif
    }
}
