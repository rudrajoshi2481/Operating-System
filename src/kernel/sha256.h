#pragma once

#include <stdint.h>
#include <stddef.h>

#define SHA256_LEN 32

struct sha256_ctx {
    uint32_t h[8];
    uint64_t bits;              /* total message bits */
    uint8_t  buf[64];           /* partial block */
    uint32_t buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_LEN]);

/* one-shot */
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);

/* lowercase hex, no separators; hex must hold 2*SHA256_LEN+1 */
void sha256_hex(const uint8_t h[SHA256_LEN], char *hex);
int  sha256_from_hex(const char *hex, uint8_t h[SHA256_LEN]); /* 0=ok */
