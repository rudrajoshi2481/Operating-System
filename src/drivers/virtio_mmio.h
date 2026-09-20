#pragma once

/*
 * virtio_mmio.h — shared virtio-mmio transport (v1 legacy + v2 modern).
 *
 * QEMU 'virt' exposes 32 transports at 0x0a000000 + i*0x200, wired
 * top-down; transport i signals SPI 16+i (INTID 48+i). Devices get
 * a struct vdev from vio_probe(), then vio_init() runs the status
 * handshake, and each queue gets a page-pair via vio_queue_init().
 */
#include <stdint.h>

#define VIO_MMIO_BASE   0x0a000000ULL
#define VIO_MMIO_STRIDE 0x200ULL
#define VIO_MMIO_SLOTS  32

#define VIO_IRQ(n)      (48 + (n))      /* slot n -> INTID 48+n */

/* device IDs */
#define VIO_DEV_BLK     2
#define VIO_DEV_CONSOLE 3

#define VIRTQ_MAX       128

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

#define VQ_DESC_NEXT   1
#define VQ_DESC_WRITE  2                 /* device writes this buffer */

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX];
};

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[VIRTQ_MAX];
};

struct vdev {
    volatile uint8_t *base;
    int               legacy;
    int               irq;              /* GIC INTID */
    uint32_t          devid;
};

struct virtqueue {
    struct vq_desc           *desc;
    volatile struct vq_avail *avail;
    volatile struct vq_used  *used;
    uint16_t                  last_used;
    uint16_t                  qsize;
    uint16_t                  idx;      /* queue number on the device */
    uint64_t                  phys;
    struct vdev              *dev;
};

/* Probe slots for the `instance`-th device of type devid (0 = first,
 * scanning slots low to high). Fills dev (incl. irq). 0 = found. */
int  vio_probe(uint64_t hhdm, uint32_t devid, int instance,
               struct vdev *dev);

/* Reset + ACK/DRIVER + feature negotiation (VERSION_1 when modern). */
int  vio_init(struct vdev *dev);

/* Allocate + hand queue idx (qsize <= VIRTQ_MAX) to the device. */
int  vio_queue_init(struct vdev *dev, int idx, struct virtqueue *q,
                    uint16_t qsize);

/* Final DRIVER_OK; device is live after this. */
void vio_driver_ok(struct vdev *dev);

/* Read + ack interrupt status bits. */
uint32_t vio_isr(struct vdev *dev);

/* Device config space (offset 0 = first config field). */
uint64_t vio_cfg64(struct vdev *dev, uint32_t off);
uint32_t vio_cfg32(struct vdev *dev, uint32_t off);
uint16_t vio_cfg16(struct vdev *dev, uint32_t off);

/* Ring helpers — caller publishes descs, then: */
static inline void vio_notify(struct virtqueue *q)
{
    *(volatile uint32_t *)(q->dev->base + 0x50) = q->idx;
}

static inline void vio_push(struct virtqueue *q, uint16_t head)
{
    __asm__ volatile("dmb sy" ::: "memory");
    q->avail->ring[q->avail->idx % q->qsize] = head;
    __asm__ volatile("dmb sy" ::: "memory");
    q->avail->idx++;
    __asm__ volatile("dmb sy" ::: "memory");
    vio_notify(q);
}
