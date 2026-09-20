#include "uri.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "pmm.h"
#include "sha256.h"
#include "thread.h"
#include "timer.h"

static int scheme_is(const char *uri, const char *scheme)
{
    while (*scheme)
        if (*uri++ != *scheme++)
            return 0;
    return 1;
}

static int sys_mem(char *b, uint32_t cap)
{
    return ksnprintf(b, cap, "free_frames %lu\nfree_mib %lu\n",
                     pmm_free_frames(),
                     pmm_free_frames() * 4096 / (1024 * 1024));
}

static int sys_objects(char *b, uint32_t cap)
{
    uint32_t off = (uint32_t)ksnprintf(b, cap, "count %u\n", obj_count());
    for (uint32_t i = 0; i < obj_count() && off < cap; i++) {
        uint8_t h[SHA256_LEN];
        uint32_t len, type;
        char hex[SHA256_LEN * 2 + 1];
        if (obj_at(i, h, &len, &type) != 0)
            break;
        sha256_hex(h, hex);
        off += (uint32_t)ksnprintf(b + off, cap - off,
                                   "%s type=%u len=%u\n", hex, type, len);
    }
    return (int)off;
}

int uri_read(const char *uri, void *buf, uint32_t cap)
{
    char *b = buf;

    if (scheme_is(uri, "sys://")) {
        const char *name = uri + 6;
        if (scheme_is(name, "proc"))
            return sched_fmt(b, cap);
        if (scheme_is(name, "mem"))
            return sys_mem(b, cap);
        if (scheme_is(name, "objects"))
            return sys_objects(b, cap);
        if (scheme_is(name, "uptime"))
            return ksnprintf(b, cap, "ticks %lu\n", timer_ticks());
        return -1;
    }

    if (scheme_is(uri, "obj://")) {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(uri + 6, h) != 0)
            return -1;
        return obj_get(h, buf, cap);
    }

    return -1;
}
