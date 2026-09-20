#include "shell.h"
#include "kprint.h"
#include "lib.h"
#include "pmm.h"
#include "proc.h"
#include "thread.h"
#include "uart.h"
#include "virtio_blk.h"
#include "virtio_console.h"
#include "objstore.h"
#include "prov.h"
#include "uri.h"
#include "sha256.h"

extern char _user_hello_start[];

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void run_cmd(char *cmd)
{
    if (eq(cmd, "help")) {
        kprint("commands: help ps free run blk host put get uri objects\n");
    } else if (cmd[0] == 'h' && cmd[1] == 'o' && cmd[2] == 's' &&
               cmd[3] == 't' && cmd[4] == ' ') {
        const char *msg = cmd + 5;
        uint32_t len = 0;
        while (msg[len])
            len++;
        con_write(msg, len);
        con_write("\n", 1);
    } else if (eq(cmd, "blk")) {
        uint8_t sec[512];
        struct blkdev *d = blk_esp();
        if (d && blk_read(d, 0, sec, 512) == 0) {
            kprint("capacity: %lu sectors\n", d->capacity);
            for (int i = 0; i < 64; i++) {
                kprint("%x ", sec[i]);
                if (i % 16 == 15)
                    kprint("\n");
            }
        } else {
            kprint("blk: read failed\n");
        }
    } else if (eq(cmd, "ps")) {
        sched_dump();
    } else if (eq(cmd, "free")) {
        kprint("free: %lu frames (%lu MiB)\n", pmm_free_frames(),
               pmm_free_frames() * 4096 / (1024 * 1024));
    } else if (eq(cmd, "run")) {
        proc_exec(_user_hello_start);
    } else if (cmd[0] == 'p' && cmd[1] == 'u' && cmd[2] == 't' &&
               cmd[3] == ' ') {
        const char *msg = cmd + 4;
        uint32_t len = 0;
        while (msg[len])
            len++;
        uint8_t h[SHA256_LEN];
        if (obj_put(OBJ_TEXT, msg, len, h) == 0) {
            prov_note(h, "put", "shell", 0, 0);
            char hex[SHA256_LEN * 2 + 1];
            sha256_hex(h, hex);
            kprint("stored: obj://%s\n", hex);
        } else {
            kprint("put: failed\n");
        }
    } else if (cmd[0] == 'g' && cmd[1] == 'e' && cmd[2] == 't' &&
               cmd[3] == ' ') {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(cmd + 4, h) == 0) {
            uint8_t buf[1024];
            int n = obj_get(h, buf, sizeof(buf) - 1);
            if (n >= 0) {
                buf[n] = 0;
                kprint("%s\n", buf);
            } else {
                kprint("get: not found\n");
            }
        } else {
            kprint("get: bad hash\n");
        }
    } else if (cmd[0] == 'u' && cmd[1] == 'r' && cmd[2] == 'i' &&
               cmd[3] == ' ') {
        uint8_t buf[2048];
        int n = uri_read(cmd + 4, buf, sizeof(buf) - 1);
        if (n >= 0) {
            buf[n] = 0;
            kprint("%s", buf);
            if (n == 0 || buf[n - 1] != '\n')
                kprint("\n");
        } else {
            kprint("uri: not found\n");
        }
    } else if (cmd[0] == 'd' && cmd[1] == 'e' && cmd[2] == 'r' &&
               cmd[3] == 'i' && cmd[4] == 'v' && cmd[5] == 'e' &&
               cmd[6] == ' ') {
        /* derive <op> <in-hex> <text> — create object with lineage */
        char *rest = cmd + 7;
        char *sp = 0;
        for (char *p = rest; *p; p++)
            if (*p == ' ') { sp = p; break; }
        uint8_t inh[SHA256_LEN];
        char hexbuf[SHA256_LEN * 2 + 1];
        int ok = 0;
        if (sp && sp[65] == ' ') {
            memcpy(hexbuf, sp + 1, SHA256_LEN * 2);
            hexbuf[SHA256_LEN * 2] = 0;
            ok = sha256_from_hex(hexbuf, inh) == 0;
        }
        if (ok) {
            char op[16];
            uint32_t olen = (uint32_t)(sp - rest);
            if (olen >= sizeof(op))
                olen = sizeof(op) - 1;
            memcpy(op, rest, olen);
            op[olen] = 0;
            const char *msg = sp + 66;
            uint32_t mlen = 0;
            while (msg[mlen])
                mlen++;
            uint8_t h[SHA256_LEN];
            uint8_t ins[1][SHA256_LEN];
            memcpy(ins[0], inh, SHA256_LEN);
            if (obj_put(OBJ_TEXT, msg, mlen, h) == 0) {
                prov_note(h, op, "shell", ins, 1);
                char hex[SHA256_LEN * 2 + 1];
                sha256_hex(h, hex);
                kprint("derived: obj://%s\n", hex);
            } else {
                kprint("derive: store failed\n");
            }
        } else {
            kprint("usage: derive <op> <in-hex> <text>\n");
        }
    } else if (cmd[0] == 'l' && cmd[1] == 'i' && cmd[2] == 'n' &&
               cmd[3] == 'e' && cmd[4] == 'a' && cmd[5] == 'g' &&
               cmd[6] == 'e' && cmd[7] == ' ') {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(cmd + 8, h) == 0) {
            static char buf[2048];
            int n = prov_lineage(h, buf, sizeof(buf) - 1);
            if (n > 0)
                kprint("%s", buf);
            else
                kprint("no lineage\n");
        } else {
            kprint("usage: lineage <hash>\n");
        }
    } else if (eq(cmd, "objects")) {
        uint8_t buf[2048];
        int n = uri_read("sys://objects", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            kprint("%s", buf);
        }
    } else if (cmd[0]) {
        kprint("unknown: %s\n", cmd);
    }
}

void shell_main(void *arg)
{
    (void)arg;
    char line[192];
    int n = 0;

    kprint("\nbioos> ");
    for (;;) {
        int c = uart_getc();
        if (c < 0) {
            yield();
            continue;
        }
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            line[n] = 0;
            run_cmd(line);
            n = 0;
            kprint("bioos> ");
        } else if ((c == 0x7f || c == '\b') && n > 0) {
            n--;
            uart_write("\b \b");
        } else if (n < (int)sizeof(line) - 1 && c >= 32 && c < 127) {
            line[n++] = (char)c;
            uart_putc((char)c);
        }
    }
}
