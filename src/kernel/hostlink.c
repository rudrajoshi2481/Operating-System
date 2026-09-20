/*
 * hostlink.c — guest end of the host channel (virtio-console).
 *
 * Line protocol:
 *   <text>\n              -> logged on serial, replies "ok <text>\n"
 *   putfile <name> <len>\n<raw bytes>  -> obj_put(OBJ_RAW), "ok <hex>\n"
 *   putvcf <len>\n<raw bytes>          -> codec_vcf, "ok <hex>\n"
 *   prov <hex>\n          -> replies with lineage text
 *
 * The binary header is a line; payload follows raw. This is the pipe
 * the agent/UI speaks — every object entering the store from the host
 * gets a provenance record here.
 */
#include "hostlink.h"
#include "codec.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"
#include "sha256.h"
#include "virtio_console.h"

#define MAX_INGEST (256 * 1024)

static void reply_hex(const uint8_t h[SHA256_LEN])
{
    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(h, hex);
    con_write("ok ", 3);
    con_write(hex, SHA256_LEN * 2);
    con_write("\n", 1);
}

static void reply_err(void)
{
    con_write("err\n", 4);
}

/* read exactly len bytes (blocking) */
static int read_exact(uint8_t *buf, uint32_t len)
{
    uint32_t got = 0;
    while (got < len) {
        con_wait();
        int n = con_read(buf + got, len - got);
        if (n > 0)
            got += (uint32_t)n;
    }
    return 0;
}

static uint32_t parse_num(const char *s)
{
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (uint32_t)(*s++ - '0');
    return n;
}

/* handle "putfile <name> <len>" / "putvcf <len>" header line.
 * Consumes payload bytes; returns 1 if it was a putfile/putvcf. */
static int try_ingest(const char *line)
{
    uint32_t len;
    int isvcf = 0;

    if (memcmp(line, "putfile ", 8) == 0) {
        /* skip name token, then read len */
        const char *p = line + 8;
        while (*p && *p != ' ')
            p++;
        if (*p != ' ')
            return 0;
        len = parse_num(p + 1);
    } else if (memcmp(line, "putvcf ", 7) == 0) {
        len = parse_num(line + 7);
        isvcf = 1;
    } else {
        return 0;
    }
    if (!len || len > MAX_INGEST) {
        reply_err();
        return 1;
    }

    uint8_t *buf = kmalloc(len);
    if (!buf) {
        reply_err();
        return 1;
    }
    read_exact(buf, len);

    uint8_t h[SHA256_LEN];
    int rc;
    if (isvcf) {
        uint32_t nv = 0;
        rc = codec_vcf(buf, len, h, &nv);
    } else {
        rc = obj_put(OBJ_RAW, buf, len, h);
        if (rc == 0)
            prov_note(h, "ingest", "host", 0, 0);
    }
    kfree(buf);
    if (rc == 0)
        reply_hex(h);
    else
        reply_err();
    return 1;
}

void hostlink_main(void *arg)
{
    (void)arg;
    char line[96];
    int n = 0;

    for (;;) {
        con_wait();
        char c;
        while (con_read(&c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (n) {
                    line[n] = 0;
                    if (try_ingest(line)) {
                        n = 0;
                        continue;
                    }
                    kprint("[host] %s\n", line);
                    con_write("ok ", 3);
                    con_write(line, (uint32_t)n);
                    con_write("\n", 1);
                    n = 0;
                }
            } else if (n < (int)sizeof(line) - 1) {
                line[n++] = c;
            }
        }
    }
}
