#include "vmm.h"
#include "lib.h"
#include "pmm.h"

#define PTE_VALID   (1ULL << 0)
#define PTE_TABLE   (1ULL << 1)
#define PTE_PAGE    (1ULL << 1)
#define PTE_BLOCK   (0ULL << 1)
#define PTE_AF      (1ULL << 10)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_UXN     (1ULL << 54)
#define PTE_PXN     (1ULL << 53)
#define PTE_AP_RW   (0ULL << 6)
#define PTE_AP_RO   (2ULL << 6)

#define ATTR_DEVICE 0ULL
#define ATTR_NORMAL 1ULL

#define MAIR_VALUE  0x000000000044ff00ULL   /* 0: dev nGnRnE, 1: normal WB, 2: NC */
#define TCR_VALUE   0x00000005b5103510ULL   /* 4K granule, 48-bit, EPD0=0: TTBR0 walks on */

#define TABLE_MASK  0x0000fffffffff000ULL

static uint64_t hhdm;

static uint64_t *new_table(void)
{
    uint64_t phys = pmm_alloc(0);
    if (!phys)
        return 0;
    uint64_t *t = (uint64_t *)(phys + hhdm);
    memset(t, 0, 4096);
    return t;
}

static uint64_t leaf_desc(uint64_t pa, unsigned flags, int page_level)
{
    uint64_t d = (pa & 0x0000ffffffffffffULL) | PTE_VALID | PTE_AF;
    d |= page_level ? PTE_PAGE : PTE_BLOCK;
    if (flags & VMM_DEVICE) {
        d |= ATTR_DEVICE << 2;
        d |= PTE_UXN | PTE_PXN;
    } else {
        d |= (ATTR_NORMAL << 2) | PTE_SH_INNER;
        if (flags & VMM_USER) {
            /* AP[7:6]: 01 = RW at EL0+EL1, 11 = RO at EL0+EL1 */
            d |= (flags & VMM_WRITE) ? (1ULL << 6) : (3ULL << 6);
            if (!(flags & VMM_EXEC))
                d |= PTE_UXN;
            d |= PTE_PXN;          /* kernel never executes user pages */
        } else {
            if (!(flags & VMM_WRITE))
                d |= PTE_AP_RO;
            if (!(flags & VMM_EXEC))
                d |= PTE_PXN;
            d |= PTE_UXN;          /* EL0 never touches kernel pages */
        }
    }
    return d;
}

static int map_page(uint64_t *l0, uint64_t va, uint64_t pa, unsigned flags)
{
    uint64_t *tab = l0;
    for (int level = 0; level < 3; level++) {
        unsigned shift = 39 - level * 9;
        uint64_t e = tab[(va >> shift) & 0x1ff];
        if ((e & 3) != 3) {
            uint64_t *nt = new_table();
            if (!nt)
                return -1;
            tab[(va >> shift) & 0x1ff] =
                (((uint64_t)(uintptr_t)nt - hhdm) & TABLE_MASK) | PTE_VALID | PTE_TABLE;
            e = tab[(va >> shift) & 0x1ff];
        }
        tab = (uint64_t *)((e & TABLE_MASK) + hhdm);
    }
    tab[(va >> 12) & 0x1ff] = leaf_desc(pa, flags, 1);
    return 0;
}

static int map_range(uint64_t *l0, uint64_t va, uint64_t pa, uint64_t len,
                     unsigned flags)
{
    while (len) {
        uint64_t size = 4096;
        if ((va & 0x3fffffff) == 0 && (pa & 0x3fffffff) == 0 && len >= (1ULL << 30))
            size = 1ULL << 30;
        else if ((va & 0x1fffff) == 0 && (pa & 0x1fffff) == 0 && len >= (2ULL << 20))
            size = 2ULL << 20;

        if (size == 4096) {
            if (map_page(l0, va, pa, flags) != 0)
                return -1;
        } else {
            int level = (size == (1ULL << 30)) ? 1 : 2;
            unsigned shift = level == 1 ? 30 : 21;
            uint64_t *tab = l0;
            for (int l = 0; l < level; l++) {
                unsigned s = 39 - l * 9;
                uint64_t e = tab[(va >> s) & 0x1ff];
                if ((e & 3) != 3) {
                    uint64_t *nt = new_table();
                    if (!nt)
                        return -1;
                    tab[(va >> s) & 0x1ff] =
                        (((uint64_t)(uintptr_t)nt - hhdm) & TABLE_MASK) |
                        PTE_VALID | PTE_TABLE;
                    e = tab[(va >> s) & 0x1ff];
                }
                tab = (uint64_t *)((e & TABLE_MASK) + hhdm);
            }
            tab[(va >> shift) & 0x1ff] = leaf_desc(pa, flags, 0);
        }
        va += size;
        pa += size;
        len -= size;
    }
    return 0;
}

void vmm_init(uint64_t hhdm_offset, uint64_t kphys, uint64_t kvirt,
              uint64_t ksize, struct limine_memmap_response *map)
{
    hhdm = hhdm_offset;

    uint64_t *l0 = new_table();
    if (!l0)
        return;

    map_range(l0, kvirt, kphys, ksize, VMM_WRITE | VMM_EXEC);
    map_range(l0, hhdm, 0, 0x40000000ULL, VMM_WRITE | VMM_DEVICE);

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE &&
            e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
            e->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
            continue;
        uint64_t s = e->base & ~4095ULL;
        uint64_t end = (e->base + e->length + 4095) & ~4095ULL;
        map_range(l0, hhdm + s, s, end - s, VMM_WRITE);
    }

    uint64_t *empty = new_table();
    if (!empty)
        return;

    uint64_t l0_phys = (uint64_t)(uintptr_t)l0 - hhdm;
    uint64_t empty_phys = (uint64_t)(uintptr_t)empty - hhdm;

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1,  %1\n"
        "msr ttbr0_el1, %2\n"
        "msr ttbr1_el1, %3\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        :: "r"(MAIR_VALUE), "r"(TCR_VALUE), "r"(empty_phys), "r"(l0_phys)
        : "memory");
}

uint64_t vmm_new_pgd(void)
{
    uint64_t *t = new_table();
    return t ? (uint64_t)(uintptr_t)t - hhdm : 0;
}

int vmm_map_pages(uint64_t pgd, uint64_t va, uint64_t pa, uint64_t len,
                  unsigned flags)
{
    return map_range((uint64_t *)(uintptr_t)(pgd + hhdm), va, pa, len, flags);
}

uint64_t vmm_translate(uint64_t pgd, uint64_t va)
{
    uint64_t *t = (uint64_t *)(uintptr_t)(pgd + hhdm);
    for (int level = 0; level < 4; level++) {
        unsigned shift = 39 - level * 9;
        uint64_t e = t[(va >> shift) & 0x1ff];
        if (!(e & 1))
            return ~0ULL;
        if ((e & 3) == 3 && level < 3) {         /* table pointer */
            t = (uint64_t *)((e & TABLE_MASK) + hhdm);
            continue;
        }
        /* block or page leaf */
        uint64_t pgsz = 1ULL << (12 + (3 - level) * 9);
        return (e & TABLE_MASK) | (va & (pgsz - 1));
    }
    return ~0ULL;
}
