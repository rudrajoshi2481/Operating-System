#pragma once

#include <stdint.h>
#include "sha256.h"

/* prov.c — provenance engine. Every derived/created object can carry a
 * lineage record; records are themselves content-addressed objects
 * (OBJ_PROV, text format) so lineage survives reboots and is itself
 * deduplicated by the store. */
void prov_note(const uint8_t out[SHA256_LEN], const char *op,
               const char *by, const uint8_t ins[][SHA256_LEN],
               uint32_t nins);

/* Full ancestor graph of `out` as text. Returns length or <0. */
int  prov_lineage(const uint8_t out[SHA256_LEN], char *buf, uint32_t cap);

/* Rebuild the out->record map by scanning OBJ_PROV objects. Call once
 * after obj_mount. */
void prov_rebuild(void);
uint32_t prov_count(void);
