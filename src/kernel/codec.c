/*
 * codec.c — ingest codecs (Step 10).
 *
 * codec_vcf: minimal VCF parser. Data lines "CHROM POS ID REF ALT ..."
 * become packed variant records (16 B): chrom id, position, ref/alt
 * base bits. SNVs only — multi-base records are skipped and counted.
 * Records are stored as a chunked OBJ_ARRAY with kind=variants.
 */
#include "codec.h"
#include "array.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"

struct varrec {
    uint32_t chrom;                     /* 0..23 chr1-22,X,Y ; 255 other */
    uint64_t pos;
    uint8_t  ref, alt;                  /* 0-3 packed base */
    uint16_t flags;
};                                      /* 16 bytes */

#define MAX_VARS (64 * 1024 / sizeof(struct varrec))   /* cap 4 KiB chunk */

static uint32_t chrom_id(const char *s, uint32_t len)
{
    if (len >= 3 && s[0] == 'c' && s[1] == 'h' && s[2] == 'r') {
        s += 3;
        len -= 3;
    }
    if (len == 1 && s[0] == 'X') return 22;
    if (len == 1 && s[0] == 'Y') return 23;
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 255;
        n = n * 10 + (uint32_t)(s[i] - '0');
    }
    return (n >= 1 && n <= 22) ? n - 1 : 255;
}

static int8_t base2(char c)
{
    switch (c) {
    case 'A': case 'a': return 0;
    case 'C': case 'c': return 1;
    case 'G': case 'g': return 2;
    case 'T': case 't': return 3;
    }
    return -1;
}

/* split a line into fields at tabs/spaces; returns field text bounds */
static const char *field(const char *p, int *flen)
{
    while (*p == '\t' || *p == ' ')
        p++;
    const char *s = p;
    while (*p && *p != '\t' && *p != ' ' && *p != '\n' && *p != '\r')
        p++;
    *flen = (int)(p - s);
    return s;
}

int codec_vcf(const void *data, uint32_t len, uint8_t out_hash[SHA256_LEN],
              uint32_t *nvars_out)
{
    struct varrec *vars = kmalloc(MAX_VARS * sizeof(struct varrec));
    if (!vars)
        return -1;
    uint32_t nv = 0, skipped = 0;

    const char *p = data, *end = p + len;
    while (p < end && nv < MAX_VARS) {
        if (*p == '#') {                        /* header/meta */
            while (p < end && *p != '\n')
                p++;
            if (p < end)
                p++;
            continue;
        }
        int fl;
        const char *f0 = field(p, &fl);          /* CHROM */
        uint32_t ch = chrom_id(f0, (uint32_t)fl);
        p = f0 + fl;

        const char *f1 = field(p, &fl);          /* POS */
        uint64_t pos = 0;
        for (int i = 0; i < fl; i++)
            pos = pos * 10 + (uint64_t)(f1[i] - '0');
        p = f1 + fl;

        const char *f2 = field(p, &fl);          /* ID (skip) */
        p = f2 + fl;
        const char *f3 = field(p, &fl);          /* REF */
        int8_t r = fl == 1 ? base2(f3[0]) : -1;
        p = f3 + fl;
        const char *f4 = field(p, &fl);          /* ALT */
        int8_t a = fl == 1 ? base2(f4[0]) : -1;
        p = f4 + fl;

        if (ch != 255 && r >= 0 && a >= 0) {
            vars[nv].chrom = ch;
            vars[nv].pos   = pos;
            vars[nv].ref   = (uint8_t)r;
            vars[nv].alt   = (uint8_t)a;
            vars[nv].flags = 0;
            nv++;
        } else {
            skipped++;
        }
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;
    }

    if (!nv) {
        kfree(vars);
        return -2;
    }

    /* one u8-typed array of packed records, chunked at 4 KiB */
    uint64_t shape = (uint64_t)nv * sizeof(struct varrec);
    uint64_t cshape = 4096;
    int rc = arr_put(ARR_KIND_VARIANTS, "u8", "raw", 1, &shape, &cshape,
                     vars, (uint64_t)nv * sizeof(struct varrec), out_hash);
    kfree(vars);
    if (rc == 0) {
        prov_note(out_hash, "ingest-vcf", "host", 0, 0);
        if (nvars_out)
            *nvars_out = nv;
        kprint("codec: vcf ingested, %u variants (%u skipped)\n",
               nv, skipped);
    }
    return rc;
}
