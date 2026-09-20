/*
 * prov.c — provenance engine (Step 9).
 *
 * Lineage record (OBJ_PROV object, text):
 *   prov v1
 *   out <hex>     object this record describes
 *   op  <name>    create | derive | put | ingest | ...
 *   by  <caller>  shell | selftest | host | user
 *   t   <tick>
 *   in  <hex>     zero or more input object ids
 *
 * The record is stored like any other object, so lineage is immutable,
 * deduplicated, and persists across mounts. An in-memory map links an
 * object hash to the hash of its provenance record.
 */
#include "prov.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "thread.h"

#define PROV_CAP 512
#define MAX_LINEAGE_NODES 64

static struct {
    uint8_t out[SHA256_LEN];
    uint8_t rec[SHA256_LEN];
} pmap[PROV_CAP];
static uint32_t nprov;

static const uint8_t *map_find(const uint8_t out[SHA256_LEN])
{
    for (uint32_t i = 0; i < nprov; i++)
        if (memcmp(pmap[i].out, out, SHA256_LEN) == 0)
            return pmap[i].rec;
    return 0;
}

static void map_add(const uint8_t out[SHA256_LEN],
                    const uint8_t rec[SHA256_LEN])
{
    if (nprov >= PROV_CAP || map_find(out))
        return;
    memcpy(pmap[nprov].out, out, SHA256_LEN);
    memcpy(pmap[nprov].rec, rec, SHA256_LEN);
    nprov++;
}

void prov_note(const uint8_t out[SHA256_LEN], const char *op,
               const char *by, const uint8_t ins[][SHA256_LEN],
               uint32_t nins)
{
    char rec[512];
    char hex[SHA256_LEN * 2 + 1];
    int off = 0;

    sha256_hex(out, hex);
    off += ksnprintf(rec + off, sizeof(rec) - off,
                     "prov v1\nout %s\nop %s\nby %s\nt %lu\n",
                     hex, op, by, timer_ticks());
    for (uint32_t i = 0; i < nins && off < (int)sizeof(rec) - 80; i++) {
        sha256_hex(ins[i], hex);
        off += ksnprintf(rec + off, sizeof(rec) - off, "in %s\n", hex);
    }

    uint8_t rh[SHA256_LEN];
    if (obj_put(OBJ_PROV, rec, (uint32_t)off, rh) == 0)
        map_add(out, rh);
}

/* ---- lineage walk ----------------------------------------------------- */

static uint8_t seen[MAX_LINEAGE_NODES][SHA256_LEN];
static uint32_t nseen;

static int already_seen(const uint8_t h[SHA256_LEN])
{
    for (uint32_t i = 0; i < nseen; i++)
        if (memcmp(seen[i], h, SHA256_LEN) == 0)
            return 1;
    return 0;
}

/* parse "key <hex>" lines out of a record body */
static int rec_hex(const char *rec, const char *key, int nth,
                   uint8_t out[SHA256_LEN])
{
    int hits = 0;
    for (const char *p = rec; *p; ) {
        const char *k = key;
        const char *q = p;
        while (*k && *q == *k)
            k++, q++;
        if (!*k && *q == ' ') {
            char hex[SHA256_LEN * 2 + 1];
            int i = 0;
            q++;
            while (i < SHA256_LEN * 2 && q[i] && q[i] != '\n') {
                hex[i] = q[i];
                i++;
            }
            hex[i] = 0;
            if (hits++ == nth)
                return sha256_from_hex(hex, out);
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return -1;
}

static int rec_field(const char *rec, const char *key, char *out,
                     uint32_t cap)
{
    for (const char *p = rec; *p; ) {
        const char *k = key;
        const char *q = p;
        while (*k && *q == *k)
            k++, q++;
        if (!*k && *q == ' ') {
            q++;
            uint32_t i = 0;
            while (i + 1 < cap && q[i] && q[i] != '\n') {
                out[i] = q[i];
                i++;
            }
            out[i] = 0;
            return 0;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return -1;
}

static int walk(const uint8_t h[SHA256_LEN], int depth, char *buf,
                uint32_t cap, uint32_t off)
{
    if (depth > 16 || nseen >= MAX_LINEAGE_NODES || already_seen(h))
        return (int)off;
    memcpy(seen[nseen++], h, SHA256_LEN);

    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(h, hex);
    for (int i = 0; i < depth && off < cap; i++)
        off += (uint32_t)ksnprintf(buf + off, cap - off, "  ");
    off += (uint32_t)ksnprintf(buf + off, cap - off, "%s\n", hex);

    const uint8_t *rech = map_find(h);
    if (!rech || off >= cap)
        return (int)off;

    char rec[512];
    int n = obj_get(rech, rec, sizeof(rec) - 1);
    if (n <= 0)
        return (int)off;
    rec[n] = 0;

    char op[40], by[24], t[16];
    rec_field(rec, "op", op, sizeof(op));
    rec_field(rec, "by", by, sizeof(by));
    rec_field(rec, "t", t, sizeof(t));
    for (int i = 0; i < depth + 1 && off < cap; i++)
        off += (uint32_t)ksnprintf(buf + off, cap - off, "  ");
    off += (uint32_t)ksnprintf(buf + off, cap - off,
                               "op=%s by=%s t=%s\n", op, by, t);

    for (int i = 0; off < cap; i++) {
        uint8_t in[SHA256_LEN];
        if (rec_hex(rec, "in", i, in) != 0)
            break;
        off = (uint32_t)walk(in, depth + 1, buf, cap, off);
    }
    return (int)off;
}

int prov_lineage(const uint8_t out[SHA256_LEN], char *buf, uint32_t cap)
{
    nseen = 0;
    return walk(out, 0, buf, cap, 0);
}

/* ---- rebuild after mount ---------------------------------------------- */

void prov_rebuild(void)
{
    nprov = 0;
    for (uint32_t i = 0; i < obj_count() && nprov < PROV_CAP; i++) {
        uint8_t h[SHA256_LEN];
        uint32_t len, type;
        if (obj_at(i, h, &len, &type) != 0 || type != OBJ_PROV)
            continue;
        char rec[512];
        int n = obj_get(h, rec, sizeof(rec) - 1);
        if (n <= 0)
            continue;
        rec[n] = 0;
        uint8_t out[SHA256_LEN];
        if (rec_hex(rec, "out", 0, out) == 0)
            map_add(out, h);
    }
    kprint("prov: %u lineage records\n", nprov);
}

uint32_t prov_count(void)
{
    return nprov;
}
