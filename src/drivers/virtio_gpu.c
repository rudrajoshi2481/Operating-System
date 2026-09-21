/*
 * virtio_gpu.c — virtio-gpu 2D scanout (Step 16 in-OS UI path).
 *
 * aarch64 QEMU ships edk2 without a virtio-gpu GOP driver, so no
 * framebuffer reaches Limine there — this driver provides the fb
 * directly over the existing virtio transport (mmio on aarch64;
 * legacy pci never exposes gpu so probe fails gracefully on x86).
 *
 * 2D command flow: GET_DISPLAY_INFO -> RESOURCE_CREATE_2D ->
 * ATTACH_BACKING(guest pages) -> SET_SCANOUT. Per-frame:
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 */
#include "virtio_gpu.h"
#include "virtio_mmio.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"

#define VIO_DEV_GPU     16

#define GPU_GET_DISPLAY_INFO   0x0100
#define GPU_RESOURCE_CREATE_2D 0x0101
#define GPU_SET_SCANOUT        0x0103
#define GPU_RESOURCE_FLUSH     0x0104
#define GPU_TRANSFER_HOST_2D   0x0105
#define GPU_ATTACH_BACKING     0x0106

#define GPU_RESP_OK_NODATA     0x1100
#define GPU_RESP_OK_INFO       0x1101

#define GPU_FMT_B8G8R8X8       1
#define GPU_RES_ID             1
#define GPU_QSIZE              64
#define GPU_TIMEOUT            0x4000000

struct gpu_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence;
    uint32_t ctx;
    uint32_t pad;
};

struct gpu_pmode {
    uint32_t x, y, w, h;
    uint32_t enabled;
    uint32_t flags;
};

static struct vdev      dev;
static struct virtqueue ctrlq;
static uint64_t         req_phys, resp_phys;
static volatile uint8_t *req_va, *resp_va;
static uint64_t         fb_phys;
static volatile uint32_t *fb_va;
static uint32_t         fb_w, fb_h;
static int              ready;

/* one controlq request: desc[0]=req out, desc[1]=resp in; polled.
 * Callers memset+fill req_va before calling. */
static int gpu_cmd(uint32_t reqlen, uint32_t resplen, uint32_t want)
{
    memset((void *)resp_va, 0, 1024);

    ctrlq.desc[0].addr  = req_phys;
    ctrlq.desc[0].len   = reqlen;
    ctrlq.desc[0].flags = VQ_DESC_NEXT;
    ctrlq.desc[0].next  = 1;
    ctrlq.desc[1].addr  = resp_phys;
    ctrlq.desc[1].len   = resplen;
    ctrlq.desc[1].flags = VQ_DESC_WRITE;
    ctrlq.desc[1].next  = 0;
    vio_push(&ctrlq, 0);

    for (uint64_t t = 0; t < GPU_TIMEOUT; t++) {
        if (ctrlq.used->idx != ctrlq.last_used) {
            __sync_synchronize();
            ctrlq.last_used++;
            vio_isr(&dev);
            volatile struct gpu_hdr *r = (volatile struct gpu_hdr *)resp_va;
            return r->type == want ? 0 : -2;
        }
    }
    return -1;
}

int gpu_init(uint64_t hhdm)
{
    if (vio_probe(hhdm, VIO_DEV_GPU, 0, &dev) != 0)
        return -1;
    if (vio_init(&dev) != 0)
        return -2;
    if (vio_queue_init(&dev, 0, &ctrlq, GPU_QSIZE) != 0)
        return -3;
    vio_driver_ok(&dev);

    req_phys  = pmm_alloc(0);
    resp_phys = pmm_alloc(0);
    if (!req_phys || !resp_phys)
        return -4;
    req_va  = (volatile uint8_t *)pmm_to_virt(req_phys);
    resp_va = (volatile uint8_t *)pmm_to_virt(resp_phys);

    /* GET_DISPLAY_INFO -> first enabled pmode */
    memset((void *)req_va, 0, 1024);
    volatile struct gpu_hdr *r = (volatile struct gpu_hdr *)req_va;
    r->type = GPU_GET_DISPLAY_INFO;
    if (gpu_cmd(sizeof(struct gpu_hdr), 1024, GPU_RESP_OK_INFO) != 0)
        return -5;
    volatile struct gpu_pmode *pm =
        (volatile struct gpu_pmode *)(resp_va + sizeof(struct gpu_hdr));
    if (!pm->enabled || !pm->w || !pm->h)
        return -6;
    fb_w = pm->w;
    fb_h = pm->h;

    /* one contiguous resource: w*h*4 bytes, order-sized alloc */
    uint64_t bytes = (uint64_t)fb_w * fb_h * 4;
    unsigned order = 0;
    while ((4096ull << order) < bytes)
        order++;
    if (order > PMM_MAX_ORDER)
        return -7;
    fb_phys = pmm_alloc(order);
    if (!fb_phys)
        return -7;
    fb_va = (volatile uint32_t *)pmm_to_virt(fb_phys);
    memset((void *)fb_va, 0, (size_t)bytes);

    /* RESOURCE_CREATE_2D */
    memset((void *)req_va, 0, 1024);
    volatile uint32_t *w = (volatile uint32_t *)(req_va + sizeof(struct gpu_hdr));
    r->type = GPU_RESOURCE_CREATE_2D;
    w[0] = GPU_RES_ID;
    w[1] = GPU_FMT_B8G8R8X8;
    w[2] = fb_w;
    w[3] = fb_h;
    if (gpu_cmd(sizeof(struct gpu_hdr) + 16, sizeof(struct gpu_hdr),
                GPU_RESP_OK_NODATA) != 0)
        return -8;

    /* RESOURCE_ATTACH_BACKING (single entry) */
    memset((void *)req_va, 0, 1024);
    r->type = GPU_ATTACH_BACKING;
    w = (volatile uint32_t *)(req_va + sizeof(struct gpu_hdr));
    w[0] = GPU_RES_ID;
    w[1] = 1;                                   /* nr_entries */
    volatile uint64_t *e =
        (volatile uint64_t *)(req_va + sizeof(struct gpu_hdr) + 8);
    e[0] = fb_phys;
    w[4] = (uint32_t)bytes;                     /* entry.length */
    w[5] = 0;
    if (gpu_cmd(sizeof(struct gpu_hdr) + 8 + 16, sizeof(struct gpu_hdr),
                GPU_RESP_OK_NODATA) != 0)
        return -9;

    /* SET_SCANOUT scanout 0 -> resource */
    memset((void *)req_va, 0, 1024);
    r->type = GPU_SET_SCANOUT;
    w = (volatile uint32_t *)(req_va + sizeof(struct gpu_hdr));
    w[0] = 0; w[1] = 0;                         /* rect x,y */
    w[2] = fb_w; w[3] = fb_h;                   /* rect w,h */
    w[4] = 0;                                   /* scanout_id */
    w[5] = GPU_RES_ID;
    if (gpu_cmd(sizeof(struct gpu_hdr) + 24, sizeof(struct gpu_hdr),
                GPU_RESP_OK_NODATA) != 0)
        return -10;

    ready = 1;
    kprint("gpu: %ux%u scanout\n", fb_w, fb_h);
    return 0;
}

int gpu_ok(void) { return ready; }

volatile uint32_t *gpu_fb(uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    if (!ready)
        return 0;
    *w = fb_w;
    *h = fb_h;
    *pitch = fb_w;
    return fb_va;
}

/* push the whole framebuffer to the scanout */
void gpu_flush(void)
{
    if (!ready)
        return;
    memset((void *)req_va, 0, 1024);
    volatile struct gpu_hdr *r = (volatile struct gpu_hdr *)req_va;
    volatile uint32_t *w = (volatile uint32_t *)(req_va + sizeof(struct gpu_hdr));

    r->type = GPU_TRANSFER_HOST_2D;
    w[0] = 0; w[1] = 0;
    w[2] = fb_w; w[3] = fb_h;
    w[4] = 0; w[5] = 0;                         /* offset u64 */
    w[6] = GPU_RES_ID; w[7] = 0;
    if (gpu_cmd(sizeof(struct gpu_hdr) + 32, sizeof(struct gpu_hdr),
                GPU_RESP_OK_NODATA) != 0)
        return;

    memset((void *)req_va, 0, 1024);
    r->type = GPU_RESOURCE_FLUSH;
    w[0] = 0; w[1] = 0;
    w[2] = fb_w; w[3] = fb_h;
    w[4] = GPU_RES_ID; w[5] = 0;
    (void)gpu_cmd(sizeof(struct gpu_hdr) + 24, sizeof(struct gpu_hdr),
                  GPU_RESP_OK_NODATA);
}
