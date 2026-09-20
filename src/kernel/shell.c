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
#include "array.h"
#include "caps.h"
#include "exp.h"
#include "gate.h"
#include "instr.h"

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
        kprint("          derive lineage mkarr arrget\n");
        kprint("          mksample mkproto instr cap irun exp\n");
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
    } else if (cmd[0] == 'm' && cmd[1] == 'k' && cmd[2] == 'a' &&
               cmd[3] == 'r' && cmd[4] == 'r' && cmd[5] == ' ') {
        /* mkarr <kind> <codec> <shape> <chunk> <text>  (1-D, dtype u8)
         * e.g. mkarr genome dna2 64 16 ACGTACGT... */
        char kind[16] = {0}, codec[8] = {0};
        uint64_t shape = 0, cshape = 0;
        const char *p = cmd + 6;
        int ti = 0;
        while (p[ti] && p[ti] != ' ' && ti < 15) { kind[ti] = p[ti]; ti++; }
        p += ti + 1;
        ti = 0;
        while (p[ti] && p[ti] != ' ' && ti < 7) { codec[ti] = p[ti]; ti++; }
        p += ti + 1;
        while (*p >= '0' && *p <= '9')
            shape = shape * 10 + (uint64_t)(*p++ - '0');
        p++;
        while (*p >= '0' && *p <= '9')
            cshape = cshape * 10 + (uint64_t)(*p++ - '0');
        p++;
        uint32_t mlen = 0;
        while (p[mlen])
            mlen++;
        if (mlen < shape) {
            kprint("mkarr: need %lu bytes of data\n", shape);
            return;
        }
        uint32_t kc = kind[0] == 'g' ? ARR_KIND_GENOME
                    : kind[0] == 'e' ? ARR_KIND_EMBED
                    : kind[0] == 'v' ? ARR_KIND_VARIANTS : ARR_KIND_RAW;
        uint8_t h[SHA256_LEN];
        if (arr_put(kc, "u8", codec, 1, &shape, &cshape, p, mlen, h) == 0) {
            prov_note(h, "mkarr", "shell", 0, 0);
            char hex[SHA256_LEN * 2 + 1];
            sha256_hex(h, hex);
            kprint("array: obj://%s\n", hex);
        } else {
            kprint("mkarr: failed\n");
        }
    } else if (cmd[0] == 'a' && cmd[1] == 'r' && cmd[2] == 'r' &&
               cmd[3] == 'g' && cmd[4] == 'e' && cmd[5] == 't' &&
               cmd[6] == ' ') {
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(cmd + 7, h) != 0) {
            kprint("usage: arrget <hash>\n");
            return;
        }
        static uint8_t buf[4096];
        int n = arr_read(h, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            kprint("%u elems\n%s\n", n, buf);
        } else {
            kprint("arrget: not an array or too big\n");
        }
    } else if (memcmp(cmd, "mksample ", 9) == 0 ||
               memcmp(cmd, "mkproto ", 8) == 0) {
        /* mksample <text> / mkproto <text> — typed objects */
        int issample = cmd[2] == 's';
        uint32_t type = issample ? OBJ_SAMPLE : OBJ_PROTOCOL;
        const char *kind = issample ? "sample" : "protocol";
        const char *text = cmd + (issample ? 9 : 8);
        char body[256];
        int n = ksnprintf(body, sizeof(body), "%s v1\nnote %s\n",
                          kind, text);
        uint8_t h[SHA256_LEN];
        if (obj_put(type, body, (uint32_t)n, h) == 0) {
            prov_note(h, kind, "shell", 0, 0);
            char hex[SHA256_LEN * 2 + 1];
            sha256_hex(h, hex);
            kprint("%s: obj://%s\n", kind, hex);
        } else {
            kprint("%s: failed\n", kind);
        }
    } else if (eq(cmd, "instr")) {
        uint8_t buf[1024];
        int n = uri_read("sys://instr", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            kprint("%s", buf);
        }
    } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 'p' &&
               cmd[3] == ' ') {
        /* cap <hash> — grant rwx capability, prints cap id */
        uint8_t h[SHA256_LEN];
        if (sha256_from_hex(cmd + 4, h) != 0) {
            kprint("usage: cap <hash>\n");
            return;
        }
        int id = cap_grant(h, CAP_R | CAP_W | CAP_X);
        if (id >= 0)
            kprint("cap %u\n", id);
        else
            kprint("cap: table full\n");
    } else if (cmd[0] == 'i' && cmd[1] == 'r' && cmd[2] == 'u' &&
               cmd[3] == 'n' && cmd[4] == ' ') {
        /* irun <capid> <instr> <in-hash> — gated instrument run */
        const char *p = cmd + 5;
        uint32_t capid = 0;
        while (*p >= '0' && *p <= '9')
            capid = capid * 10 + (uint32_t)(*p++ - '0');
        if (*p++ != ' ') {
            kprint("usage: irun <capid> <instr> <in-hash>\n");
            return;
        }
        char name[24];
        int ti = 0;
        while (p[ti] && p[ti] != ' ' && ti < 23) { name[ti] = p[ti]; ti++; }
        name[ti] = 0;
        p += ti;
        if (*p++ != ' ') {
            kprint("usage: irun <capid> <instr> <in-hash>\n");
            return;
        }
        uint8_t inh[SHA256_LEN], outh[SHA256_LEN];
        if (sha256_from_hex(p, inh) != 0) {
            kprint("irun: bad input hash\n");
            return;
        }
        int rc = instr_run(capid, name, inh, outh);
        if (rc == 0) {
            char hex[SHA256_LEN * 2 + 1];
            sha256_hex(outh, hex);
            kprint("%s emitted obj://%s\n", name, hex);
        } else if (rc == -2) {
            kprint("irun: capability denied\n");
        } else {
            kprint("irun: failed (%d)\n", rc);
        }
    } else if (cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'p' &&
               cmd[3] == ' ') {
        /* exp new <plan> | exp add <exp> <obj> | exp close <exp> <v> */
        const char *p = cmd + 4;
        if (p[0] == 'n' && p[1] == 'e' && p[2] == 'w' && p[3] == ' ') {
            uint8_t h[SHA256_LEN];
            if (exp_new(p + 4, h) == 0) {
                char hex[SHA256_LEN * 2 + 1];
                sha256_hex(h, hex);
                kprint("experiment: obj://%s\n", hex);
            } else {
                kprint("exp: failed\n");
            }
        } else if (p[0] == 'a' && p[1] == 'd' && p[2] == 'd' &&
                   p[3] == ' ') {
            p += 4;
            uint8_t eh[SHA256_LEN], oh[SHA256_LEN];
            if (sha256_from_hexn(p, eh) != 0 || p[64] != ' ' ||
                sha256_from_hex(p + 65, oh) != 0) {
                kprint("usage: exp add <exp> <obj>\n");
                return;
            }
            uint8_t nh[SHA256_LEN];
            if (exp_attach(eh, oh, "meas", nh) == 0) {
                char hex[SHA256_LEN * 2 + 1];
                sha256_hex(nh, hex);
                kprint("experiment v2: obj://%s\n", hex);
            } else {
                kprint("exp add: failed\n");
            }
        } else if (p[0] == 'c' && p[1] == 'l' && p[2] == 'o' &&
                   p[3] == 's' && p[4] == 'e' && p[5] == ' ') {
            p += 6;
            uint8_t eh[SHA256_LEN];
            if (sha256_from_hexn(p, eh) != 0 || p[64] != ' ') {
                kprint("usage: exp close <exp> <verdict>\n");
                return;
            }
            uint8_t nh[SHA256_LEN];
            if (exp_close(eh, p + 65, nh) == 0) {
                char hex[SHA256_LEN * 2 + 1];
                sha256_hex(nh, hex);
                kprint("experiment closed: obj://%s\n", hex);
            } else {
                kprint("exp close: failed\n");
            }
        } else {
            kprint("usage: exp new|add|close ...\n");
        }
    } else if (memcmp(cmd, "approve ", 8) == 0) {
        /* approve <reqid> — grant a pending agent execute request */
        uint32_t id = 0;
        const char *p = cmd + 8;
        while (*p >= '0' && *p <= '9')
            id = id * 10 + (uint32_t)(*p++ - '0');
        gate_approve(id);
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
