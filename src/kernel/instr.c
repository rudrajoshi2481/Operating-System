/*
 * instr.c — simulated instruments (Step 12).
 *
 * sequencer      -> emits a 256-base genome array (dna2 chunks)
 * thermocycler   -> amplifies: repeats input bytes x4 as a genome array
 * platereader    -> emits 96 u16 milli-OD absorbance values
 *
 * All outputs are deterministic functions of the input object hash —
 * simulated, but reproducible, which is what provenance needs.
 */
#include "instr.h"
#include "array.h"
#include "caps.h"
#include "heap.h"
#include "kprint.h"
#include "lib.h"
#include "objstore.h"
#include "prov.h"
#include "sha256.h"

#define NINSTR 3

static uint8_t instr_hash[NINSTR][SHA256_LEN];
static const char *instr_name[NINSTR] = {
    "sequencer", "thermocycler", "platereader"
};

static uint64_t seed_from(const uint8_t h[SHA256_LEN])
{
    uint64_t s = 0;
    for (int i = 0; i < 8; i++)
        s = (s << 8) | h[i];
    return s ? s : 0x9e3779b97f4a7c15ull;
}

static uint64_t xorshift(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* ---- instrument bodies -------------------------------------------------- */

static int run_sequencer(const uint8_t in[SHA256_LEN],
                         uint8_t out[SHA256_LEN])
{
    uint64_t s = seed_from(in);
    uint8_t bases[256];
    static const char b[] = "ACGT";
    for (int i = 0; i < 256; i++)
        bases[i] = (uint8_t)b[xorshift(&s) & 3];
    uint64_t shape = 256, cs = 64;
    return arr_put(ARR_KIND_GENOME, "u8", "dna2", 1, &shape, &cs,
                   bases, sizeof(bases), out);
}

static int run_thermocycler(const uint8_t in[SHA256_LEN],
                            uint8_t out[SHA256_LEN])
{
    /* amplify: 4 tandem repeats of the input object's first 64 bytes */
    uint8_t src[64];
    int n = obj_get(in, src, sizeof(src));
    if (n <= 0)
        return -1;
    uint8_t amp[256];
    for (int i = 0; i < 256; i++)
        amp[i] = src[i % n];
    uint64_t shape = 256, cs = 64;
    return arr_put(ARR_KIND_GENOME, "u8", "dna2", 1, &shape, &cs,
                   amp, sizeof(amp), out);
}

static int run_platereader(const uint8_t in[SHA256_LEN],
                           uint8_t out[SHA256_LEN])
{
    /* 96-well absorbance in milli-OD, dtype u16 */
    uint64_t s = seed_from(in);
    uint16_t wells[96];
    for (int i = 0; i < 96; i++)
        wells[i] = (uint16_t)(xorshift(&s) % 2500);
    uint64_t shape = 96, cs = 32;
    return arr_put(ARR_KIND_RAW, "u16", "raw", 1, &shape, &cs,
                   wells, sizeof(wells), out);
}

static int (*runs[NINSTR])(const uint8_t *, uint8_t *) = {
    run_sequencer, run_thermocycler, run_platereader
};

/* ---- registry ----------------------------------------------------------- */

void instr_init(void)
{
    for (int i = 0; i < NINSTR; i++) {
        char body[128];
        int n = ksnprintf(body, sizeof(body),
                          "instr v1\nname %s\nops run\n",
                          instr_name[i]);
        if (obj_put(OBJ_INSTRUMENT, body, (uint32_t)n,
                    instr_hash[i]) == 0)
            prov_note(instr_hash[i], "register-instr", "kernel", 0, 0);
    }
    kprint("instr: %u instruments registered\n", NINSTR);
}

int instr_find(const char *name, uint8_t hash[SHA256_LEN])
{
    for (int i = 0; i < NINSTR; i++) {
        const char *n = instr_name[i];
        int j = 0;
        while (n[j] && n[j] == name[j])
            j++;
        if (!n[j] && !name[j]) {
            memcpy(hash, instr_hash[i], SHA256_LEN);
            return 0;
        }
    }
    return -1;
}

int instr_run(uint32_t capid, const char *name,
              const uint8_t in[SHA256_LEN], uint8_t out[SHA256_LEN])
{
    int idx = -1;
    for (int i = 0; i < NINSTR; i++) {
        const char *n = instr_name[i];
        int j = 0;
        while (n[j] && n[j] == name[j])
            j++;
        if (!n[j] && !name[j])
            idx = i;
    }
    if (idx < 0)
        return -1;
    if (cap_check(capid, instr_hash[idx], CAP_X) != 0)
        return -2;                      /* capability denied */

    if (runs[idx](in, out) != 0)
        return -3;
    uint8_t ins[2][SHA256_LEN];
    memcpy(ins[0], in, SHA256_LEN);
    memcpy(ins[1], instr_hash[idx], SHA256_LEN);
    prov_note(out, instr_name[idx], "instrument", ins, 2);
    return 0;
}

int instr_fmt(char *buf, uint32_t cap)
{
    uint32_t off = 0;
    for (int i = 0; i < NINSTR && off < cap - 100; i++) {
        char hex[SHA256_LEN * 2 + 1];
        sha256_hex(instr_hash[i], hex);
        off += (uint32_t)ksnprintf(buf + off, cap - off,
                                   "%s obj://%s\n", instr_name[i], hex);
    }
    return (int)off;
}
