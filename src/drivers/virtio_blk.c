/*
 * virtio_blk.c — virtio-blk on the shared virtio-mmio transport.
 * Synchronous requests: one descriptor chain (hdr -> data -> status)
 * at a time, completion polled on the used ring.
 */
#include "virtio_blk.h"
#include "virtio_mmio.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "spinlock.h"

#define BLK_T_IN    0                   /* read: device -> driver */
#define BLK_T_OUT   1                   /* write: driver -> device */
#define QSIZE       128
#define TIMEOUT     0x1000000
#define DATA_MAX    4096                /* one page of payload per req */

struct blk_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct vdev       dev;
static struct virtqueue  q;
static uint64_t          capacity;      /* sectors */
static uint32_t          sec_size;      /* bytes */
static uint64_t          hdr_phys, data_phys;
static volatile uint8_t  *hdr_va, *data_va;
static spinlock_t        lock;
static int               ready;

int blk_init(uint64_t hhdm)
{
    if (vio_probe(hhdm, VIO_DEV_BLK, &dev) != 0)
        return -1;
    if (vio_init(&dev) != 0)
        return -2;
    if (vio_queue_init(&dev, 0, &q, QSIZE) != 0)
        return -3;
    vio_driver_ok(&dev);

    capacity = vio_cfg64(&dev, 0x00);
    sec_size = vio_cfg32(&dev, 0x14);
    if (sec_size != 512)
        return -4;

    hdr_phys  = pmm_alloc(0);
    data_phys = pmm_alloc(0);
    if (!hdr_phys || !data_phys)
        return -5;
    hdr_va  = (volatile uint8_t *)pmm_to_virt(hdr_phys);
    data_va = (volatile uint8_t *)pmm_to_virt(data_phys);

    ready = 1;
    kprint("virtio-blk: %lu MiB, %u-byte sectors (%s)\n",
           capacity * sec_size / (1024 * 1024), sec_size,
           dev.legacy ? "legacy" : "modern");
    return 0;
}

uint64_t blk_capacity(void)    { return capacity; }
uint32_t blk_sector_size(void) { return sec_size; }

/* one descriptor chain: hdr -> data -> status; dlen = sector multiple */
static int blk_rw(uint32_t type, uint64_t lba, uint32_t dlen)
{
    volatile struct blk_hdr *h = (volatile struct blk_hdr *)hdr_va;
    h->type = type;
    h->reserved = 0;
    h->sector = lba;
    volatile uint8_t *status = hdr_va + 16;
    *status = 0xff;

    q.desc[0].addr  = hdr_phys;
    q.desc[0].len   = 16;
    q.desc[0].flags = VQ_DESC_NEXT;
    q.desc[0].next  = 1;
    q.desc[1].addr  = data_phys;
    q.desc[1].len   = dlen;
    q.desc[1].flags = VQ_DESC_NEXT | (type == BLK_T_IN ? VQ_DESC_WRITE : 0);
    q.desc[1].next  = 2;
    q.desc[2].addr  = hdr_phys + 16;
    q.desc[2].len   = 1;
    q.desc[2].flags = VQ_DESC_WRITE;
    q.desc[2].next  = 0;
    vio_push(&q, 0);

    for (uint64_t t = 0; t < TIMEOUT; t++) {
        if (q.used->idx != q.last_used) {
            __asm__ volatile("dmb sy" ::: "memory");
            uint32_t id = q.used->ring[q.last_used % q.qsize].id;
            q.last_used++;
            vio_isr(&dev);
            if (id != 0)
                return -2;
            return *status == 0 ? 0 : -3;
        }
    }
    return -1;
}

int blk_read(uint64_t lba, void *buf, uint32_t nbytes)
{
    if (!ready || nbytes == 0 || lba >= capacity)
        return -1;
    spin_lock(&lock);
    uint8_t *p = buf;
    int rc = 0;
    while (nbytes) {
        uint32_t chunk = nbytes > DATA_MAX ? DATA_MAX : nbytes;
        uint32_t dlen  = (chunk + 511) & ~511u;
        if ((rc = blk_rw(BLK_T_IN, lba, dlen)) != 0)
            break;
        memcpy(p, (const void *)data_va, chunk);
        p += chunk;
        nbytes -= chunk;
        lba += dlen / 512;
    }
    spin_unlock(&lock);
    return rc;
}

int blk_write(uint64_t lba, const void *buf, uint32_t nbytes)
{
    if (!ready || nbytes == 0 || (nbytes & 511) || lba >= capacity)
        return -1;
    spin_lock(&lock);
    const uint8_t *p = buf;
    int rc = 0;
    while (nbytes) {
        uint32_t chunk = nbytes > DATA_MAX ? DATA_MAX : nbytes;
        memcpy((void *)data_va, p, chunk);
        if ((rc = blk_rw(BLK_T_OUT, lba, chunk)) != 0)
            break;
        p += chunk;
        nbytes -= chunk;
        lba += chunk / 512;
    }
    spin_unlock(&lock);
    return rc;
}

void blk_selftest(void)
{
    uint8_t sec[512];
    if (blk_read(0, sec, 512) != 0) {
        kprint("virtio-blk: selftest read failed\n");
        return;
    }
    kprint("virtio-blk: sector 0 sig %x%x, bytes %x %x %x %x\n",
           sec[510], sec[511], sec[0], sec[1], sec[2], sec[3]);
}
