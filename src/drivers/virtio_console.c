/*
 * virtio_console.c — virtio-console port 0 (virtconsole) driver.
 *
 * Queue map (port i -> rxq 2i, txq 2i+1): port 0 uses queues 0/1.
 * Unlike blk there is no request header — raw byte buffers, and the
 * guest must pre-post empty WRITE descriptors on the rx queue for the
 * device to fill. RX completion is IRQ-driven; TX polls the used ring.
 */
#include "virtio_console.h"
#include "virtio_mmio.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "spinlock.h"
#include "thread.h"
#include "irq.h"
#include "gic.h"

#define QSIZE       32
#define RX_BUFS     8
#define RX_BUFLEN   512                 /* 8 x 512 = one 4 KiB page */
#define TX_BUFLEN   4096                /* one page per write chunk */
#define TIMEOUT     0x1000000
#define RING_SIZE   2048

static struct vdev      dev;
static struct virtqueue rxq, txq;
static uint64_t         rx_phys, tx_phys;
static volatile uint8_t *rx_va, *tx_va;
static spinlock_t       tx_lock;
static int              ready;

/* ISR -> reader thread ring buffer */
static uint8_t          rx_ring[RING_SIZE];
static volatile uint32_t rx_head, rx_tail;   /* head=write, tail=read */
static void            *rx_chan = &rx_ring;  /* sleep_on channel */
static volatile uint32_t rx_dropped;

/* drain used ring: copy payload into rx_ring, repost the descriptor */
static void rx_drain(void)
{
    while (rxq.used->idx != rxq.last_used) {
        __asm__ volatile("dmb sy" ::: "memory");
        struct vq_used_elem e = rxq.used->ring[rxq.last_used % rxq.qsize];
        rxq.last_used++;
        uint16_t id = (uint16_t)e.id;
        if (id >= RX_BUFS)
            continue;
        const volatile uint8_t *src = rx_va + id * RX_BUFLEN;
        for (uint32_t i = 0; i < e.len; i++) {
            uint32_t n = (rx_head + 1) % RING_SIZE;
            if (n == rx_tail) {
                rx_dropped++;
                break;
            }
            rx_ring[rx_head] = src[i];
            rx_head = n;
        }
        __asm__ volatile("dmb sy" ::: "memory");
        rxq.avail->ring[rxq.avail->idx % rxq.qsize] = id;
        rxq.avail->idx++;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    vio_notify(&rxq);
}

static void con_irq(struct trap_frame *f)
{
    (void)f;
    (void)vio_isr(&dev);                /* read+ack interrupt status */
    rx_drain();
    /* consume tx completions so the used ring doesn't stall */
    while (txq.used->idx != txq.last_used)
        txq.last_used++;
    wakeup(rx_chan);
}

int con_init(uint64_t hhdm)
{
    if (vio_probe(hhdm, VIO_DEV_CONSOLE, 0, &dev) != 0)
        return -1;
    if (vio_init(&dev) != 0)
        return -2;
    if (vio_queue_init(&dev, 0, &rxq, QSIZE) != 0 ||
        vio_queue_init(&dev, 1, &txq, QSIZE) != 0)
        return -3;
    vio_driver_ok(&dev);

    rx_phys = pmm_alloc(0);
    tx_phys = pmm_alloc(0);
    if (!rx_phys || !tx_phys)
        return -4;
    rx_va = (volatile uint8_t *)pmm_to_virt(rx_phys);
    tx_va = (volatile uint8_t *)pmm_to_virt(tx_phys);

    /* post all rx buffers so the device can write immediately */
    for (int i = 0; i < RX_BUFS; i++) {
        rxq.desc[i].addr  = rx_phys + (uint64_t)i * RX_BUFLEN;
        rxq.desc[i].len   = RX_BUFLEN;
        rxq.desc[i].flags = VQ_DESC_WRITE;
        rxq.desc[i].next  = 0;
        rxq.avail->ring[i] = (uint16_t)i;
    }
    __asm__ volatile("dmb sy" ::: "memory");
    rxq.avail->idx = RX_BUFS;
    __asm__ volatile("dmb sy" ::: "memory");
    vio_notify(&rxq);

    irq_register(dev.irq, con_irq);
    gic_enable(dev.irq, 0x90);

    ready = 1;
    kprint("virtio-console: host channel up, irq %d (%s)\n",
           dev.irq, dev.legacy ? "legacy" : "modern");
    return 0;
}

int con_write(const void *buf, uint32_t len)
{
    if (!ready || !len)
        return -1;
    spin_lock(&tx_lock);
    const uint8_t *p = buf;
    int rc = 0;
    while (len) {
        uint32_t chunk = len > TX_BUFLEN ? TX_BUFLEN : len;
        memcpy((void *)tx_va, p, chunk);
        __asm__ volatile("dmb sy" ::: "memory");

        uint16_t expect = txq.last_used + 1;
        txq.desc[0].addr  = tx_phys;
        txq.desc[0].len   = chunk;
        txq.desc[0].flags = 0;          /* device reads */
        txq.desc[0].next  = 0;
        vio_push(&txq, 0);

        uint64_t t = TIMEOUT;
        while (txq.used->idx != expect && t--)
            ;
        if (!t) {
            rc = -2;
            break;
        }
        txq.last_used++;
        __asm__ volatile("dmb sy" ::: "memory");
        p += chunk;
        len -= chunk;
    }
    spin_unlock(&tx_lock);
    return rc;
}

int con_read(void *buf, uint32_t len)
{
    uint8_t *p = buf;
    uint32_t n = 0;
    while (n < len && rx_tail != rx_head) {
        p[n++] = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % RING_SIZE;
    }
    return (int)n;
}

void con_wait(void)
{
    while (rx_tail == rx_head)
        sleep_on(rx_chan);
}
