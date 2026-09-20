#include "mmio.h"

#define DESC_VALID  (1ULL << 0)
#define DESC_BLOCK  (0ULL << 1)
#define DESC_TABLE  (3ULL << 0)
#define DESC_AF     (1ULL << 10)
#define DESC_UXN    (1ULL << 53)
#define DESC_PXN    (1ULL << 54)

#define PHYS_MASK   0x0000fffffffff000ULL

static inline uint64_t read_ttbr1(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(v));
    return v;
}

static inline uint64_t read_mair(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(v));
    return v;
}

static inline uint64_t read_tcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(v));
    return v;
}

static int device_attr_index(uint64_t mair)
{
    for (int i = 0; i < 8; i++) {
        uint8_t a = (mair >> (i * 8)) & 0xff;
        if ((a & 0xf0) == 0)
            return i;
    }
    return -1;
}

int mmio_map_low(uint64_t hhdm_offset)
{
    uint64_t tcr = read_tcr();
    unsigned tg1 = (tcr >> 30) & 0x3;
    unsigned t1sz = (tcr >> 16) & 0x3f;

    if (tg1 != 2 || t1sz != 16)
        return -1;

    int idx = device_attr_index(read_mair());
    if (idx < 0)
        return -2;

    uint64_t va = hhdm_offset;
    uint64_t l0_phys = read_ttbr1() & PHYS_MASK;
    uint64_t *l0 = (uint64_t *)(l0_phys + hhdm_offset);
    uint64_t e0 = l0[(va >> 39) & 0x1ff];
    if ((e0 & 3) != 3)
        return -3;

    uint64_t *l1 = (uint64_t *)((e0 & PHYS_MASK) + hhdm_offset);
    uint64_t *slot = &l1[(va >> 30) & 0x1ff];

    *slot = DESC_VALID | DESC_BLOCK | DESC_AF | DESC_UXN | DESC_PXN |
            ((uint64_t)idx << 2);

    __asm__ volatile("tlbi vmalle1; dsb sy; isb" ::: "memory");
    return 0;
}
