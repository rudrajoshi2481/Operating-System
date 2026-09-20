#pragma once

#include <stdint.h>
#include "sha256.h"

/* Packed variant record — element layout of kind=variants arrays.
 * 16 bytes: chrom id (0..23 = chr1..22,X,Y; 255 = other), position,
 * ref/alt packed base bits, flags. */
struct varrec {
    uint32_t chrom;
    uint64_t pos;
    uint8_t  ref, alt;
    uint16_t flags;
};

/* codec.c — ingest codecs: external formats -> chunked store objects.
 * vcf_ingest parses VCF text into a chunked variants array (kind=8),
 * SNVs only; returns the manifest object hash. */
int codec_vcf(const void *data, uint32_t len, uint8_t out_hash[SHA256_LEN],
              uint32_t *nvars_out);
