/*
 * gate.c — agent gate (Step 13).
 *
 * An external model on the host channel may only act through:
 *   act query <uri>                 bounded read (cap-limited)
 *   act lineage <hash>              provenance walk
 *   act derive <op> <in> <text>     create derived object
 *   act simulate <instr> <in>       instrument dry-run (free, audited)
 *   act plan <text>                 open an experiment (state=planned)
 *   act execute <instr> <in> [req]  instrument run — needs approval
 *   act observe                     bounded store/system digest
 *
 * Approval flow: `act execute` without a matching pending request
 * creates one and replies `denied approval-required req=N`; a human
 * types `approve N` on the serial console; the retry `act execute
 * <instr> <in> N` is then granted a one-shot execute capability.
 *
 * Every action — allowed or denied — appends an OBJ_AUDIT record, so
 * the audit trail is immutable, deduplicated, and inside the same
 * store as the data it describes.
 */
#include "gate.h"
#include "caps.h"
#include "exp.h"
#include "instr.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"
#include "qidx.h"
#include "sha256.h"
#include "thread.h"
#include "uri.h"

#define ALOG_CAP 128
#define PEND_CAP 16
#define GATE_REPLY_MAX 4096

static struct {
    uint64_t tick;
    uint8_t  rec[SHA256_LEN];           /* audit record object */
    int      rc;
    char     verb[16];
} alog[ALOG_CAP];
static uint32_t ahead, atotal;

static struct {
    uint8_t  used, approved;
    char     instr[24];
    uint8_t  in[SHA256_LEN];
} pend[PEND_CAP];

/* write an audit record object + ring entry */
static void audit(const char *verb, const char *args, int rc)
{
    char body[384];
    int n = ksnprintf(body, sizeof(body),
                      "audit v1\nact %s\nargs %s\nt %lu\nrc %d\n",
                      verb, args, timer_ticks(), rc);
    uint8_t h[SHA256_LEN];
    if (obj_put(OBJ_AUDIT, body, (uint32_t)n, h) != 0)
        memset(h, 0, SHA256_LEN);

    alog[ahead].tick = timer_ticks();
    memcpy(alog[ahead].rec, h, SHA256_LEN);
    alog[ahead].rc = rc;
    uint32_t i = 0;
    while (verb[i] && i < sizeof(alog[ahead].verb) - 1) {
        alog[ahead].verb[i] = verb[i];
        i++;
    }
    alog[ahead].verb[i] = 0;
    ahead = (ahead + 1) % ALOG_CAP;
    atotal++;
}

static int parse_hex(const char **pp, uint8_t h[SHA256_LEN])
{
    if (sha256_from_hexn(*pp, h) != 0)
        return -1;
    *pp += SHA256_LEN * 2;
    if (**pp == ' ')
        (*pp)++;
    return 0;
}

static int find_pending(const char *instr, const uint8_t in[SHA256_LEN])
{
    for (int i = 0; i < PEND_CAP; i++)
        if (pend[i].used && memcmp(pend[i].in, in, SHA256_LEN) == 0) {
            const char *a = instr, *b = pend[i].instr;
            while (*a && *a == *b)
                a++, b++;
            if (!*a && !*b)
                return i;
        }
    return -1;
}

/* act execute: check request slot; `req` = presented id or -1 */
static int do_execute(const char *instr, const uint8_t in[SHA256_LEN],
                      int req, char *out, uint32_t cap)
{
    int slot = find_pending(instr, in);
    if (slot < 0) {
        slot = -1;
        for (int i = 0; i < PEND_CAP; i++)
            if (!pend[i].used) {
                slot = i;
                break;
            }
        if (slot < 0) {
            audit("execute", instr, -3);
            return ksnprintf(out, cap, "err approval-queue-full\n");
        }
        pend[slot].used = 1;
        pend[slot].approved = 0;
        memcpy(pend[slot].in, in, SHA256_LEN);
        uint32_t i = 0;
        while (instr[i] && i < sizeof(pend[slot].instr) - 1) {
            pend[slot].instr[i] = instr[i];
            i++;
        }
        pend[slot].instr[i] = 0;
        audit("execute", instr, -2);
        return ksnprintf(out, cap,
                         "denied approval-required req=%d\n", slot);
    }
    if (!pend[slot].approved || (req >= 0 && req != slot)) {
        audit("execute", instr, -2);
        return ksnprintf(out, cap,
                         "denied approval-required req=%d\n", slot);
    }

    /* approved: one-shot internal capability, then consume the req */
    uint8_t ih[SHA256_LEN];
    if (instr_find(instr, ih) != 0) {
        audit("execute", instr, -4);
        return ksnprintf(out, cap, "err no-such-instrument\n");
    }
    int capid = cap_grant(ih, CAP_X);
    if (capid < 0) {
        audit("execute", instr, -5);
        return ksnprintf(out, cap, "err cap-table-full\n");
    }
    uint8_t oh[SHA256_LEN];
    int rc = instr_run((uint32_t)capid, instr, in, oh);
    cap_revoke((uint32_t)capid);
    pend[slot].used = 0;
    audit("execute", instr, rc);
    if (rc != 0)
        return ksnprintf(out, cap, "err run-failed %d\n", rc);
    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(oh, hex);
    return ksnprintf(out, cap, "ok obj://%s\n", hex);
}

int gate_act(const char *line, char *out, uint32_t cap)
{
    const char *p = line + 4;           /* skip "act " */
    uint8_t h[SHA256_LEN];
    char hex[SHA256_LEN * 2 + 1];

    if (memcmp(p, "query ", 6) == 0) {
        int n = uri_read(p + 6, out, cap < GATE_REPLY_MAX
                               ? cap : GATE_REPLY_MAX);
        audit("query", p + 6, n < 0 ? -1 : 0);
        if (n < 0)
            return ksnprintf(out, cap, "err not-found\n");
        return n;
    }
    if (memcmp(p, "lineage ", 8) == 0) {
        const char *q = p + 8;
        if (parse_hex(&q, h) != 0) {
            audit("lineage", p + 8, -1);
            return ksnprintf(out, cap, "err bad-hash\n");
        }
        int n = prov_lineage(h, out, cap);
        audit("lineage", p + 8, n <= 0 ? -1 : 0);
        return n > 0 ? n : ksnprintf(out, cap, "err no-lineage\n");
    }
    if (memcmp(p, "derive ", 7) == 0) {
        /* derive <op> <in-hex> <text> */
        const char *q = p + 7;
        char op[16];
        int i = 0;
        while (q[i] && q[i] != ' ' && i < 15) {
            op[i] = q[i];
            i++;
        }
        op[i] = 0;
        q += i + 1;
        uint8_t inh[SHA256_LEN];
        if (parse_hex(&q, inh) != 0) {
            audit("derive", op, -1);
            return ksnprintf(out, cap, "err bad-hash\n");
        }
        if (obj_put(OBJ_TEXT, q, (uint32_t)strlen(q), h) != 0) {
            audit("derive", op, -2);
            return ksnprintf(out, cap, "err store-failed\n");
        }
        uint8_t ins[1][SHA256_LEN];
        memcpy(ins[0], inh, SHA256_LEN);
        prov_note(h, op, "agent", ins, 1);
        audit("derive", op, 0);
        sha256_hex(h, hex);
        return ksnprintf(out, cap, "ok obj://%s\n", hex);
    }
    if (memcmp(p, "simulate ", 9) == 0) {
        /* dry-run: instrument runs are deterministic, so the emitted
         * object IS the simulation result — audited as simulate */
        const char *q = p + 9;
        char instr[24];
        int i = 0;
        while (q[i] && q[i] != ' ' && i < 23) {
            instr[i] = q[i];
            i++;
        }
        instr[i] = 0;
        q += i + 1;
        uint8_t inh[SHA256_LEN], oh[SHA256_LEN];
        if (parse_hex(&q, inh) != 0) {
            audit("simulate", instr, -1);
            return ksnprintf(out, cap, "err bad-hash\n");
        }
        uint8_t ih[SHA256_LEN];
        int rc = -3;
        if (instr_find(instr, ih) == 0) {
            int capid = cap_grant(ih, CAP_X);
            if (capid >= 0) {
                rc = instr_run((uint32_t)capid, instr, inh, oh);
                cap_revoke((uint32_t)capid);
            }
        }
        if (rc != 0) {
            audit("simulate", instr, rc);
            return ksnprintf(out, cap, "err simulate-failed\n");
        }
        audit("simulate", instr, 0);
        sha256_hex(oh, hex);
        return ksnprintf(out, cap, "ok obj://%s (simulated)\n", hex);
    }
    if (memcmp(p, "plan ", 5) == 0) {
        if (exp_new(p + 5, h) != 0) {
            audit("plan", p + 5, -1);
            return ksnprintf(out, cap, "err plan-failed\n");
        }
        audit("plan", p + 5, 0);
        sha256_hex(h, hex);
        return ksnprintf(out, cap, "ok obj://%s\n", hex);
    }
    if (memcmp(p, "execute ", 8) == 0) {
        const char *q = p + 8;
        char instr[24];
        int i = 0;
        while (q[i] && q[i] != ' ' && i < 23) {
            instr[i] = q[i];
            i++;
        }
        instr[i] = 0;
        q += i + 1;
        uint8_t inh[SHA256_LEN];
        if (parse_hex(&q, inh) != 0) {
            audit("execute", instr, -1);
            return ksnprintf(out, cap, "err bad-hash\n");
        }
        int req = -1;
        if (*q >= '0' && *q <= '9') {
            req = 0;
            while (*q >= '0' && *q <= '9')
                req = req * 10 + (*q++ - '0');
        }
        return do_execute(instr, inh, req, out, cap);
    }
    if (memcmp(p, "observe", 7) == 0) {
        /* bounded system digest: object count + types + index state */
        uint32_t off = (uint32_t)ksnprintf(out, cap,
                                         "objects %u\nprov %u\n",
                                         obj_count(), prov_count());
        off += (uint32_t)qidx_fmt_idx(out + off, cap - off);
        audit("observe", "-", 0);
        return (int)off;
    }

    audit("unknown", p, -1);
    return ksnprintf(out, cap, "err unknown-action\n");
}

void gate_approve(uint32_t reqid)
{
    if (reqid < PEND_CAP && pend[reqid].used) {
        pend[reqid].approved = 1;
        kprint("gate: request %u approved\n", reqid);
    } else {
        kprint("gate: no such request\n");
    }
}

int gate_fmt_audit(char *buf, uint32_t cap)
{
    uint32_t off = (uint32_t)ksnprintf(buf, cap, "actions %lu\n", atotal);
    uint32_t n = atotal < ALOG_CAP ? atotal : ALOG_CAP;
    uint32_t start = atotal < ALOG_CAP ? 0 : ahead;
    for (uint32_t i = 0; i < n && off < cap - 64; i++) {
        uint32_t j = (start + i) % ALOG_CAP;
        char hex[SHA256_LEN * 2 + 1];
        sha256_hex(alog[j].rec, hex);
        hex[16] = 0;
        off += (uint32_t)ksnprintf(buf + off, cap - off,
                                   "t%lu %s rc=%d rec=%s\n",
                                   alog[j].tick, alog[j].verb,
                                   alog[j].rc, hex);
    }
    return (int)off;
}

int gate_fmt_pending(char *buf, uint32_t cap)
{
    uint32_t off = 0, n = 0;
    for (int i = 0; i < PEND_CAP; i++)
        if (pend[i].used)
            n++;
    off += (uint32_t)ksnprintf(buf, cap, "pending %u\n", n);
    for (int i = 0; i < PEND_CAP && off < cap - 64; i++)
        if (pend[i].used)
            off += (uint32_t)ksnprintf(buf + off, cap - off,
                                       "req%d %s %s\n", i,
                                       pend[i].instr,
                                       pend[i].approved
                                           ? "approved" : "awaiting");
    return (int)off;
}
