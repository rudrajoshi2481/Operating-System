/* virtio_mmio.c — shared virtio-mmio transport init (v1 + v2). */
#include "virtio_mmio.h"
#include "lib.h"
#include "pmm.h"

#define R_MAGIC      0x000              /* 'virt' = 0x74726976 */
#define R_VERSION    0x004              /* 1 legacy, 2 modern */
#define R_DEVID      0x008
#define R_DFEATSEL   0x014
#define R_GFEAT      0x020
#define R_GFEATSEL   0x024
#define R_GPAGESIZE  0x028              /* legacy only */
#define R_QSEL       0x030
#define R_QNUMMAX    0x034
#define R_QNUM       0x038
#define R_QALIGN     0x03c              /* legacy only */
#define R_QPFN       0x040              /* legacy only */
#define R_QREADY     0x044              /* modern only */
#define R_ISTATUS    0x060
#define R_IACK       0x064
#define R_STATUS     0x070
#define R_QDESCLO    0x080              /* modern only */
#define R_QDESCHI    0x084
#define R_QAVAILLO   0x090
#define R_QAVAILHI   0x094
#define R_QUSEDLO    0x0a0
#define R_QUSEDHI    0x0a4
#define R_CONFIG     0x100

#define S_ACK        1
#define S_DRIVER     2
#define S_DRIVER_OK  4
#define S_FEAT_OK    8

#define USED_OFF     4096               /* used ring always page-aligned */
#define Q_PAGES      8                  /* order-1: 8 KiB per queue */

static inline uint32_t rd(struct vdev *d, uint32_t off)
{
    return *(volatile uint32_t *)(d->base + off);
}

static inline void wr(struct vdev *d, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(d->base + off) = v;
}

int vio_probe(uint64_t hhdm, uint32_t devid, struct vdev *dev)
{
    for (int i = 0; i < VIO_MMIO_SLOTS; i++) {
        volatile uint8_t *r = (volatile uint8_t *)(hhdm + VIO_MMIO_BASE +
                                                   (uint64_t)i * VIO_MMIO_STRIDE);
        if (*(volatile uint32_t *)(r + R_MAGIC) == 0x74726976 &&
            *(volatile uint32_t *)(r + R_DEVID) == devid) {
            uint32_t ver = *(volatile uint32_t *)(r + R_VERSION);
            if (ver != 1 && ver != 2)
                continue;
            dev->base   = r;
            dev->legacy = ver == 1;
            dev->irq    = VIO_IRQ(i);
            dev->devid  = devid;
            return 0;
        }
    }
    return -1;
}

int vio_init(struct vdev *dev)
{
    wr(dev, R_STATUS, 0);               /* reset */
    __asm__ volatile("dmb sy" ::: "memory");
    wr(dev, R_STATUS, S_ACK);
    wr(dev, R_STATUS, S_ACK | S_DRIVER);

    wr(dev, R_GFEATSEL, 0);
    wr(dev, R_GFEAT, 0);                /* accept no low features */
    if (!dev->legacy) {
        wr(dev, R_GFEATSEL, 1);         /* VIRTIO_F_VERSION_1 */
        wr(dev, R_GFEAT, 1);
        wr(dev, R_STATUS, S_ACK | S_DRIVER | S_FEAT_OK);
        if (!(rd(dev, R_STATUS) & S_FEAT_OK))
            return -3;
    }
    return 0;
}

int vio_queue_init(struct vdev *dev, int idx, struct virtqueue *q,
                   uint16_t qsize)
{
    uint64_t qphys = pmm_alloc(1);      /* order-1: 8 KiB, page-aligned */
    if (!qphys)
        return -1;
    uint8_t *qva = (uint8_t *)pmm_to_virt(qphys);
    memset(qva, 0, Q_PAGES * 1024);

    if (16 * (uint32_t)qsize + 6 + 2 * (uint32_t)qsize > USED_OFF)
        return -2;                      /* desc+avail must stay under 4 KiB */

    q->desc      = (struct vq_desc *)qva;
    q->avail     = (volatile struct vq_avail *)(qva + 16 * qsize);
    q->used      = (volatile struct vq_used *)(qva + USED_OFF);
    q->last_used = 0;
    q->qsize     = qsize;
    q->idx       = (uint16_t)idx;
    q->phys      = qphys;
    q->dev       = dev;

    wr(dev, R_QSEL, idx);
    if (rd(dev, R_QNUMMAX) < qsize)
        return -3;
    wr(dev, R_QNUM, qsize);

    if (dev->legacy) {
        wr(dev, R_GPAGESIZE, 4096);
        wr(dev, R_QALIGN, 4096);
        wr(dev, R_QPFN, (uint32_t)(qphys >> 12));
    } else {
        wr(dev, R_QDESCLO, (uint32_t)qphys);
        wr(dev, R_QDESCHI, (uint32_t)(qphys >> 32));
        wr(dev, R_QAVAILLO, (uint32_t)(qphys + 16 * qsize));
        wr(dev, R_QAVAILHI, (uint32_t)((qphys + 16 * qsize) >> 32));
        wr(dev, R_QUSEDLO, (uint32_t)(qphys + USED_OFF));
        wr(dev, R_QUSEDHI, (uint32_t)((qphys + USED_OFF) >> 32));
        wr(dev, R_QREADY, 1);
    }
    return 0;
}

void vio_driver_ok(struct vdev *dev)
{
    wr(dev, R_STATUS,
         S_ACK | S_DRIVER | (dev->legacy ? 0 : S_FEAT_OK) | S_DRIVER_OK);
    __asm__ volatile("dmb sy" ::: "memory");
}

uint32_t vio_isr(struct vdev *dev)
{
    uint32_t s = rd(dev, R_ISTATUS);
    wr(dev, R_IACK, s);
    return s;
}

uint64_t vio_cfg64(struct vdev *dev, uint32_t off)
{
    return *(volatile uint64_t *)(dev->base + R_CONFIG + off);
}

uint32_t vio_cfg32(struct vdev *dev, uint32_t off)
{
    return *(volatile uint32_t *)(dev->base + R_CONFIG + off);
}

uint16_t vio_cfg16(struct vdev *dev, uint32_t off)
{
    return *(volatile uint16_t *)(dev->base + R_CONFIG + off);
}
