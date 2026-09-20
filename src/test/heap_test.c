#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "heap.h"
#include "pmm.h"

#define RAM_BASE 0x40000000ULL
#define RAM_SIZE (32ULL << 20)

static uint8_t fake_ram[RAM_SIZE] __attribute__((aligned(4096)));

static struct limine_memmap_response *make_map(void)
{
    static struct limine_memmap_entry e;
    static struct limine_memmap_entry *list[1];
    static struct limine_memmap_response r;
    e.base = RAM_BASE;
    e.length = RAM_SIZE;
    e.type = LIMINE_MEMMAP_USABLE;
    list[0] = &e;
    r.revision = 0;
    r.entry_count = 1;
    r.entries = list;
    return &r;
}

int main(void)
{
    assert(pmm_init(make_map(), (uint64_t)(uintptr_t)fake_ram - RAM_BASE) == 0);
    heap_init();

    /* every size class + large allocs */
    size_t sizes[] = { 1, 8, 16, 17, 24, 33, 64, 100, 128, 200, 256,
                       500, 512, 1000, 1024, 1500, 2048, 2049, 3000,
                       4096, 8192, 20000, 100000 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *p = kmalloc(sizes[i]);
        assert(p != 0);
        memset(p, 0xa5, sizes[i]);
        assert(((uint8_t *)p)[sizes[i] - 1] == 0xa5);
        kfree(p);
    }

    /* kcalloc zeroing */
    uint8_t *z = kcalloc(64, 32);
    assert(z != 0);
    for (int i = 0; i < 64 * 32; i++)
        assert(z[i] == 0);
    kfree(z);

    /* churn: interleaved alloc/free of random sizes */
    void *live[512];
    size_t livesz[512];
    uint8_t livepat[512];
    int nl = 0;
    srand(999);
    for (int it = 0; it < 40000; it++) {
        if (nl > 0 && (rand() % 3)) {
            int i = rand() % nl;
            for (size_t b = 0; b < livesz[i]; b += 97)
                assert(((uint8_t *)live[i])[b] == livepat[i]);
            kfree(live[i]);
            nl--;
            live[i] = live[nl];
            livesz[i] = livesz[nl];
            livepat[i] = livepat[nl];
        } else if (nl < 512) {
            size_t sz = 1 + rand() % 5000;
            void *p = kmalloc(sz);
            assert(p != 0);
            uint8_t pat = (uint8_t)(1 + rand() % 255);
            for (size_t b = 0; b < sz; b += 97)
                ((uint8_t *)p)[b] = pat;
            live[nl] = p;
            livesz[nl] = sz;
            livepat[nl++] = pat;
        }
    }
    while (nl > 0)
        kfree(live[--nl]);


    printf("heap_test: PASS\n");
    return 0;
}
