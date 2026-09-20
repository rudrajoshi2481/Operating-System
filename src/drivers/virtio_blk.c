/*
 * virtio_blk.c — virtio-mmio (v2) block device driver.
 *
 * QEMU 'virt' exposes virtio-mmio transports at 0x0a000000 + i*0x200
 * (31 slots); '-device virtio-blk-device' occupies slot 0. Split
 * virtqueue in a single PMM page; requests are synchronous — one
 * outstanding descriptor chain at a time, completion polled.
 */
#include "virtio_blk.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "spinlock.h"

/* ---- virtio-mmio v2 registers -------------------------------------- */
#define MMIO_BASE      0x0a000000ULL
#define MMIO_STRIDE    0x200ULL
#define MMIO_SLOTS     32               /* virt wires devices top-down */

#define R_MAGIC        0x000            /* 'virt' = 0x74726976 */
#define R_VERSION      0x004            /* 2 = modern interface */
#define R_DEVID        0x008            /* 2 = block device */
#define R_DFEAT        0x010
#define R_DFEATSEL     0x014
#define R_GFEAT        0x020
#define R_GFEATSEL     0x024
#define R_GPAGESIZE    0x028            /* legacy only */
#define R_QSEL         0x030
#define R_QNUMMAX      0x034
#define R_QNUM         0x038
#define R_QALIGN       0x03c            /* legacy only */
#define R_QPFN         0x040            /* legacy only */
#define R_QREADY       0x044            /* modern only */
#define R_QNOTIFY      0x050
#define R_ISTATUS      0x060
#define R_IACK         0x064
#define R_STATUS       0x070
#define R_QDESCLO      0x080
#define R_QDESCHI      0x084
#define R_QAVAILLO     0x090
#define R_QAVAILHI     0x094
#define R_QUSEDLO      0x0a0
#define R_QUSEDHI      0x0a4
#define R_CONFIG       0x100

/* status bits */
#define S_ACK          1
#define S_DRIVER       2
#define S_DRIVER_OK    4
#define S_FEAT_OK      8
#define S_FAILED       0x80

/* virtio-blk request types */
#define BLK_T_IN       0                /* read device -> driver */
#define BLK_T_OUT      1                /* write driver -> device */

#define QSIZE          128
#define DESC_SZ        (QSIZE * 16)
#define AVAIL_OFF      DESC_SZ                              /* 2048 */
#define USED_OFF       2560             /* 4-aligned, fits page (3590B) */

#define DESC_NEXT      1
#define DESC_WRITE     2                /* device writes this buf */

#define TIMEOUT        0x1000000

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
};

struct vq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct vq_used_elem ring[QSIZE];
};

struct blk_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static volatile uint8_t  *base;
static struct vq_desc    *desc;
static volatile struct vq_avail *avail;
static volatile struct vq_used  *used;
static uint16_t          last_used;
static uint64_t          capacity;      /* sectors */
static uint32_t          sec_size;      /* bytes */
static uint64_t          hdr_phys, data_phys;
static volatile uint8_t  *hdr_va, *data_va;
static spinlock_t        lock;
static int               ready, legacy;

#define DATA_MAX 4096                   /* one page of payload per req */

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static inline void wmb(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

static uint64_t slot_probe(uint64_t hhdm)
{
    for (int i = 0; i < MMIO_SLOTS; i++) {
        volatile uint8_t *r = (volatile uint8_t *)(hhdm + MMIO_BASE +
                                                   (uint64_t)i * MMIO_STRIDE);
        if (*(volatile uint32_t *)(r + R_MAGIC) == 0x74726976 &&
            *(volatile uint32_t *)(r + R_DEVID) == 2)
            return (uint64_t)r;
    }
    return 0;
}

/*
 * Modern (v2): queue regions given as three explicit addresses.
 * Legacy (v1): one contiguous block — desc, avail, then used on a
 * page boundary — handed to the device as a PFN at GuestPageSize.
 */
static int queue_setup(void)
{
    uint64_t qphys;
    uint8_t *qva;

    wr(R_QSEL, 0);
    if (rd(R_QNUMMAX) < QSIZE)
        return -2;

    if (legacy) {
        qphys = pmm_alloc(1);           /* 8 KiB: rings + used@+4096 */
        if (!qphys)
            return -1;
        qva = (uint8_t *)pmm_to_virt(qphys);
        memset(qva, 0, 8192);
        desc  = (struct vq_desc *)qva;
        avail = (volatile struct vq_avail *)(qva + AVAIL_OFF);
        used  = (volatile struct vq_used *)(qva + 4096);
        wr(R_GPAGESIZE, 4096);
        wr(R_QNUM, QSIZE);
        wr(R_QALIGN, 4096);
        wr(R_QPFN, (uint32_t)(qphys >> 12));
        return 0;
    }

    qphys = pmm_alloc(0);
    if (!qphys)
        return -1;
    qva = (uint8_t *)pmm_to_virt(qphys);
    memset(qva, 0, 4096);

    desc  = (struct vq_desc *)qva;
    avail = (volatile struct vq_avail *)(qva + AVAIL_OFF);
    used  = (volatile struct vq_used *)(qva + USED_OFF);

    wr(R_QNUM, QSIZE);
    wr(R_QDESCLO, (uint32_t)qphys);
    wr(R_QDESCHI, (uint32_t)(qphys >> 32));
    wr(R_QAVAILLO, (uint32_t)(qphys + AVAIL_OFF));
    wr(R_QAVAILHI, (uint32_t)((qphys + AVAIL_OFF) >> 32));
    wr(R_QUSEDLO, (uint32_t)(qphys + USED_OFF));
    wr(R_QUSEDHI, (uint32_t)((qphys + USED_OFF) >> 32));
    wr(R_QREADY, 1);
    return 0;
}

int blk_init(uint64_t hhdm)
{
    uint64_t b = slot_probe(hhdm);
    if (!b)
        return -1;
    base = (volatile uint8_t *)b;
    uint32_t ver = rd(R_VERSION);
    if (ver != 1 && ver != 2)
        return -2;
    legacy = ver == 1;

    wr(R_STATUS, 0);                    /* reset */
    wmb();
    wr(R_STATUS, S_ACK);
    wr(R_STATUS, S_ACK | S_DRIVER);

    /* modern requires VIRTIO_F_VERSION_1 (bit 32: sel 1, bit 0) */
    if (legacy) {
        wr(R_GFEATSEL, 0);
        wr(R_GFEAT, 0);
    } else {
        wr(R_GFEATSEL, 0);
        wr(R_GFEAT, 0);
        wr(R_GFEATSEL, 1);
        wr(R_GFEAT, 1);
        wr(R_STATUS, S_ACK | S_DRIVER | S_FEAT_OK);
        if (!(rd(R_STATUS) & S_FEAT_OK))
            return -3;
    }

    if (queue_setup() != 0)
        return -4;

    wr(R_STATUS, S_ACK | S_DRIVER |
         (legacy ? 0 : S_FEAT_OK) | S_DRIVER_OK);
    wmb();

    capacity  = *(volatile uint64_t *)(base + R_CONFIG);
    sec_size  = *(volatile uint32_t *)(base + R_CONFIG + 0x14);
    if (sec_size != 512)
        return -5;

    hdr_phys  = pmm_alloc(0);
    data_phys = pmm_alloc(0);
    if (!hdr_phys || !data_phys)
        return -6;
    hdr_va  = (volatile uint8_t *)pmm_to_virt(hdr_phys);
    data_va = (volatile uint8_t *)pmm_to_virt(data_phys);

    ready = 1;
    kprint("virtio-blk: %lu MiB, %u-byte sectors (%s)\n",
           capacity * sec_size / (1024 * 1024), sec_size,
           legacy ? "legacy" : "modern");
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

    desc[0].addr  = hdr_phys;
    desc[0].len   = 16;
    desc[0].flags = DESC_NEXT;
    desc[0].next  = 1;
    desc[1].addr  = data_phys;
    desc[1].len   = dlen;
    desc[1].flags = DESC_NEXT | (type == BLK_T_IN ? DESC_WRITE : 0);
    desc[1].next  = 2;
    desc[2].addr  = hdr_phys + 16;
    desc[2].len   = 1;
    desc[2].flags = DESC_WRITE;
    desc[2].next  = 0;
    wmb();

    avail->ring[avail->idx % QSIZE] = 0;
    wmb();
    avail->idx++;
    wmb();
    wr(R_QNOTIFY, 0);

    for (uint64_t t = 0; t < TIMEOUT; t++) {
        if (used->idx != last_used) {
            wmb();
            uint32_t id = used->ring[last_used % QSIZE].id;
            last_used++;
            wr(R_IACK, rd(R_ISTATUS));
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
