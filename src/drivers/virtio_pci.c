/*
 * virtio_pci.c — legacy virtio-pci transport (x86_64, Step 16).
 *
 * QEMU 'pc' wires -device virtio-*-pci as transitional devices:
 * vendor 0x1af4, device id 0x0fff + virtio devid (blk 0x1001,
 * console 0x1003). The legacy register map lives in BAR0 I/O space:
 *   +0x00 dev features    +0x04 guest features   +0x08 queue PFN
 *   +0x0c queue num max   +0x0e queue select     +0x10 queue notify
 *   +0x12 status          +0x13 ISR              +0x14 device config
 *
 * Ring layout is the same contiguous legacy split ring as mmio v1,
 * so the vio_* surface is identical — drivers are transport-agnostic.
 */
#include "virtio_mmio.h"
#include "io.h"
#include "lib.h"
#include "pmm.h"

#define PCI_ADDR 0xcf8
#define PCI_DATA 0xcfc

#define R_DFEAT    0x00
#define R_GFEAT    0x04
#define R_QPFN     0x08
#define R_QNUMMAX  0x0c
#define R_QSEL     0x0e
#define R_QNOTIFY  0x10
#define R_STATUS   0x12
#define R_ISR      0x13
#define R_CONFIG   0x14

#define S_ACK       1
#define S_DRIVER    2
#define S_DRIVER_OK 4

/* legacy pci: the queue size is device-fixed (QueueNum), and the used
 * ring must start on the next page boundary after desc+avail. */

static uint32_t pci_rd32(uint32_t bus, uint32_t dev, uint32_t fn,
                         uint32_t off)
{
    outl(PCI_ADDR, 0x80000000u | (bus << 16) | (dev << 11) |
                   (fn << 8) | (off & 0xfc));
    return inl(PCI_DATA);
}

static void pci_wr32(uint32_t bus, uint32_t dev, uint32_t fn,
                     uint32_t off, uint32_t v)
{
    outl(PCI_ADDR, 0x80000000u | (bus << 16) | (dev << 11) |
                   (fn << 8) | (off & 0xfc));
    outl(PCI_DATA, v);
}

/* Transitional PCI device IDs are 0x1000+devid for most types, but
 * console is 0x1003 and balloon 0x1002 — QEMU's ids predate the rule. */
static uint32_t pci_id_for(uint32_t devid)
{
    switch (devid) {
    case 1:  return 0x1000;             /* net */
    case 2:  return 0x1001;             /* blk */
    case 3:  return 0x1003;             /* console/serial */
    case 5:  return 0x1005;             /* rng */
    case 9:  return 0x1009;             /* 9p */
    default: return 0x1000 + devid;
    }
}

int vio_probe(uint64_t hhdm, uint32_t devid, int instance,
              struct vdev *dev)
{
    (void)hhdm;
    uint32_t want = pci_id_for(devid);
    for (uint32_t bus = 0; bus < 256; bus++)
        for (uint32_t d = 0; d < 32; d++)
            for (uint32_t f = 0; f < 8; f++) {
                uint32_t id = pci_rd32(bus, d, f, 0);
                if ((id & 0xffff) != 0x1af4 ||
                    (id >> 16) != want)
                    continue;
                if (instance--)
                    continue;
                uint32_t bar0 = pci_rd32(bus, d, f, 0x10);
                if (!(bar0 & 1))        /* need I/O space BAR */
                    return -2;
                /* PCI command: I/O space + memory + bus master (DMA) */
                pci_wr32(bus, d, f, 0x04,
                         pci_rd32(bus, d, f, 0x04) | 0x7);
                dev->iobase  = (uint16_t)(bar0 & ~3u);
                dev->base    = 0;
                dev->legacy  = 1;
                dev->irq     = -1;      /* no IOAPIC setup; polled */
                dev->devid   = devid;
                return 0;
            }
    return -1;
}

int vio_init(struct vdev *dev)
{
    uint16_t io = dev->iobase;
    outb(io + R_STATUS, 0);             /* reset */
    outb(io + R_STATUS, S_ACK);
    outb(io + R_STATUS, S_ACK | S_DRIVER);
    outl(io + R_GFEAT, 0);              /* accept no features */
    return 0;
}

int vio_queue_init(struct vdev *dev, int idx, struct virtqueue *q,
                   uint16_t qsize)
{
    (void)qsize;
    uint16_t io = dev->iobase;
    outw(io + R_QSEL, (uint16_t)idx);
    uint16_t qn = inw(io + R_QNUMMAX);
    if (!qn || qn > VIRTQ_MAX)
        return -3;
    qsize = qn;

    uint64_t used_off = (16ull * qsize + 6 + 2ull * qsize + 4095) & ~4095ull;
    uint64_t total = used_off + 6 + 8ull * qsize;
    unsigned order = 0;
    while ((4096ull << order) < total)
        order++;

    uint64_t qphys = pmm_alloc(order);
    if (!qphys)
        return -1;
    uint8_t *qva = (uint8_t *)pmm_to_virt(qphys);
    memset(qva, 0, (size_t)(4096ull << order));

    q->desc      = (struct vq_desc *)qva;
    q->avail     = (volatile struct vq_avail *)(qva + 16 * qsize);
    q->used      = (volatile struct vq_used *)(qva + used_off);
    q->last_used = 0;
    q->qsize     = qsize;
    q->idx       = (uint16_t)idx;
    q->phys      = qphys;
    q->dev       = dev;

    outl(io + R_QPFN, (uint32_t)(qphys >> 12));
    return 0;
}

void vio_driver_ok(struct vdev *dev)
{
    outb(dev->iobase + R_STATUS, S_ACK | S_DRIVER | S_DRIVER_OK);
}

uint32_t vio_isr(struct vdev *dev)
{
    return inb(dev->iobase + R_ISR);    /* read clears */
}

void vio_notify(struct virtqueue *q)
{
    outw(q->dev->iobase + R_QNOTIFY, q->idx);
}

uint64_t vio_cfg64(struct vdev *dev, uint32_t off)
{
    uint32_t lo = inl(dev->iobase + R_CONFIG + off);
    uint32_t hi = inl(dev->iobase + R_CONFIG + off + 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

uint32_t vio_cfg32(struct vdev *dev, uint32_t off)
{
    return inl(dev->iobase + R_CONFIG + off);
}

uint16_t vio_cfg16(struct vdev *dev, uint32_t off)
{
    return inw(dev->iobase + R_CONFIG + off);
}
