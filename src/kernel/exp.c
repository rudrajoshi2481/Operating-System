/*
 * exp.c — experiment transactions (Step 12).
 *
 * An experiment is a versioned OBJ_EXPERIMENT document. Each phase is
 * an immutable new object whose provenance record points at the prior
 * version — atomic (single store append), replayable (prov:// walk
 * shows every phase), and deduplicated.
 */
#include "exp.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"

int exp_new(const char *plan, uint8_t out[SHA256_LEN])
{
    char body[512];
    int n = ksnprintf(body, sizeof(body),
                      "exp v1\nstate planned\nplan %s\n", plan);
    if (obj_put(OBJ_EXPERIMENT, body, (uint32_t)n, out) != 0)
        return -1;
    prov_note(out, "exp-plan", "shell", 0, 0);
    return 0;
}

/* shared: load an experiment body, verify type */
static int exp_load(const uint8_t exp[SHA256_LEN], char *buf,
                    uint32_t cap)
{
    int n = obj_get(exp, buf, cap - 1);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    if (buf[0] != 'e' || buf[1] != 'x' || buf[2] != 'p')
        return -2;
    return n;
}

int exp_attach(const uint8_t exp[SHA256_LEN],
               const uint8_t obj[SHA256_LEN], const char *tag,
               uint8_t out[SHA256_LEN])
{
    char body[1024];
    int n = exp_load(exp, body, sizeof(body) - 128);
    if (n < 0)
        return n;

    /* rewrite state line to measured */
    char *st = 0;
    for (char *p = body; *p; p++)
        if (p[0] == 's' && p[1] == 't' && p[2] == 'a' && p[3] == 't') {
            st = p;
            break;
        }
    if (st) {
        char *eol = st;
        while (*eol && *eol != '\n')
            eol++;
        memmove(st + strlen("state measured"), eol,
                (uint32_t)(n - (eol - body)) + 1);
        memcpy(st, "state measured", strlen("state measured"));
    }
    n = strlen(body);

    char hex[SHA256_LEN * 2 + 1];
    sha256_hex(obj, hex);
    n += ksnprintf(body + n, sizeof(body) - n, "meas %s %s\n", hex, tag);

    if (obj_put(OBJ_EXPERIMENT, body, (uint32_t)n, out) != 0)
        return -3;
    uint8_t ins[2][SHA256_LEN];
    memcpy(ins[0], exp, SHA256_LEN);    /* previous version */
    memcpy(ins[1], obj, SHA256_LEN);    /* measurement added */
    prov_note(out, "exp-measure", "shell", ins, 2);
    return 0;
}

int exp_close(const uint8_t exp[SHA256_LEN], const char *verdict,
              uint8_t out[SHA256_LEN])
{
    char body[1024];
    int n = exp_load(exp, body, sizeof(body) - 256);
    if (n < 0)
        return n;

    char *st = 0;
    for (char *p = body; *p; p++)
        if (p[0] == 's' && p[1] == 't' && p[2] == 'a' && p[3] == 't') {
            st = p;
            break;
        }
    if (st) {
        char *eol = st;
        while (*eol && *eol != '\n')
            eol++;
        memmove(st + strlen("state analyzed"), eol,
                (uint32_t)(n - (eol - body)) + 1);
        memcpy(st, "state analyzed", strlen("state analyzed"));
    }
    n = strlen(body);
    n += ksnprintf(body + n, sizeof(body) - n, "verdict %s\n", verdict);

    if (obj_put(OBJ_EXPERIMENT, body, (uint32_t)n, out) != 0)
        return -3;
    uint8_t ins[1][SHA256_LEN];
    memcpy(ins[0], exp, SHA256_LEN);
    prov_note(out, "exp-analyze", "shell", ins, 1);
    return 0;
}
