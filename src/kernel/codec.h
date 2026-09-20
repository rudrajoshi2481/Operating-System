#pragma once

#include <stdint.h>
#include "sha256.h"

/* codec.c — ingest codecs: external formats -> chunked store objects.
 * vcf_ingest parses VCF text into a chunked variants array (kind=8),
 * SNVs only; returns the manifest object hash. */
int codec_vcf(const void *data, uint32_t len, uint8_t out_hash[SHA256_LEN],
              uint32_t *nvars_out);
