#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "pmm.h"

#define RAM_BASE 0x40000000ULL
#define RAM_SIZE (16ULL << 20)
#define NFRAMES  (RAM_SIZE >> 12)

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

static void seen(uint64_t *map, uint64_t phys)
{
    uint64_t f = (phys - RAM_BASE) >> 12;
    assert(f < NFRAMES);
    assert(!(map[f >> 6] & (1ULL << (f & 63))));
    map[f >> 6] |= (1ULL << (f & 63));
}

int main(void)
{
    assert(pmm_init(make_map(), (uint64_t)(uintptr_t)fake_ram - RAM_BASE) == 0);
    assert(pmm_free_frames() == NFRAMES);

    /* drain the allocator one frame at a time; every frame must be distinct */
    uint64_t *map = calloc(NFRAMES / 64 + 1, sizeof(uint64_t));
    uint64_t pages[NFRAMES];
    for (uint64_t i = 0; i < NFRAMES; i++) {
        pages[i] = pmm_alloc(0);
        assert(pages[i] != 0);
        seen(map, pages[i]);
    }
    assert(pmm_alloc(0) == 0);
    assert(pmm_free_frames() == 0);

    /* free everything back */
    for (uint64_t i = 0; i < NFRAMES; i++)
        pmm_free(pages[i], 0);
    fprintf(stderr, "after free-all: free=%llu want=%llu\n",
            pmm_free_frames(), (uint64_t)NFRAMES);
    assert(pmm_free_frames() == NFRAMES);

    /* coalescing proof: after freeing everything, a max-order block exists */
    uint64_t big = pmm_alloc(PMM_MAX_ORDER);
    assert(big != 0);
    assert((big % (4096ULL << PMM_MAX_ORDER)) == 0);
    pmm_free(big, PMM_MAX_ORDER);
    assert(pmm_free_frames() == NFRAMES);

    /* ordered allocation returns aligned blocks */
    uint64_t p2 = pmm_alloc(2);
    assert(p2 && (p2 % (4096ULL << 2)) == 0);
    pmm_free(p2, 2);

    /* churn: random order alloc/free, invariants hold throughout */
    uint64_t live[256];
    unsigned live_order[256];
    int nl = 0;
    srand(12345);
    for (int it = 0; it < 20000; it++) {
        if (nl > 0 && (rand() & 1)) {
            int i = rand() % nl;
            pmm_free(live[i], live_order[i]);
            nl--;
            live[i] = live[nl];
            live_order[i] = live_order[nl];
        } else {
            unsigned o = rand() % 4;
            uint64_t p = pmm_alloc(o);
            if (p) {
                assert((p % (4096ULL << o)) == 0);
                live[nl] = p;
                live_order[nl++] = o;
            }
        }
    }
    while (nl > 0) {
        nl--;
        pmm_free(live[nl], live_order[nl]);
    }
    fprintf(stderr, "after churn: free=%llu want=%llu\n",
            pmm_free_frames(), (uint64_t)NFRAMES);
    assert(pmm_free_frames() == NFRAMES);

    printf("pmm_test: PASS\n");
    return 0;
}
