#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "kprint.h"
#include "mmio.h"
#include "uart.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

static void hcf(void)
{
    for (;;)
        __asm__ volatile("wfi");
}

void kernel_main(void)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        hcf();
    if (hhdm_request.response == NULL)
        hcf();
    if (mmio_map_low(hhdm_request.response->offset) != 0)
        hcf();

    uart_init(hhdm_request.response->offset);
    kprint("BIOOS_OK hello kernel (limine base rev %lu)\n", limine_base_revision[2]);
    hcf();
}
