/*
 * test_runner.h -- minimal C89 unit-test framework for BoltPack
 *
 * Usage: include this header in exactly one translation unit per test binary.
 * Call UNIT_CHECK(cond, "description") for each assertion.
 * Call UNIT_SECTION("name") to print a section header.
 * Return UNIT_REPORT() from main() -- returns 1 on any failure, 0 on all pass.
 *
 * ANSI C (C89/C90) only. No dependencies beyond <stdio.h>.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_TEST_RUNNER_H
#define BP_TEST_RUNNER_H

#include <stdio.h>

static int bp_unit_pass_count = 0;
static int bp_unit_fail_count = 0;

#define UNIT_CHECK(cond, name) \
    do { \
        if (cond) { \
            printf("  PASS  %s\n", (name)); \
            bp_unit_pass_count++; \
        } else { \
            printf("  FAIL  %s  [%s:%d]\n", (name), __FILE__, __LINE__); \
            bp_unit_fail_count++; \
        } \
    } while (0)

#define UNIT_SECTION(name) \
    printf("\n--- %s ---\n", (name))

#define UNIT_REPORT() \
    (printf("\nResults: %d passed, %d failed\n", \
            bp_unit_pass_count, bp_unit_fail_count), \
     (bp_unit_fail_count != 0))

#endif /* BP_TEST_RUNNER_H */
