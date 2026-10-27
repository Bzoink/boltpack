/* Verify mini builds compile and use only the target's configured codepage. */
#include <stdio.h>
#include <string.h>
#include "bp_codepage.h"

int main(void)
{
    char utf8[8], local[8];
    const char *expected;
    unsigned char expected_byte;
#if BP_CREATOR_OS == BP_OS_OS2
    expected = "\xC3\xB8"; /* CP850 0x9B is U+00F8. */
    expected_byte = 0x9B;
#else
    expected = "\xC2\xA2"; /* CP437 0x9B is U+00A2. */
    expected_byte = 0x9B;
#endif
    local[0] = (char)expected_byte;
    local[1] = '\0';
    if (bp_local_to_utf8(local, utf8, sizeof(utf8), BP_MINI_CODEPAGE) != 0 ||
        strcmp(utf8, expected) != 0) return 1;
    if (bp_utf8_to_local(expected, local, sizeof(local), BP_MINI_CODEPAGE) != 0 ||
        (unsigned char)local[0] != expected_byte || local[1] != '\0') return 2;
    if (bp_utf8_to_local("\xE4\xB8\xAD", local, sizeof(local),
                         BP_MINI_CODEPAGE) != BP_CP_UNREPRESENTABLE) return 3;
    puts("mini codepage mapping and refusal checks passed");
    return 0;
}
