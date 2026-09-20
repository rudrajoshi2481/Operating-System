/*
 * virtio_blk.c — virtio-blk on the shared virtio-mmio transport.
 * Synchronous requests: one descriptor chain (hdr -> data -> status)
 * at a time, completion polled on the used ring.
 */
#include "virtio_blk.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"

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

static struct blkdev disks[BLK_MAX_DEVS];
static struct blkdev *esp, *store;
static int            ndisks;

static int blk_init_dev(uint64_t hhdm, int instance, struct blkdev *d)
{
    memset(d, 0, sizeof(*d));
    if (vio_probe(hhdm, VIO_DEV_BLK, instance, &d->dev) != 0)
        return -1;
    if (vio_init(&d->dev) != 0)
        return -2;
    if (vio_queue_init(&d->dev, 0, &d->q, QSIZE) != 0)
        return -3;
    vio_driver_ok(&d->dev);

    d->capacity = vio_cfg64(&d->dev, 0x00);
    d->sec_size = vio_cfg32(&d->dev, 0x14);
    if (d->sec_size != 512)
        return -4;

    d->hdr_phys  = pmm_alloc(0);
    d->data_phys = pmm_alloc(0);
    if (!d->hdr_phys || !d->data_phys)
        return -5;
    d->hdr_va  = (volatile uint8_t *)pmm_to_virt(d->hdr_phys);
    d->data_va = (volatile uint8_t *)pmm_to_virt(d->data_phys);

    d->ready = 1;
    kprint("virtio-blk[%d]: %lu MiB, %u-byte sectors (%s)\n", instance,
           d->capacity * d->sec_size / (1024 * 1024), d->sec_size,
           d->dev.legacy ? "legacy" : "modern");
    return 0;
}

int blk_init(uint64_t hhdm)
{
    for (int i = 0; i < BLK_MAX_DEVS; i++) {
        if (blk_init_dev(hhdm, i, &disks[i]) != 0)
            break;
        uint8_t sec[512];
        if (blk_read(&disks[i], 0, sec, 512) == 0 &&
            sec[510] == 0x55 && sec[511] == 0xaa)
            esp = &disks[i];
        else
            store = &disks[i];
        ndisks++;
    }
    return ndisks;
}

struct blkdev *blk_esp(void)   { return esp; }
struct blkdev *blk_store(void) { return store; }

/* one descriptor chain: hdr -> data -> status; dlen = sector multiple */
static int blk_rw(struct blkdev *d, uint32_t type, uint64_t lba,
                  uint32_t dlen)
{
    volatile struct blk_hdr *h = (volatile struct blk_hdr *)d->hdr_va;
    h->type = type;
    h->reserved = 0;
    h->sector = lba;
    volatile uint8_t *status = d->hdr_va + 16;
    *status = 0xff;

    d->q.desc[0].addr  = d->hdr_phys;
    d->q.desc[0].len   = 16;
    d->q.desc[0].flags = VQ_DESC_NEXT;
    d->q.desc[0].next  = 1;
    d->q.desc[1].addr  = d->data_phys;
    d->q.desc[1].len   = dlen;
    d->q.desc[1].flags = VQ_DESC_NEXT |
                         (type == BLK_T_IN ? VQ_DESC_WRITE : 0);
    d->q.desc[1].next  = 2;
    d->q.desc[2].addr  = d->hdr_phys + 16;
    d->q.desc[2].len   = 1;
    d->q.desc[2].flags = VQ_DESC_WRITE;
    d->q.desc[2].next  = 0;
    vio_push(&d->q, 0);

    for (uint64_t t = 0; t < TIMEOUT; t++) {
        if (d->q.used->idx != d->q.last_used) {
            __asm__ volatile("dmb sy" ::: "memory");
            uint32_t id = d->q.used->ring[d->q.last_used % QSIZE].id;
            d->q.last_used++;
            vio_isr(&d->dev);
            if (id != 0)
                return -2;
            return *status == 0 ? 0 : -3;
        }
    }
    return -1;
}

int blk_read(struct blkdev *d, uint64_t lba, void *buf, uint32_t nbytes)
{
    if (!d->ready || nbytes == 0 || lba >= d->capacity)
        return -1;
    spin_lock(&d->lock);
    uint8_t *p = buf;
    int rc = 0;
    while (nbytes) {
        uint32_t chunk = nbytes > DATA_MAX ? DATA_MAX : nbytes;
        uint32_t dlen  = (chunk + 511) & ~511u;
        if ((rc = blk_rw(d, BLK_T_IN, lba, dlen)) != 0)
            break;
        memcpy(p, (const void *)d->data_va, chunk);
        p += chunk;
        nbytes -= chunk;
        lba += dlen / 512;
    }
    spin_unlock(&d->lock);
    return rc;
}

int blk_write(struct blkdev *d, uint64_t lba, const void *buf,
              uint32_t nbytes)
{
    if (!d->ready || nbytes == 0 || (nbytes & 511) || lba >= d->capacity)
        return -1;
    spin_lock(&d->lock);
    const uint8_t *p = buf;
    int rc = 0;
    while (nbytes) {
        uint32_t chunk = nbytes > DATA_MAX ? DATA_MAX : nbytes;
        memcpy((void *)d->data_va, p, chunk);
        if ((rc = blk_rw(d, BLK_T_OUT, lba, chunk)) != 0)
            break;
        p += chunk;
        nbytes -= chunk;
        lba += chunk / 512;
    }
    spin_unlock(&d->lock);
    return rc;
}

void blk_selftest(struct blkdev *d)
{
    uint8_t sec[512];
    if (blk_read(d, 0, sec, 512) != 0) {
        kprint("virtio-blk: selftest read failed\n");
        return;
    }
    kprint("virtio-blk: sector 0 sig %x%x, bytes %x %x %x %x\n",
           sec[510], sec[511], sec[0], sec[1], sec[2], sec[3]);
}
