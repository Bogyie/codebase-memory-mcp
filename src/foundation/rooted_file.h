#ifndef CBM_ROOTED_FILE_H
#define CBM_ROOTED_FILE_H

/* Bounded, race-resistant reads of regular files below an already-authorized
 * repository root.  Callers provide a relative path; the implementation binds
 * containment, bytes, digest, and metadata to one opened descriptor/handle. */

#include "foundation/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CBM_ROOTED_FILE_OK = 0,
    CBM_ROOTED_FILE_NOT_FOUND,
    CBM_ROOTED_FILE_TOO_LARGE,
    CBM_ROOTED_FILE_INVALID,
    CBM_ROOTED_FILE_UNAVAILABLE,
    CBM_ROOTED_FILE_CHANGED,
    CBM_ROOTED_FILE_OUT_OF_MEMORY,
} cbm_rooted_file_status_t;

typedef struct {
    /* malloc-owned and NUL-terminated on CBM_ROOTED_FILE_OK, including for an
     * empty file. len excludes the terminator. */
    char *data;
    size_t len;
    char sha256[CBM_SHA256_HEX_LEN + 1];

    /* Populated after a regular file has been opened and inspected. This is
     * also true for TOO_LARGE and allocation failures. */
    bool metadata_valid;
    int64_t mtime_ns;
    int64_t size;
} cbm_rooted_file_t;

/* Validate the portable relative-path subset accepted by the rooted reader.
 * Absolute paths, empty components, and `.` / `..` components are rejected. */
bool cbm_rooted_relative_path_valid(const char *relative_path);

/* Read and hash one stable regular-file generation below root_path. max_bytes
 * must be positive. Symlinks are never followed on POSIX; on Windows the
 * opened file HANDLE's final path must be contained by the opened root HANDLE.
 * Always initialize/free `out` through this API when reusing it. */
cbm_rooted_file_status_t cbm_rooted_file_read(const char *root_path, const char *relative_path,
                                              size_t max_bytes, cbm_rooted_file_t *out);
void cbm_rooted_file_free(cbm_rooted_file_t *file);

/* Deterministic race injection for regression tests. The hook runs after the
 * final file is opened and initially authorized, but before its bytes are
 * read. Production callers must leave it unset. */
typedef void (*cbm_rooted_file_test_hook_fn)(void *context);
void cbm_rooted_file_set_test_hook(cbm_rooted_file_test_hook_fn hook, void *context);

#endif /* CBM_ROOTED_FILE_H */
