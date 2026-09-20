#pragma once

#include <stdint.h>
#include "sha256.h"

/* exp.c — experiment transactions (Step 12). An experiment is an
 * immutable, versioned object: each state transition stores a NEW
 * object provenance-linked to the previous version, so the full
 * history is replayable through prov:// and nothing is ever
 * overwritten (atomic commit = object store append).
 *
 *   exp_new(plan)            -> state=planned
 *   exp_attach(exp,obj,tag)  -> +meas <hash> <tag>, state=measured
 *   exp_close(exp,verdict)   -> state=analyzed, verdict text          */
int exp_new(const char *plan, uint8_t out[SHA256_LEN]);
int exp_attach(const uint8_t exp[SHA256_LEN],
               const uint8_t obj[SHA256_LEN], const char *tag,
               uint8_t out[SHA256_LEN]);
int exp_close(const uint8_t exp[SHA256_LEN], const char *verdict,
              uint8_t out[SHA256_LEN]);
