#include "uri.h"
#include "array.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "pmm.h"
#include "prov.h"
#include "qidx.h"
#include "sha256.h"
#include "thread.h"
#include "timer.h"

static int scheme_is(const char *uri, const char *scheme)
{
    while (*scheme)
        if (*uri++ != *scheme++)
            return 0;
    return 1;
}

static int sys_mem(char *b, uint32_t cap)
{
    return ksnprintf(b, cap, "free_frames %lu\nfree_mib %lu\n",
                     pmm_free_frames(),
                     pmm_free_frames() * 4096 / (1024 * 1024));
}

static int sys_objects(char *b, uint32_t cap)
{
    uint32_t off = (uint32_t)ksnprintf(b, cap, "count %u\n", obj_count());
    for (uint32_t i = 0; i < obj_count() && off < cap; i++) {
        uint8_t h[SHA256_LEN];
        uint32_t len, type;
        char hex[SHA256_LEN * 2 + 1];
        if (obj_at(i, h, &len, &type) != 0)
            break;
        sha256_hex(h, hex);
        off += (uint32_t)ksnprintf(b + off, cap - off,
                                   "%s type=%u len=%u\n", hex, type, len);
    }
    return (int)off;
}

int uri_read(const char *uri, void *buf, uint32_t cap)
{
    char *b = buf;

    if (scheme_is(uri, "sys://")) {
        const char *name = uri + 6;
        if (scheme_is(name, "proc"))
            return sched_fmt(b, cap);
        if (scheme_is(name, "mem"))
            return sys_mem(b, cap);
        if (scheme_is(name, "objects"))
            return sys_objects(b, cap);
        if (scheme_is(name, "uptime"))
            return ksnprintf(b, cap, "ticks %lu\n", timer_ticks());
        if (scheme_is(name, "prov"))
            return ksnprintf(b, cap, "lineage_records %u\n", prov_count());
        if (scheme_is(name, "qlog"))
            return qidx_fmt_log(b, cap);
        if (scheme_is(name, "qidx"))
            return qidx_fmt_idx(b, cap);
        return -1;
    }

    if (scheme_is(uri, "prov://")) {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(uri + 7, h) != 0)
            return -1;
        return prov_lineage(h, b, cap);
    }

    if (scheme_is(uri, "obj://")) {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(uri + 6, h) != 0)
            return -1;
        return obj_get(h, buf, cap);
    }

    /* seq://<hex>/<lo>-<hi> — element range of a 1-D array;
     * bounded: only overlapping chunks are read. */
    if (scheme_is(uri, "seq://")) {
        uint8_t h[SHA256_LEN];
        const char *p = uri + 6;
        if (sha256_from_hexn(p, h) != 0 || p[64] != '/')
            return -1;
        p += 65;
        uint64_t lo = 0, hi = 0;
        while (*p >= '0' && *p <= '9')
            lo = lo * 10 + (uint64_t)(*p++ - '0');
        if (*p++ != '-')
            return -1;
        while (*p >= '0' && *p <= '9')
            hi = hi * 10 + (uint64_t)(*p++ - '0');
        qidx_log(0, 0, lo, hi);
        return arr_range(h, lo, hi, buf, cap);
    }

    /* var://<hex>/<chrN>:<lo>-<hi> — position query on a variants
     * array, routed through the workload-driven index. */
    if (scheme_is(uri, "var://")) {
        uint8_t h[SHA256_LEN];
        const char *p = uri + 6;
        if (sha256_from_hexn(p, h) != 0 || p[64] != '/')
            return -1;
        p += 65;
        if (p[0] == 'c' && p[1] == 'h' && p[2] == 'r')
            p += 3;
        uint32_t chrom;
        if (*p == 'X')
            chrom = 22, p++;
        else if (*p == 'Y')
            chrom = 23, p++;
        else {
            chrom = 0;
            while (*p >= '0' && *p <= '9')
                chrom = chrom * 10 + (uint32_t)(*p++ - '0');
            if (!chrom || chrom > 22)
                return -1;
            chrom--;
        }
        if (*p++ != ':')
            return -1;
        uint64_t lo = 0, hi = 0;
        while (*p >= '0' && *p <= '9')
            lo = lo * 10 + (uint64_t)(*p++ - '0');
        if (*p++ != '-')
            return -1;
        while (*p >= '0' && *p <= '9')
            hi = hi * 10 + (uint64_t)(*p++ - '0');
        return qidx_variants(h, chrom, lo, hi, b, cap, 0);
    }

    return -1;
}
