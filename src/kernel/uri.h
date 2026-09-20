#pragma once

#include <stdint.h>

/*
 * uri.c — URI facade over the object store + introspection namespace.
 *
 *   obj://<64-hex-sha256>   -> stored object bytes
 *   sys://proc              -> thread table (text)
 *   sys://mem               -> PMM/heap stats (text)
 *   sys://objects           -> object index listing (text)
 *   sys://uptime            -> timer ticks (text)
 *
 * Everything the agent reads — data or kernel state — goes through
 * this one read path, so a single gate/audit point exists.
 */
int uri_read(const char *uri, void *buf, uint32_t cap);
