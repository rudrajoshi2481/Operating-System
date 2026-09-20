/*
 * qidx.c — coordinate queries + workload-driven index (Step 11).
 *
 * Query log: bounded ring of every seq:// / var:// lookup. sys://qlog
 * exposes it — the same stream a query-routing model would consume.
 *
 * Learned index: per-(array,chrom) hit counter. Once a chromosome
 * crosses QIDX_HOT_AFTER queries, we materialize its record span
 * [lo_rec,hi_rec) by a single full scan; every later query scans only
 * that span. Cold chromosomes stay on the baseline full scan, so the
 * index only pays for ranges the workload actually touches.
 */
#include "qidx.h"
#include "array.h"
#include "codec.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "thread.h"

#define QLOG_CAP 128
#define HOT_CAP  64

struct qentry {
    uint32_t kind, chrom;
    uint64_t lo, hi, tick;
};
static struct qentry qlog[QLOG_CAP];
static uint32_t qhead, qtotal;

struct hotentry {
    uint8_t  arr[SHA256_LEN];
    uint32_t chrom;
    uint32_t hits;
    uint64_t lo_rec, hi_rec;            /* record-index span */
    uint8_t  built;
};
static struct hotentry hot[HOT_CAP];
static uint32_t nhot;

void qidx_log(uint32_t kind, uint32_t chrom, uint64_t lo, uint64_t hi)
{
    qlog[qhead].kind  = kind;
    qlog[qhead].chrom = chrom;
    qlog[qhead].lo    = lo;
    qlog[qhead].hi    = hi;
    qlog[qhead].tick  = timer_ticks();
    qhead = (qhead + 1) % QLOG_CAP;
    qtotal++;
}

static struct hotentry *hot_find(const uint8_t arr[SHA256_LEN],
                                 uint32_t chrom)
{
    for (uint32_t i = 0; i < nhot; i++)
        if (hot[i].chrom == chrom &&
            memcmp(hot[i].arr, arr, SHA256_LEN) == 0)
            return &hot[i];
    return 0;
}

static struct hotentry *hot_add(const uint8_t arr[SHA256_LEN],
                                uint32_t chrom)
{
    if (nhot >= HOT_CAP)
        return 0;
    struct hotentry *h = &hot[nhot++];
    memcpy(h->arr, arr, SHA256_LEN);
    h->chrom = chrom;
    h->hits = 0;
    h->built = 0;
    h->lo_rec = h->hi_rec = 0;
    return h;
}

/* count variant records in a manifest array */
static int var_count(const uint8_t hash[SHA256_LEN], uint64_t *nrec)
{
    uint32_t kind, ndim;
    char dtype[8], codec[8];
    uint64_t shape[ARR_MAX_DIM], cs[ARR_MAX_DIM];
    if (arr_info(hash, &kind, dtype, codec, &ndim, shape, cs) != 0 ||
        kind != ARR_KIND_VARIANTS || ndim != 1)
        return -1;
    *nrec = shape[0] / sizeof(struct varrec);
    return 0;
}

/* scan records [r0,r1) of a variants array; emit matching rows.
 * *touched = records examined. */
static uint32_t scan(const uint8_t hash[SHA256_LEN], uint64_t r0,
                     uint64_t r1, uint32_t chrom, uint64_t lo,
                     uint64_t hi, char *out, uint32_t cap, uint32_t off,
                     uint32_t *touched)
{
    static const char bases[] = "ACGT";
    uint64_t nbytes = (r1 - r0) * sizeof(struct varrec);
    struct varrec *v = kmalloc((uint32_t)nbytes);
    if (!v)
        return off;
    int got = arr_range(hash, r0 * sizeof(struct varrec),
                        r1 * sizeof(struct varrec), v, (uint32_t)nbytes);
    if (got <= 0) {
        kfree(v);
        return off;
    }
    uint32_t n = (uint32_t)got / sizeof(struct varrec);
    *touched += n;
    for (uint32_t i = 0; i < n && off < cap - 64; i++) {
        if (v[i].chrom != chrom || v[i].pos < lo || v[i].pos >= hi)
            continue;
        off += (uint32_t)ksnprintf(out + off, cap - off,
                                   "chr%u:%lu %c>%c\n", v[i].chrom + 1,
                                   v[i].pos, bases[v[i].ref & 3],
                                   bases[v[i].alt & 3]);
    }
    kfree(v);
    return off;
}

/* materialize the record span for (arr,chrom): one full scan */
static void hot_build(const uint8_t hash[SHA256_LEN], uint32_t chrom,
                      uint64_t nrec, uint64_t *lo, uint64_t *hi)
{
    uint64_t first = nrec, last = 0;
    struct varrec *v = kmalloc((uint32_t)(nrec * sizeof(struct varrec)));
    if (v) {
        int got = arr_range(hash, 0, nrec * sizeof(struct varrec), v,
                            (uint32_t)(nrec * sizeof(struct varrec)));
        if (got > 0) {
            uint32_t n = (uint32_t)got / sizeof(struct varrec);
            for (uint32_t i = 0; i < n; i++)
                if (v[i].chrom == chrom) {
                    if (first == nrec)
                        first = i;
                    last = i + 1;
                }
        }
        kfree(v);
    }
    if (first == nrec) {                /* chrom absent: empty span */
        first = 0;
        last = 0;
    }
    *lo = first;
    *hi = last;
}

int qidx_variants(const uint8_t hash[SHA256_LEN], uint32_t chrom,
                  uint64_t lo, uint64_t hi, char *out, uint32_t cap,
                  uint32_t *touched)
{
    uint64_t nrec;
    if (var_count(hash, &nrec) != 0)
        return -1;

    struct hotentry *he = hot_find(hash, chrom);
    if (!he)
        he = hot_add(hash, chrom);

    uint64_t r0 = 0, r1 = nrec;
    int indexed = 0;
    if (he) {
        he->hits++;
        if (!he->built && he->hits >= QIDX_HOT_AFTER) {
            hot_build(hash, chrom, nrec, &he->lo_rec, &he->hi_rec);
            he->built = 1;
        }
        if (he->built) {
            r0 = he->lo_rec;
            r1 = he->hi_rec;
            indexed = 1;
        }
    }

    uint32_t t = 0;
    uint32_t off = scan(hash, r0, r1, chrom, lo, hi, out, cap, 0, &t);
    off += (uint32_t)ksnprintf(out + off, cap - off,
                               "# %s scan: %lu records\n",
                               indexed ? "indexed" : "baseline", r1 - r0);
    if (touched)
        *touched = t;
    qidx_log(1, chrom, lo, hi);
    return (int)off;
}

int qidx_fmt_log(char *buf, uint32_t cap)
{
    uint32_t off = (uint32_t)ksnprintf(buf, cap, "queries %lu\n", qtotal);
    uint32_t n = qtotal < QLOG_CAP ? qtotal : QLOG_CAP;
    uint32_t start = qtotal < QLOG_CAP ? 0 : qhead;
    for (uint32_t i = 0; i < n && off < cap - 48; i++) {
        uint32_t j = (start + i) % QLOG_CAP;
        off += (uint32_t)ksnprintf(buf + off, cap - off,
                                   "t%lu %s chr%u %lu-%lu\n",
                                   qlog[j].tick,
                                   qlog[j].kind == 1 ? "var" : "seq",
                                   qlog[j].chrom + 1,
                                   qlog[j].lo, qlog[j].hi);
    }
    return (int)off;
}

int qidx_fmt_idx(char *buf, uint32_t cap)
{
    uint32_t off = (uint32_t)ksnprintf(buf, cap, "hot entries %u\n", nhot);
    for (uint32_t i = 0; i < nhot && off < cap - 80; i++) {
        char hex[SHA256_LEN * 2 + 1];
        sha256_hex(hot[i].arr, hex);
        hex[16] = 0;
        off += (uint32_t)ksnprintf(buf + off, cap - off,
                                   "%s chr%u hits=%u %s span=%lu-%lu\n",
                                   hex, hot[i].chrom + 1, hot[i].hits,
                                   hot[i].built ? "indexed" : "cold",
                                   hot[i].lo_rec, hot[i].hi_rec);
    }
    return (int)off;
}
