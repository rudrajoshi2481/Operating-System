#include "heap.h"
#include "assert.h"
#include "lib.h"
#include "pmm.h"
#include "spinlock.h"

#define SLAB_MAGIC   0x514c4142ul
#define BLK_MAGIC_A  0x626c6b61ul
#define BLK_MAGIC_B  0x626c6b62ul
#define PAGE         4096
#define HDR_RESERVE  32
#define SLAB_HDR     64
#define MAX_SMALL    2048

static const unsigned classes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define NCLASSES (sizeof(classes) / sizeof(classes[0]))

struct slab {
    uint64_t      magic;
    uint32_t      obj_size;
    uint32_t      cls;
    uint16_t      total;
    uint16_t      free_cnt;
    struct slab  *prev;
    struct slab  *next;
    void         *freelist;
};

static struct slab *slabs[NCLASSES];
static spinlock_t   lock;

void heap_init(void)
{
    memset(slabs, 0, sizeof(slabs));
}

static int class_of(size_t n)
{
    for (unsigned i = 0; i < NCLASSES; i++)
        if (n <= classes[i])
            return (int)i;
    return -1;
}

static struct slab *slab_new(int cls)
{
    uint64_t phys = pmm_alloc(0);
    if (!phys)
        return 0;

    struct slab *s = pmm_to_virt(phys);
    s->magic = SLAB_MAGIC;
    s->obj_size = classes[cls];
    s->cls = (uint32_t)cls;
    s->prev = 0;
    s->next = slabs[cls];
    s->freelist = 0;
    if (s->next)
        s->next->prev = s;
    slabs[cls] = s;

    unsigned first = (SLAB_HDR + 15) & ~15u;
    unsigned cap = (PAGE - first) / s->obj_size;
    s->total = (uint16_t)cap;
    s->free_cnt = (uint16_t)cap;

    char *base = (char *)s;
    for (unsigned i = 0; i < cap; i++) {
        void *o = base + first + i * s->obj_size;
        *(void **)o = s->freelist;
        s->freelist = o;
    }
    return s;
}

static void *slab_alloc(int cls)
{
    struct slab *s = slabs[cls];
    if (!s || s->free_cnt == 0) {
        for (s = slabs[cls]; s && s->free_cnt == 0; s = s->next)
            ;
        if (!s)
            s = slab_new(cls);
    }
    if (!s)
        return 0;

    void *o = s->freelist;
    s->freelist = *(void **)o;
    s->free_cnt--;
    return o;
}

static void slab_free(void *obj, struct slab *s)
{
    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->free_cnt++;

    if (s->free_cnt == s->total) {
        uint32_t cls = s->cls;
        if (s->prev)
            s->prev->next = s->next;
        else
            slabs[cls] = s->next;
        if (s->next)
            s->next->prev = s->prev;
        s->magic = 0;
        pmm_free((uint64_t)(uintptr_t)s - pmm_hhdm(), 0);
    }
}

static int order_for(size_t n)
{
    size_t need = n + HDR_RESERVE;
    int o = 0;
    while (((size_t)PAGE << o) < need)
        o++;
    return o;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return 0;

    int cls = class_of(size);
    if (cls < 0) {
        int order = order_for(size);
        if (order > PMM_MAX_ORDER)
            return 0;
        uint64_t phys = pmm_alloc((unsigned)order);
        if (!phys)
            return 0;
        uint64_t *h = pmm_to_virt(phys);
        h[0] = BLK_MAGIC_A;
        h[1] = (uint64_t)order;
        h[2] = BLK_MAGIC_B;
        h[3] = 0;
        return (char *)h + HDR_RESERVE;
    }

    spin_lock(&lock);
    void *o = slab_alloc(cls);
    spin_unlock(&lock);
    return o;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uint64_t *h = (uint64_t *)((char *)ptr - HDR_RESERVE);
    if (h[0] == BLK_MAGIC_A && h[2] == BLK_MAGIC_B) {
        uint64_t order = h[1];
        h[0] = h[1] = h[2] = 0;
        pmm_free((uint64_t)(uintptr_t)h - pmm_hhdm(), (unsigned)order);
        return;
    }

    struct slab *s = (struct slab *)((uintptr_t)ptr & ~(PAGE - 1));
    if (s->magic == SLAB_MAGIC) {
        spin_lock(&lock);
        slab_free(ptr, s);
        spin_unlock(&lock);
        return;
    }

    kpanic("kfree: bad pointer %p", ptr);
}

void *kcalloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (size && total / size != n)
        return 0;
    void *p = kmalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}
