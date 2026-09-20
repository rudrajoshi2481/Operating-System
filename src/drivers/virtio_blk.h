#pragma once

#include <stdint.h>
#include "virtio_mmio.h"
#include "spinlock.h"

/* One initialized virtio-blk device. Our QEMU config attaches two:
 * the FAT32 ESP (boot image) and a raw disk for the object store. */
struct blkdev {
    struct vdev      dev;
    struct virtqueue q;
    uint64_t         hdr_phys, data_phys;
    volatile uint8_t *hdr_va, *data_va;
    uint64_t         capacity;          /* sectors */
    uint32_t         sec_size;          /* bytes */
    spinlock_t       lock;
    int              ready;
};

#define BLK_MAX_DEVS 4

/* Probe+init all blk devices, classify: sector-0 0x55AA sig => ESP,
 * otherwise => object-store disk. Returns count initialized. */
int             blk_init(uint64_t hhdm);
struct blkdev  *blk_esp(void);          /* FAT boot disk or NULL */
struct blkdev  *blk_store(void);        /* raw store disk or NULL */

int      blk_read(struct blkdev *d, uint64_t lba, void *buf,
                  uint32_t nbytes);
int      blk_write(struct blkdev *d, uint64_t lba, const void *buf,
                   uint32_t nbytes);
void     blk_selftest(struct blkdev *d);
