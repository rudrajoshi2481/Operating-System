#pragma once

#include <stdint.h>
#include "sha256.h"
#include "virtio_blk.h"

/* object types */
#define OBJ_RAW    0
#define OBJ_TEXT   1
#define OBJ_JSON   2
#define OBJ_GENOME 3
#define OBJ_PROV   4

#define OBJ_MAX_SIZE (64 * 1024)

/* Mount the store disk; formats it if it lacks the superblock magic.
 * Returns 0 on success. */
int  obj_mount(struct blkdev *d);

/* Store `len` bytes; out_hash gets the object's SHA-256 id. If the
 * content already exists nothing is written (dedup = immutability). */
int  obj_put(uint32_t type, const void *data, uint32_t len,
             uint8_t out_hash[SHA256_LEN]);

/* Read object by hash. Returns object length, -1 if absent,
 * -2 if buf too small. */
int  obj_get(const uint8_t hash[SHA256_LEN], void *buf, uint32_t maxlen);

int  obj_exists(const uint8_t hash[SHA256_LEN]);
uint32_t obj_count(void);

/* Iterate the index: i in [0, obj_count). Fills hash/len/type. */
int  obj_at(uint32_t i, uint8_t hash[SHA256_LEN], uint32_t *len,
            uint32_t *type);

/* put/get round-trip check, prints result on serial */
void obj_selftest(void);
