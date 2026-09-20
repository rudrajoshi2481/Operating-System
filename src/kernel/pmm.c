#include "pmm.h"
#include "assert.h"
#include "lib.h"
#include "spinlock.h"

#define PAGE_SIZE     4096ULL
#define MAX_BYTES     (16ULL << 30)            /* cap managed RAM at 16 GiB */
#define MAX_FRAMES    (MAX_BYTES >> 12)
#define MAX_BITS      (2 * MAX_FRAMES)         /* per-order bitmap, 1 bit/block */

#define USABLE(t)  ((t) == LIMINE_MEMMAP_USABLE)

struct free_node {
    struct free_node *next;
    struct free_node *prev;
};

static struct free_node *flist[PMM_MAX_ORDER + 1];
static uint8_t           bm[MAX_BITS / 8];
static uint64_t          order_off[PMM_MAX_ORDER + 2];
static uint64_t          ram_base, nframes, hhdm, free_count;
static spinlock_t        lock;

/* Limine's memmap lives in bootloader-reclaimable memory — freeing those
 * frames would overwrite the map mid-iteration, so snapshot it up front. */
struct mmap_entry {
    uint64_t base, length, type;
};
#define SNAP_MAX 128
static struct mmap_entry snap[SNAP_MAX];
static uint64_t          snap_count;

/* Physical span the running kernel occupies — never returned to the
 * allocator (kernel text stays resident, like Linux's memblock_reserve
 * of _text.._end). */
static uint64_t          keep_base, keep_end;

static int bit_get(unsigned order, uint64_t blk)
{
    uint64_t i = order_off[order] + blk;
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(unsigned order, uint64_t blk)
{
    uint64_t i = order_off[order] + blk;
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static void bit_clear(unsigned order, uint64_t blk)
{
    uint64_t i = order_off[order] + blk;
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint64_t blk_phys(unsigned order, uint64_t blk)
{
    return ram_base + (blk << (12 + order));
}

static void *p2v(uint64_t phys)
{
    return (void *)(phys + hhdm);
}

static void list_push(unsigned order, uint64_t blk)
{
    struct free_node *n = p2v(blk_phys(order, blk));
    n->prev = 0;
    n->next = flist[order];
    if (n->next)
        n->next->prev = n;
    flist[order] = n;
}

static void list_remove(unsigned order, uint64_t blk)
{
    struct free_node *n = p2v(blk_phys(order, blk));
    if (n->prev)
        n->prev->next = n->next;
    else
        flist[order] = n->next;
    if (n->next)
        n->next->prev = n->prev;
}

static void insert_block(unsigned order, uint64_t blk)
{
    bit_set(order, blk);
    list_push(order, blk);
    free_count += (1ULL << order);
}

static void release_range(uint64_t start, uint64_t end)
{
    if (keep_base < keep_end && start < keep_end && end > keep_base) {
        if (start < keep_base)
            release_range(start, keep_base);
        if (end > keep_end)
            release_range(keep_end, end);
        return;
    }

    uint64_t pos = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    uint64_t limit = ram_base + (nframes << 12);
    if (end > limit)
        end = limit;

    while (pos < end) {
        unsigned o = 0;
        while (o < PMM_MAX_ORDER &&
               (pos % (PAGE_SIZE << (o + 1))) == 0 &&
               pos + (PAGE_SIZE << (o + 1)) <= end)
            o++;
        insert_block(o, (pos - ram_base) >> (12 + o));
        pos += PAGE_SIZE << o;
    }
}

static void release_typed(uint64_t type)
{
    for (uint64_t i = 0; i < snap_count; i++)
        if (snap[i].type == type)
            release_range(snap[i].base, snap[i].base + snap[i].length);
}

int pmm_init(struct limine_memmap_response *map, uint64_t hhdm_offset)
{
    if (map == 0)
        return -1;

    hhdm = hhdm_offset;
    ram_base = ~(uint64_t)0;
    uint64_t top = 0;

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *e = map->entries[i];
        if (!USABLE(e->type) || e->length == 0)
            continue;
        if (e->base < ram_base)
            ram_base = e->base;
        if (e->base + e->length > top)
            top = e->base + e->length;
    }
    if (ram_base == ~(uint64_t)0 || top <= ram_base)
        return -2;

    if (top - ram_base > MAX_BYTES)
        top = ram_base + MAX_BYTES;
    nframes = (top - ram_base) >> 12;

    uint64_t off = 0;
    for (unsigned o = 0; o <= PMM_MAX_ORDER + 1; o++) {
        order_off[o] = off;
        off += (nframes >> o) + 1;
    }
    assert(off <= MAX_BITS);

    snap_count = map->entry_count < SNAP_MAX ? map->entry_count : SNAP_MAX;
    for (uint64_t i = 0; i < snap_count; i++) {
        snap[i].base = map->entries[i]->base;
        snap[i].length = map->entries[i]->length;
        snap[i].type = map->entries[i]->type;
    }

    release_typed(LIMINE_MEMMAP_USABLE);
    return 0;
}

void pmm_release_reclaimable(uint64_t kphys, uint64_t ksize)
{
    keep_base = kphys & ~(PAGE_SIZE - 1);
    keep_end = (kphys + ksize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    spin_lock(&lock);
    release_typed(LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE);
    release_typed(LIMINE_MEMMAP_EXECUTABLE_AND_MODULES);
    spin_unlock(&lock);
}

uint64_t pmm_alloc(unsigned order)
{
    if (order > PMM_MAX_ORDER)
        return 0;

    spin_lock(&lock);
    unsigned o = order;
    while (o <= PMM_MAX_ORDER && flist[o] == 0)
        o++;
    if (o > PMM_MAX_ORDER) {
        spin_unlock(&lock);
        return 0;
    }

    /* the free node lives at the block's physical address */
    uint64_t blk = (((uint64_t)(uintptr_t)flist[o]) - hhdm - ram_base) >> (12 + o);
    list_remove(o, blk);
    bit_clear(o, blk);
    free_count -= (1ULL << o);

    while (o > order) {
        o--;
        insert_block(o, 2 * blk + 1);
        blk = 2 * blk;
    }

    spin_unlock(&lock);
    return blk_phys(order, blk);
}

void pmm_free(uint64_t phys, unsigned order)
{
    if (order > PMM_MAX_ORDER || phys < ram_base || phys >= ram_base + (nframes << 12))
        return;

    spin_lock(&lock);
    uint64_t blk = (phys - ram_base) >> (12 + order);

    while (order < PMM_MAX_ORDER) {
        uint64_t buddy = blk ^ 1;
        if ((buddy << order) >= nframes || !bit_get(order, buddy))
            break;
        bit_clear(order, buddy);
        list_remove(order, buddy);
        free_count -= (1ULL << order);
        blk >>= 1;
        order++;
    }

    insert_block(order, blk);
    spin_unlock(&lock);
}

uint64_t pmm_free_frames(void)
{
    return free_count;
}

void *pmm_to_virt(uint64_t phys)
{
    return p2v(phys);
}

uint64_t pmm_hhdm(void)
{
    return hhdm;
}
