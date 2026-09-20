#pragma once

#include <stdint.h>
#include "sha256.h"

/* qidx.c — coordinate query engine (Step 11).
 *
 * Semantic coordinate queries translate to physical chunk ranges:
 *   seq://<arr>/<lo>-<hi>           element range of a 1-D array
 *   var://<arr>/<chr>:<lo>-<hi>     position query on a variants array
 *
 * Every coordinate query is workload-logged (sys://qlog). A learned
 * index materializes a record-span per hot chromosome once it crosses
 * the query threshold — subsequent queries scan only that span.
 * sys://qidx exposes the index state; qidx_bench compares scans. */

#define QIDX_HOT_AFTER 2                /* queries before index build */

/* Log a coordinate query into the workload ring. */
void qidx_log(uint32_t kind, uint32_t chrom, uint64_t lo, uint64_t hi);

/* Variant range query; emits matching records as text lines into out.
 * Returns bytes written or <0. `*touched` = records scanned. */
int  qidx_variants(const uint8_t hash[SHA256_LEN], uint32_t chrom,
                   uint64_t lo, uint64_t hi, char *out, uint32_t cap,
                   uint32_t *touched);

/* sys://qlog and sys://qidx text. */
int  qidx_fmt_log(char *buf, uint32_t cap);
int  qidx_fmt_idx(char *buf, uint32_t cap);
