#include "../../src/frontend/amiga/amiga_meta.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *message)
{
    if (condition) return 0;
    fprintf(stderr, "Amiga metadata: %s\n", message);
    return 1;
}

int main(void)
{
    static const unsigned char valid[] = {0x40, 0x23, 0x12, 0x01, 3, 'A', 0xe9, 'Z'};
    static const unsigned char empty[] = {0x40, 0, 0, 0, 0};
    static const unsigned char embedded_nul[] = {0x40, 0, 0, 0, 3, 'A', 0, 'B'};
    static const unsigned char short_payload[] = {0x40, 0, 0, 0};
    static const unsigned char bad_length[] = {0x40, 0, 0, 0, 2, 'A'};
    unsigned long protection = 0;
    char comment[80];
    int failed = 0;

    memset(comment, 0x7f, sizeof(comment));
    failed += check(bp_amiga_meta_decode(valid, sizeof(valid),
                                         &protection, comment) == 0,
                    "valid Latin-1 comment was rejected");
    failed += check(protection == 0x01122340UL,
                    "protection bits decoded incorrectly");
    failed += check((unsigned char)comment[0] == 'A' &&
                    (unsigned char)comment[1] == 0xe9 && comment[2] == 'Z' &&
                    comment[3] == '\0', "comment bytes were not preserved");

    failed += check(bp_amiga_meta_decode(empty, sizeof(empty),
                                         &protection, comment) == 0 &&
                    comment[0] == '\0', "empty comment was rejected");

    memset(comment, 0x7f, sizeof(comment));
    protection = 0xdeadbeefUL;
    failed += check(bp_amiga_meta_decode(embedded_nul, sizeof(embedded_nul),
                                         &protection, comment) != 0,
                    "embedded NUL in declared comment was accepted");
    failed += check(protection == 0xdeadbeefUL && comment[0] == 0x7f,
                    "rejected payload modified output values");
    failed += check(bp_amiga_meta_decode(short_payload, sizeof(short_payload),
                                         &protection, comment) != 0,
                    "short metadata payload was accepted");
    failed += check(bp_amiga_meta_decode(bad_length, sizeof(bad_length),
                                         &protection, comment) != 0,
                    "mismatched comment length was accepted");
    failed += check(bp_amiga_meta_decode(valid, 85, &protection, comment) != 0,
                    "oversized metadata payload was accepted");

    printf("Amiga native metadata decoder: %s\n",
           failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
