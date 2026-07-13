#ifndef TEST_CHILD_MODES_H
#define TEST_CHILD_MODES_H

#include "../src/foundation/compat.h"

#include <stdio.h>
#include <string.h>

/* Deterministic child modes used by subprocess tests on every host. They avoid
 * shell-specific quoting and keep the test focused on spawn, output capture,
 * supervision, and termination. */
static inline int tf_maybe_run_subprocess_child(int argc, char **argv) {
    if (argc < 2) {
        return -1;
    }
    if (strcmp(argv[1], "__cbm_subprocess_emit") == 0) {
        (void)fputs("alpha\nbeta\n", stdout);
        (void)fflush(stdout);
        return 0;
    }
    if (strcmp(argv[1], "__cbm_subprocess_spin") == 0) {
        for (;;) {
            cbm_usleep(10000);
        }
    }
    return -1;
}

#endif
