/*
 * objstore.c — content-addressed object store on a raw virtio-blk disk.
 *
 * On-disk layout (sector = 512 B):
 *   sector 0 : superblock { magic, version, count, head_lba }
 *   sector 1+: append-only record log. Each record:
 *     struct orec_hdr (56 B) + data bytes, padded to a sector boundary.
 *
 * Objects are keyed by SHA-256 of their payload. put() dedups — the
 * same bytes are never written twice — so the log is immutable and
 * copy-on-write comes free. Mount scans the log to rebuild the index.
 */
#include "objstore.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "spinlock.h"

#define SECTOR      512
#define SB_MAGIC    0x314f4953424f4942ULL   /* "BIOSTOI1"-ish LE tag */
#define REC_MAGIC   0x0b1ec7edUL
#define IDX_CAP     1024
#define HDR_SECTORS 1

struct superblock {
    uint64_t magic;
    uint32_t version;
    uint32_t type_pad;
    uint64_t count;
    uint64_t head_lba;                  /* next free sector */
};

struct orec_hdr {
    uint32_t magic;
    uint32_t type;
    uint64_t len;
    uint8_t  hash[SHA256_LEN];
    uint8_t  rsvd[8];
};                                      /* 56 bytes */

struct idxent {
    uint8_t  hash[SHA256_LEN];
    uint64_t lba;
    uint32_t len;
    uint32_t type;
    uint8_t  used;
};

static struct blkdev *disk;
static struct idxent  idx[IDX_CAP];
static uint32_t       nidx;             /* live entries */
static uint64_t       head_lba;
static spinlock_t     lock;
static int            mounted;

/* ---- index ---------------------------------------------------------- */

static struct idxent *idx_find(const uint8_t hash[SHA256_LEN])
{
    uint32_t h = *(const uint32_t *)hash % IDX_CAP;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        struct idxent *e = &idx[(h + i) % IDX_CAP];
        if (!e->used)
            return 0;
        if (memcmp(e->hash, hash, SHA256_LEN) == 0)
            return e;
    }
    return 0;
}

static int idx_insert(const uint8_t hash[SHA256_LEN], uint64_t lba,
                      uint32_t len, uint32_t type)
{
    uint32_t h = *(const uint32_t *)hash % IDX_CAP;
    for (uint32_t i = 0; i < IDX_CAP; i++) {
        struct idxent *e = &idx[(h + i) % IDX_CAP];
        if (!e->used) {
            e->used = 1;
            memcpy(e->hash, hash, SHA256_LEN);
            e->lba = lba;
            e->len = len;
            e->type = type;
            nidx++;
            return 0;
        }
        if (memcmp(e->hash, hash, SHA256_LEN) == 0)
            return 0;                   /* already indexed */
    }
    return -1;                          /* index full */
}

static uint64_t rec_sectors(uint64_t len)
{
    return (sizeof(struct orec_hdr) + len + SECTOR - 1) / SECTOR;
}

/* ---- mount ------------------------------------------------------------ */

static int obj_format(void)
{
    uint8_t *sec = kmalloc(SECTOR);
    if (!sec)
        return -1;
    memset(sec, 0, SECTOR);
    struct superblock *sb = (struct superblock *)sec;
    sb->magic    = SB_MAGIC;
    sb->version  = 1;
    sb->count    = 0;
    sb->head_lba = 1;
    int rc = blk_write(disk, 0, sec, SECTOR);
    kfree(sec);
    head_lba = 1;
    kprint("objstore: formatted\n");
    return rc;
}

int obj_mount(struct blkdev *d)
{
    if (!d || !d->ready)
        return -1;
    disk = d;

    uint8_t *sec = kmalloc(SECTOR);
    if (!sec)
        return -2;
    if (blk_read(disk, 0, sec, SECTOR) != 0) {
        kfree(sec);
        return -3;
    }
    struct superblock *sb = (struct superblock *)sec;
    if (sb->magic != SB_MAGIC) {
        kfree(sec);
        int rc = obj_format();
        if (rc != 0)
            return rc;
        mounted = 1;
        kprint("objstore: mounted, %u objects, head lba %lu\n",
               nidx, head_lba);
        return 0;
    }
    head_lba = sb->head_lba;
    kfree(sec);

    /* rebuild the index by scanning the append-only log */
    uint8_t *hbuf = kmalloc(SECTOR);
    if (!hbuf)
        return -4;
    uint64_t lba = 1;
    while (lba < head_lba) {
        if (blk_read(disk, lba, hbuf, SECTOR) != 0)
            break;
        struct orec_hdr *r = (struct orec_hdr *)hbuf;
        if (r->magic != REC_MAGIC || r->len > OBJ_MAX_SIZE)
            break;
        if (idx_insert(r->hash, lba, (uint32_t)r->len, r->type) != 0)
            break;
        lba += rec_sectors(r->len);
    }
    kfree(hbuf);

    mounted = 1;
    kprint("objstore: mounted, %u objects, head lba %lu\n", nidx, head_lba);
    return 0;
}

/* ---- put / get -------------------------------------------------------- */

int obj_put(uint32_t type, const void *data, uint32_t len,
            uint8_t out_hash[SHA256_LEN])
{
    if (!mounted || !data || len == 0 || len > OBJ_MAX_SIZE)
        return -1;

    uint8_t hash[SHA256_LEN];
    sha256(data, len, hash);
    if (out_hash)
        memcpy(out_hash, hash, SHA256_LEN);

    spin_lock(&lock);
    if (idx_find(hash)) {               /* dedup: same content, same id */
        spin_unlock(&lock);
        return 0;
    }
    if (nidx >= IDX_CAP) {
        spin_unlock(&lock);
        return -2;
    }

    uint64_t nsec = rec_sectors(len);
    uint32_t blen = (uint32_t)(nsec * SECTOR);
    uint8_t *rec = kmalloc(blen);
    if (!rec) {
        spin_unlock(&lock);
        return -3;
    }
    memset(rec, 0, blen);
    struct orec_hdr *h = (struct orec_hdr *)rec;
    h->magic = REC_MAGIC;
    h->type  = type;
    h->len   = len;
    memcpy(h->hash, hash, SHA256_LEN);
    memcpy(rec + sizeof(*h), data, len);

    if (blk_write(disk, head_lba, rec, blen) != 0) {
        kfree(rec);
        spin_unlock(&lock);
        return -4;
    }
    kfree(rec);

    uint64_t obj_lba = head_lba;
    head_lba += nsec;

    /* persist head + count */
    uint8_t *sec = kmalloc(SECTOR);
    if (sec) {
        memset(sec, 0, SECTOR);
        struct superblock *sb = (struct superblock *)sec;
        sb->magic    = SB_MAGIC;
        sb->version  = 1;
        sb->count    = nidx + 1;
        sb->head_lba = head_lba;
        blk_write(disk, 0, sec, SECTOR);
        kfree(sec);
    }

    idx_insert(hash, obj_lba, len, type);
    spin_unlock(&lock);
    return 0;
}

int obj_get(const uint8_t hash[SHA256_LEN], void *buf, uint32_t maxlen)
{
    if (!mounted || !hash || !buf)
        return -1;
    spin_lock(&lock);
    struct idxent *e = idx_find(hash);
    if (!e) {
        spin_unlock(&lock);
        return -1;
    }
    if (e->len > maxlen) {
        uint32_t need = e->len;
        spin_unlock(&lock);
        (void)need;
        return -2;
    }

    uint32_t blen = (uint32_t)(rec_sectors(e->len) * SECTOR);
    uint8_t *rec = kmalloc(blen);
    if (!rec) {
        spin_unlock(&lock);
        return -3;
    }
    int rc = blk_read(disk, e->lba, rec, blen);
    if (rc == 0) {
        struct orec_hdr *h = (struct orec_hdr *)rec;
        if (h->magic == REC_MAGIC && h->len == e->len)
            memcpy(buf, rec + sizeof(*h), e->len);
        else
            rc = -4;
    }
    uint32_t len = e->len;
    kfree(rec);
    spin_unlock(&lock);
    return rc == 0 ? (int)len : -5;
}

int obj_exists(const uint8_t hash[SHA256_LEN])
{
    if (!mounted || !hash)
        return 0;
    spin_lock(&lock);
    int r = idx_find(hash) != 0;
    spin_unlock(&lock);
    return r;
}

uint32_t obj_count(void)
{
    return nidx;
}

void obj_selftest(void)
{
    const char *msg = "bioos object store selftest";
    uint8_t h[SHA256_LEN];
    uint8_t buf[64];

    if (obj_put(OBJ_TEXT, msg, 27, h) != 0) {
        kprint("objstore: selftest put failed\n");
        return;
    }
    int n = obj_get(h, buf, sizeof(buf));
    if (n != 27 || memcmp(buf, msg, 27) != 0) {
        kprint("objstore: selftest get mismatch\n");
        return;
    }
    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(h, hex);
    kprint("objstore: selftest ok, obj://%s\n", hex);
}

int obj_at(uint32_t i, uint8_t hash[SHA256_LEN], uint32_t *len,
           uint32_t *type)
{
    uint32_t seen = 0;
    for (uint32_t k = 0; k < IDX_CAP; k++) {
        if (!idx[k].used)
            continue;
        if (seen++ == i) {
            memcpy(hash, idx[k].hash, SHA256_LEN);
            *len  = idx[k].len;
            *type = idx[k].type;
            return 0;
        }
    }
    return -1;
}
