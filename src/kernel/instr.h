#pragma once

#include <stdint.h>
#include "sha256.h"

/* instr.c — simulated instruments (Step 12). Instruments are typed
 * objects (OBJ_INSTRUMENT) registered at boot; running one requires an
 * X capability on its object. Output data is deterministic pseudo-data
 * derived from the input hash, so runs are reproducible. Every run
 * records provenance. */
void instr_init(void);

/* Look up an instrument object by name. 0=ok */
int  instr_find(const char *name, uint8_t hash[SHA256_LEN]);

/* Run `name` on input object `in` -> new output object `out`.
 * capid must hold CAP_X on the instrument's object. 0=ok */
int  instr_run(uint32_t capid, const char *name,
               const uint8_t in[SHA256_LEN], uint8_t out[SHA256_LEN]);

int  instr_fmt(char *buf, uint32_t cap);        /* sys://instr */
