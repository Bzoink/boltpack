/* Test-only DOS16 stack low-water sampling. */
#ifndef BP_MINI_STACK_PROBE_H
#define BP_MINI_STACK_PROBE_H

#ifdef BP_TEST_STACK_HIGHWATER
void bp_test_stack_probe(const char *source_file, unsigned int source_line);
#define BP_STACK_PROBE() bp_test_stack_probe(__FILE__, (unsigned int)__LINE__)
#else
#define BP_STACK_PROBE() ((void)0)
#endif

#endif
