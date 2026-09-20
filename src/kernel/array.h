#pragma once

#include <stdint.h>
#include "sha256.h"
#include "objstore.h"

/* array.c — chunked N-dimensional array objects (Zarr data model).
 *
 * An array is an OBJ_ARRAY manifest object:
 *   arr v1 / kind / dtype / codec / ndim / shape / chunk / nchunks /
 *   ch <hex> ...
 * whose payload chunks live as separate OBJ_CHUNK objects — identical
 * chunks dedup automatically since everything is content-addressed.
 *
 * dtype: u8 u16 u32 f32   codec: raw dna2 (2-bit ACGT) */

#define ARR_MAX_DIM    4
#define ARR_MAX_CHUNKS 128
#define ARR_MAX_ELEMS  (1 << 20)

/* `kind` is stored as the manifest's semantic tag; it maps onto the
 * object-store type namespace (5/6 are chunk/array manifest internals). */
#define ARR_KIND_RAW      OBJ_RAW
#define ARR_KIND_GENOME   OBJ_GENOME
#define ARR_KIND_EMBED    7
#define ARR_KIND_VARIANTS 8

int  arr_put(uint32_t kind, const char *dtype, const char *codec,
             uint32_t ndim, const uint64_t *shape,
             const uint64_t *chunkshape, const void *data,
             uint64_t len, uint8_t out_hash[SHA256_LEN]);

/* Reassemble the whole array into buf (raw elements, unpacked).
 * Returns element count or <0. */
int  arr_read(const uint8_t hash[SHA256_LEN], void *buf, uint64_t maxlen);

/* Bounded range read: elements [lo,hi) of a 1-D array; only loads the
 * chunks the range overlaps. Returns elements copied or <0. */
int  arr_range(const uint8_t hash[SHA256_LEN], uint64_t lo, uint64_t hi,
               void *buf, uint64_t maxlen);

/* Parse manifest fields without loading chunks. */
int  arr_info(const uint8_t hash[SHA256_LEN], uint32_t *kind,
              char *dtype, char *codec, uint32_t *ndim,
              uint64_t *shape, uint64_t *chunkshape);
