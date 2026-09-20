/* sha_test.c — SHA-256 known-answer vectors (FIPS 180-4 examples). */
#include <stdio.h>
#include <string.h>
#include "sha256.h"

static int fails;

static void check(const char *msg, const char *want_hex)
{
    uint8_t h[SHA256_LEN];
    char hex[SHA256_LEN * 2 + 1];
    sha256(msg, strlen(msg), h);
    sha256_hex(h, hex);
    if (strcmp(hex, want_hex)) {
        printf("sha256(\"%s\"):\n  got  %s\n  want %s\n", msg, hex, want_hex);
        fails++;
    }
    /* hex round-trip */
    uint8_t h2[SHA256_LEN];
    if (sha256_from_hex(hex, h2) != 0 || memcmp(h, h2, SHA256_LEN)) {
        printf("hex round-trip failed for \"%s\"\n", msg);
        fails++;
    }
}

int main(void)
{
    check("",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check("hello world",
          "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");

    /* streaming: same input split across update boundaries */
    struct sha256_ctx c;
    uint8_t h[SHA256_LEN];
    char hex[SHA256_LEN * 2 + 1];
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&c);
    sha256_update(&c, m, 20);
    sha256_update(&c, m + 20, strlen(m) - 20);
    sha256_final(&c, h);
    sha256_hex(h, hex);
    if (strcmp(hex,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")) {
        printf("streaming sha256 mismatch\n");
        fails++;
    }

    if (fails)
        return 1;
    printf("sha_test: PASS\n");
    return 0;
}
