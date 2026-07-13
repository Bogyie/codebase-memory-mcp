/* Minimal runner for `make -f Makefile.cbm test-foundation`.
 *
 * tests/test_main.c intentionally embeds the complete MCP index-worker path
 * and references every suite. Linking that entry point to the foundation-only
 * source list makes the supposedly fast target fail with unresolved symbols.
 */

int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

#include "test_framework.h"
#include "test_child_modes.h"

#include <stdio.h>

extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_platform(void);
extern void suite_subprocess(void);
extern void suite_dump_verify(void);
extern void suite_rooted_file(void);

int main(int argc, char **argv) {
    int subprocess_rc = tf_maybe_run_subprocess_child(argc, argv);
    if (subprocess_rc >= 0) {
        return subprocess_rc;
    }
    printf("\n  codebase-memory-mcp  foundation test suite\n");
    RUN_SUITE(arena);
    RUN_SUITE(hash_table);
    RUN_SUITE(dyn_array);
    RUN_SUITE(str_intern);
    RUN_SUITE(log);
    RUN_SUITE(str_util);
    RUN_SUITE(platform);
    RUN_SUITE(subprocess);
    RUN_SUITE(dump_verify);
    RUN_SUITE(rooted_file);
    TEST_SUMMARY();
}
