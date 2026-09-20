#pragma once

#include <stdint.h>
#include "sha256.h"

/* caps.c — capability table. A capability is an unforgeable kernel
 * handle: { id -> object hash, rights }. Shell and the agent gate hold
 * ids; the kernel checks rights on every gated operation. */
#define CAP_R 1
#define CAP_W 2
#define CAP_X 4

int  cap_grant(const uint8_t obj[SHA256_LEN], uint32_t rights);
int  cap_check(uint32_t id, const uint8_t obj[SHA256_LEN],
               uint32_t right);
void cap_revoke(uint32_t id);
int  cap_fmt(char *buf, uint32_t cap);          /* sys://caps */
