/* caps.c — capability table (Step 12). Ids are indices into a kernel
 * table, so user/agent code cannot fabricate rights; revocable. */
#include "caps.h"
#include "kprint.h"
#include "lib.h"
#include "sha256.h"

#define CAP_MAX 64

static struct {
    uint8_t  used;
    uint8_t  obj[SHA256_LEN];
    uint32_t rights;
} caps[CAP_MAX];

int cap_grant(const uint8_t obj[SHA256_LEN], uint32_t rights)
{
    for (uint32_t i = 0; i < CAP_MAX; i++)
        if (!caps[i].used) {
            caps[i].used = 1;
            memcpy(caps[i].obj, obj, SHA256_LEN);
            caps[i].rights = rights;
            return (int)i;
        }
    return -1;
}

int cap_check(uint32_t id, const uint8_t obj[SHA256_LEN],
              uint32_t right)
{
    if (id >= CAP_MAX || !caps[id].used)
        return -1;
    if (!(caps[id].rights & right))
        return -1;
    return memcmp(caps[id].obj, obj, SHA256_LEN) == 0 ? 0 : -1;
}

void cap_revoke(uint32_t id)
{
    if (id < CAP_MAX)
        caps[id].used = 0;
}

int cap_fmt(char *buf, uint32_t cap)
{
    uint32_t off = 0, n = 0;
    for (uint32_t i = 0; i < CAP_MAX; i++)
        if (caps[i].used)
            n++;
    off += (uint32_t)ksnprintf(buf, cap, "caps %u\n", n);
    for (uint32_t i = 0; i < CAP_MAX && off < cap - 96; i++) {
        if (!caps[i].used)
            continue;
        char hex[SHA256_LEN * 2 + 1];
        sha256_hex(caps[i].obj, hex);
        hex[16] = 0;
        off += (uint32_t)ksnprintf(buf + off, cap - off,
                                   "cap%u %s %c%c%c\n", i, hex,
                                   caps[i].rights & CAP_R ? 'r' : '-',
                                   caps[i].rights & CAP_W ? 'w' : '-',
                                   caps[i].rights & CAP_X ? 'x' : '-');
    }
    return (int)off;
}
