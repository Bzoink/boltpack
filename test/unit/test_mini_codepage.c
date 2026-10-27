/* Verify mini builds compile and use only the target's configured codepage. */
#include <stdio.h>
#include <string.h>
#include "bp_codepage.h"

int main(void)
{
    char utf8[16], local[16], folded_lower[16], folded_upper[16];
    const char *expected;
    const char *filename_utf8;
    const char *filename_local;
    unsigned char expected_byte;
    char lower_case[2], upper_case[2];
#if BP_MINI_CODEPAGE == BP_CP_850
    expected = "\xC3\xB8"; /* CP850 0x9B is U+00F8. */
    expected_byte = 0x9B;
    filename_utf8 = "caf\xC3\xB8.txt";
    filename_local = "caf\x9B.txt";
    lower_case[0] = (char)0x9B; /* ø */
    upper_case[0] = (char)0x9D; /* Ø */
#elif BP_MINI_CODEPAGE == BP_CP_437
    expected = "\xC2\xA2"; /* CP437 0x9B is U+00A2. */
    expected_byte = 0x9B;
    filename_utf8 = "caf\xC3\xA9.txt";
    filename_local = "caf\x82.txt";
    lower_case[0] = (char)0x82; /* é */
    upper_case[0] = (char)0x90; /* É */
#else
#error test_mini_codepage supports only the configured CP437 and CP850 fixtures
#endif
    lower_case[1] = upper_case[1] = '\0';
    local[0] = (char)expected_byte;
    local[1] = '\0';
    if (bp_local_to_utf8(local, utf8, sizeof(utf8), BP_MINI_CODEPAGE) != 0 ||
        strcmp(utf8, expected) != 0) return 1;
    if (bp_utf8_to_local(expected, local, sizeof(local), BP_MINI_CODEPAGE) != 0 ||
        (unsigned char)local[0] != expected_byte || local[1] != '\0') return 2;
    if (bp_utf8_to_local("\xE4\xB8\xAD", local, sizeof(local),
                         BP_MINI_CODEPAGE) != BP_CP_UNREPRESENTABLE) return 3;
    if (bp_local_to_utf8(filename_local, utf8, sizeof(utf8),
                         BP_MINI_CODEPAGE) != 0 ||
        strcmp(utf8, filename_utf8) != 0) return 5;
    if (bp_utf8_to_local(filename_utf8, local, sizeof(local),
                         BP_MINI_CODEPAGE) != 0 ||
        strcmp(local, filename_local) != 0) return 6;
    if (bp_local_casefold(lower_case, folded_lower, sizeof(folded_lower),
                          BP_MINI_CODEPAGE) != 0 ||
        bp_local_casefold(upper_case, folded_upper, sizeof(folded_upper),
                          BP_MINI_CODEPAGE) != 0 ||
        strcmp(folded_lower, folded_upper) != 0) return 4;
    puts("mini codepage mapping and refusal checks passed");
    return 0;
}
