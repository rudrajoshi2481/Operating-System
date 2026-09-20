/*
 * array.c — chunked N-dim array objects (Step 10, Zarr-style).
 *
 * Manifest (OBJ_ARRAY, text): dtype/shape/chunk/codec + ordered list
 * of chunk hashes. Chunks are OBJ_CHUNK objects — content-addressed,
 * so identical regions across arrays share storage for free.
 *
 * Elements are row-major. Codec transforms each chunk independently;
 * "dna2" packs A/C/G/T to 2 bits (chunk element count padded to a
 * multiple of 4 — the manifest's true shape hides the pad).
 */
#include "array.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"

#define OBJ_CHUNK  5
#define OBJ_ARRAY  6

static uint32_t dtype_size(const char *dt)
{
    if (dt[0] == 'u' && dt[1] == '8')  return 1;
    if (dt[0] == 'u' && dt[1] == '1')  return 2;   /* u16 */
    if (dt[0] == 'u' && dt[1] == '3')  return 4;   /* u32 */
    if (dt[0] == 'f')                  return 4;   /* f32 */
    return 0;
}

static uint32_t kind_code(const char *k)
{
    if (k[0] == 'g') return ARR_KIND_GENOME;
    if (k[0] == 'e') return ARR_KIND_EMBED;
    if (k[0] == 'v') return ARR_KIND_VARIANTS;
    return ARR_KIND_RAW;
}

static const char *kind_name(uint32_t k)
{
    switch (k) {
    case ARR_KIND_GENOME:   return "genome";
    case ARR_KIND_EMBED:    return "embed";
    case ARR_KIND_VARIANTS: return "variants";
    default:                return "raw";
    }
}

/* ---- dna2 codec -------------------------------------------------------- */

static uint8_t base_bits(char c)
{
    switch (c) {
    case 'A': case 'a': return 0;
    case 'C': case 'c': return 1;
    case 'G': case 'g': return 2;
    case 'T': case 't': return 3;
    }
    return 0;                               /* N and friends -> A */
}

static uint32_t dna2_pack(const uint8_t *in, uint32_t nbases, uint8_t *out)
{
    uint32_t nbytes = (nbases + 3) / 4;
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 4; j++) {
            uint32_t e = i * 4 + j;
            b |= (e < nbases ? base_bits(in[e]) : 0) << (6 - j * 2);
        }
        out[i] = b;
    }
    return nbytes;
}

static void dna2_unpack(const uint8_t *in, uint32_t nbases, uint8_t *out)
{
    static const char b[] = "ACGT";
    for (uint32_t i = 0; i < nbases; i++)
        out[i] = (uint8_t)b[(in[i / 4] >> (6 - (i % 4) * 2)) & 3];
}

/* ---- N-dim gather/scatter ---------------------------------------------- */

static uint64_t prod(const uint64_t *v, uint32_t n)
{
    uint64_t r = 1;
    for (uint32_t i = 0; i < n; i++)
        r *= v[i];
    return r;
}

/* move the box [lo,hi) between a row-major `array` and a flat `chunk`
 * buffer. dir=0 gathers (array->chunk), dir=1 scatters (chunk->array). */
static void box_copy(uint8_t *array, uint8_t *chunk, uint32_t ndim,
                     const uint64_t *shape, const uint64_t *lo,
                     const uint64_t *hi, uint64_t esz, int dir)
{
    if (ndim == 1) {
        uint64_t off = lo[0] * esz;
        uint64_t n   = (hi[0] - lo[0]) * esz;
        if (dir == 0)
            memcpy(chunk, array + off, n);
        else
            memcpy(array + off, chunk, n);
        return;
    }

    uint64_t coord[ARR_MAX_DIM];
    for (uint32_t i = 0; i < ndim; i++)
        coord[i] = lo[i];

    for (;;) {
        /* innermost dim = one contiguous run */
        uint64_t off = 0;
        for (uint32_t i = 0; i < ndim - 1; i++)
            off = off * shape[i] + coord[i];
        off = off * shape[ndim - 1] + lo[ndim - 1];
        uint64_t run = (hi[ndim - 1] - lo[ndim - 1]) * esz;
        if (dir == 0)
            memcpy(chunk, array + off * esz, run);
        else
            memcpy(array + off * esz, chunk, run);
        chunk += run;

        /* odometer over dims ndim-2 .. 0 */
        int i = (int)ndim - 2;
        for (; i >= 0; i--) {
            if (++coord[i] < hi[i])
                break;
            coord[i] = lo[i];
        }
        if (i < 0)
            break;
    }
}

/* ---- manifest encode/decode -------------------------------------------- */

static void put_u64s(char *b, uint32_t *off, uint32_t cap,
                     const uint64_t *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        *off += (uint32_t)ksnprintf(b + *off, cap - *off, "%s%lu",
                                    i ? "," : "", v[i]);
    *off += (uint32_t)ksnprintf(b + *off, cap - *off, "\n");
}

static int get_u64s(const char *line, uint64_t *v, uint32_t maxn)
{
    uint32_t n = 0;
    while (*line && *line != '\n' && n < maxn) {
        uint64_t x = 0;
        if (*line < '0' || *line > '9')
            return -1;
        while (*line >= '0' && *line <= '9')
            x = x * 10 + (uint64_t)(*line++ - '0');
        v[n++] = x;
        if (*line == ',')
            line++;
    }
    return (int)n;
}

/* find start of nth line */
static const char *nth_line(const char *s, int n)
{
    while (n-- > 0) {
        while (*s && *s != '\n')
            s++;
        if (!*s)
            return 0;
        s++;
    }
    return s;
}

/* copy "key value" line's value into out */
static int field_str(const char *s, const char *key, char *out,
                     uint32_t cap)
{
    for (const char *p = s; *p; ) {
        const char *k = key, *q = p;
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

static int field_u64(const char *s, const char *key, uint64_t *out)
{
    char tmp[80];
    if (field_str(s, key, tmp, sizeof(tmp)) != 0)
        return -1;
    return get_u64s(tmp, out, 1) == 1 ? 0 : -1;
}

/* ---- public API --------------------------------------------------------- */

int arr_put(uint32_t kind, const char *dtype, const char *codec,
            uint32_t ndim, const uint64_t *shape,
            const uint64_t *chunkshape, const void *data,
            uint64_t len, uint8_t out_hash[SHA256_LEN])
{
    uint32_t esz = dtype_size(dtype);
    int dna2 = codec[0] == 'd';
    if (!esz || !ndim || ndim > ARR_MAX_DIM || !data)
        return -1;
    uint64_t elems = prod(shape, ndim);
    if (!elems || elems > ARR_MAX_ELEMS || len < elems * esz)
        return -2;

    /* chunk-grid dims + chunk count */
    uint64_t gd[ARR_MAX_DIM];
    uint32_t nchunks = 1;
    for (uint32_t i = 0; i < ndim; i++) {
        gd[i] = (shape[i] + chunkshape[i] - 1) / chunkshape[i];
        nchunks *= (uint32_t)gd[i];
    }
    if (nchunks > ARR_MAX_CHUNKS)
        return -3;

    uint64_t max_elems = prod(chunkshape, ndim);
    uint32_t cbuf_len = (uint32_t)(dna2 ? (max_elems + 3) / 4
                                        : max_elems * esz);
    uint8_t *cbuf = kmalloc(cbuf_len);
    if (!cbuf)
        return -4;

    /* manifest grows as chunks are hashed */
    uint32_t mancap = 512 + nchunks * (SHA256_LEN * 2 + 4);
    char *man = kmalloc(mancap);
    if (!man) {
        kfree(cbuf);
        return -4;
    }
    uint32_t moff = (uint32_t)ksnprintf(man, mancap,
                                      "arr v1\nkind %s\ndtype %s\ncodec %s\n"
                                      "ndim %lu\nshape ", kind_name(kind),
                                      dtype, codec, (uint64_t)ndim);
    put_u64s(man, &moff, mancap, shape, ndim);
    moff += (uint32_t)ksnprintf(man + moff, mancap - moff, "chunk ");
    put_u64s(man, &moff, mancap, chunkshape, ndim);
    moff += (uint32_t)ksnprintf(man + moff, mancap - moff, "nchunks %u\n",
                                nchunks);

    /* walk chunk grid in row-major order */
    uint64_t cc[ARR_MAX_DIM] = {0};
    for (uint32_t ci = 0; ci < nchunks; ci++) {
        uint64_t lo[ARR_MAX_DIM], hi[ARR_MAX_DIM];
        for (uint32_t i = 0; i < ndim; i++) {
            lo[i] = cc[i] * chunkshape[i];
            hi[i] = lo[i] + chunkshape[i];
            if (hi[i] > shape[i])
                hi[i] = shape[i];
        }
        uint64_t box_elems = 1;
        for (uint32_t i = 0; i < ndim; i++)
            box_elems *= hi[i] - lo[i];

        uint32_t clen;
        if (dna2) {
            /* gather elements into cbuf first as bases, then pack */
            uint8_t *tmp = kmalloc((uint32_t)box_elems);
            if (!tmp) {
                kfree(cbuf); kfree(man);
                return -4;
            }
            box_copy((uint8_t *)data, tmp, ndim, shape, lo, hi, 1, 0);
            clen = dna2_pack(tmp, (uint32_t)box_elems, cbuf);
            kfree(tmp);
        } else {
            box_copy((uint8_t *)data, cbuf, ndim, shape, lo, hi, esz, 0);
            clen = (uint32_t)(box_elems * esz);
        }

        uint8_t ch[SHA256_LEN];
        if (obj_put(OBJ_CHUNK, cbuf, clen, ch) != 0) {
            kfree(cbuf); kfree(man);
            return -5;
        }
        char hex[SHA256_LEN * 2 + 1];
        sha256_hex(ch, hex);
        moff += (uint32_t)ksnprintf(man + moff, mancap - moff,
                                    "ch %s\n", hex);

        /* advance chunk coords row-major */
        int i = (int)ndim - 1;
        for (; i >= 0; i--) {
            if (++cc[i] < gd[i])
                break;
            cc[i] = 0;
        }
    }

    int rc = obj_put(OBJ_ARRAY, man, moff, out_hash);
    kfree(cbuf);
    kfree(man);
    return rc;
}

int arr_info(const uint8_t hash[SHA256_LEN], uint32_t *kind,
             char *dtype, char *codec, uint32_t *ndim,
             uint64_t *shape, uint64_t *chunkshape)
{
    uint8_t man[4096];
    int n = obj_get(hash, man, sizeof(man) - 1);
    if (n <= 0)
        return -1;
    man[n] = 0;
    if (!((const char *)man)[0] || ((const char *)man)[0] != 'a')
        return -1;                      /* not an array manifest */

    char k[24];
    uint64_t nd;
    if (field_str((char *)man, "kind", k, sizeof(k)) != 0 ||
        field_str((char *)man, "dtype", dtype, 8) != 0 ||
        field_str((char *)man, "codec", codec, 8) != 0 ||
        field_u64((char *)man, "ndim", &nd) != 0 || !nd ||
        nd > ARR_MAX_DIM)
        return -2;
    *kind = kind_code(k);
    *ndim = (uint32_t)nd;

    char tmp[96];
    if (field_str((char *)man, "shape", tmp, sizeof(tmp)) != 0 ||
        get_u64s(tmp, shape, ARR_MAX_DIM) != (int)nd)
        return -2;
    if (field_str((char *)man, "chunk", tmp, sizeof(tmp)) != 0 ||
        get_u64s(tmp, chunkshape, ARR_MAX_DIM) != (int)nd)
        return -2;
    return 0;
}

int arr_read(const uint8_t hash[SHA256_LEN], void *buf, uint64_t maxlen)
{
    uint32_t kind, ndim;
    char dtype[8], codec[8];
    uint64_t shape[ARR_MAX_DIM], cs[ARR_MAX_DIM];
    if (arr_info(hash, &kind, dtype, codec, &ndim, shape, cs) != 0)
        return -1;
    (void)kind;

    uint32_t esz = dtype_size(dtype);
    int dna2 = codec[0] == 'd';
    uint64_t elems = prod(shape, ndim);
    if (elems * esz > maxlen)
        return -2;

    uint8_t man[4096];
    int mn = obj_get(hash, man, sizeof(man) - 1);
    if (mn <= 0)
        return -1;
    man[mn] = 0;

    /* first "ch " line: header is arr/kind/dtype/codec/ndim/shape/chunk/
     * nchunks = 8 lines */
    const char *p = nth_line((const char *)man, 8);
    if (!p)
        return -3;

    uint64_t gd[ARR_MAX_DIM];
    uint32_t nchunks = 1;
    for (uint32_t i = 0; i < ndim; i++) {
        gd[i] = (shape[i] + cs[i] - 1) / cs[i];
        nchunks *= (uint32_t)gd[i];
    }

    uint8_t *cbuf = kmalloc(prod(cs, ndim) * esz + 4);
    uint8_t *ubuf = kmalloc((uint32_t)(prod(cs, ndim) + 4));
    if (!cbuf || !ubuf) {
        kfree(cbuf); kfree(ubuf);
        return -4;
    }

    uint64_t cc[ARR_MAX_DIM] = {0};
    for (uint32_t ci = 0; ci < nchunks; ci++, p = nth_line(p, 1)) {
        if (!p || p[0] != 'c' || p[1] != 'h' || p[2] != ' ')
            break;
        char hex[SHA256_LEN * 2 + 1];
        memcpy(hex, p + 3, SHA256_LEN * 2);
        hex[SHA256_LEN * 2] = 0;
        uint8_t ch[SHA256_LEN];
        if (sha256_from_hex(hex, ch) != 0)
            break;

        uint64_t lo[ARR_MAX_DIM], hi[ARR_MAX_DIM], be = 1;
        for (uint32_t i = 0; i < ndim; i++) {
            lo[i] = cc[i] * cs[i];
            hi[i] = lo[i] + cs[i];
            if (hi[i] > shape[i])
                hi[i] = shape[i];
            be *= hi[i] - lo[i];
        }

        int cn = obj_get(ch, cbuf, (uint32_t)(dna2 ? (be + 3) / 4
                                                  : be * esz));
        if (cn <= 0) {
            kfree(cbuf); kfree(ubuf);
            return -5;
        }
        if (dna2) {
            dna2_unpack(cbuf, (uint32_t)be, ubuf);
            box_copy(buf, ubuf, ndim, shape, lo, hi, 1, 1);
        } else {
            box_copy(buf, cbuf, ndim, shape, lo, hi, esz, 1);
        }

        int i = (int)ndim - 1;
        for (; i >= 0; i--) {
            if (++cc[i] < gd[i])
                break;
            cc[i] = 0;
        }
    }
    kfree(cbuf);
    kfree(ubuf);
    return (int)elems;
}

/* ---- bounded range read (1-D) ------------------------------------------ */

int arr_range(const uint8_t hash[SHA256_LEN], uint64_t lo, uint64_t hi,
              void *buf, uint64_t maxlen)
{
    uint32_t kind, ndim;
    char dtype[8], codec[8];
    uint64_t shape[ARR_MAX_DIM], cs[ARR_MAX_DIM];
    if (arr_info(hash, &kind, dtype, codec, &ndim, shape, cs) != 0)
        return -1;
    (void)kind;
    if (ndim != 1 || lo >= hi || hi > shape[0])
        return -2;

    uint32_t esz = dtype_size(dtype);
    int dna2 = codec[0] == 'd';
    if ((hi - lo) * esz > maxlen)
        return -3;

    uint8_t man[4096];
    int mn = obj_get(hash, man, sizeof(man) - 1);
    if (mn <= 0)
        return -1;
    man[mn] = 0;
    const char *p = nth_line((const char *)man, 8);
    if (!p)
        return -4;

    uint64_t nchunks = (shape[0] + cs[0] - 1) / cs[0];
    uint64_t c0 = lo / cs[0], c1 = (hi - 1) / cs[0];

    uint8_t *cbuf = kmalloc(cs[0] * esz + 4);
    uint8_t *ubuf = kmalloc((uint32_t)cs[0] + 4);
    if (!cbuf || !ubuf) {
        kfree(cbuf); kfree(ubuf);
        return -5;
    }

    uint64_t out = 0;
    for (uint64_t ci = 0; ci < nchunks && p; ci++, p = nth_line(p, 1)) {
        if (ci < c0)
            continue;                   /* skip chunk hashes before range */
        if (ci > c1)
            break;                      /* past the range: stop loading */
        if (p[0] != 'c' || p[1] != 'h' || p[2] != ' ')
            break;
        char hex[SHA256_LEN * 2 + 1];
        memcpy(hex, p + 3, SHA256_LEN * 2);
        hex[SHA256_LEN * 2] = 0;
        uint8_t ch[SHA256_LEN];
        if (sha256_from_hex(hex, ch) != 0)
            break;

        uint64_t clo = ci * cs[0];
        uint64_t chi = clo + cs[0];
        if (chi > shape[0])
            chi = shape[0];
        uint64_t ce = chi - clo;

        int cn = obj_get(ch, cbuf, (uint32_t)(dna2 ? (ce + 3) / 4
                                                  : ce * esz));
        if (cn <= 0) {
            kfree(cbuf); kfree(ubuf);
            return -6;
        }
        const uint8_t *src = cbuf;
        if (dna2) {
            dna2_unpack(cbuf, (uint32_t)ce, ubuf);
            src = ubuf;
        }
        /* intersect [clo,chi) with [lo,hi) */
        uint64_t s = lo > clo ? lo - clo : 0;
        uint64_t e = hi < chi ? hi - clo : ce;
        memcpy((uint8_t *)buf + out * esz, src + s * esz,
               (e - s) * esz);
        out += e - s;
    }
    kfree(cbuf);
    kfree(ubuf);
    return (int)out;
}
