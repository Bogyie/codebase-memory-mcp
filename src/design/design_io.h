/*
 * design_io.h — Internal Design Context input helpers.
 *
 * Keeps bounded configuration matching and complete source-file reads away
 * from the graph materialization logic in design.c. This is not a public API.
 */
#ifndef CBM_DESIGN_IO_H
#define CBM_DESIGN_IO_H

#include "discover/discover.h"
#include "foundation/sha256.h"

#include <yyjson/yyjson.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **items;
    int count;
} cbm_design_patterns_t;

/* Match repository-relative paths without recursive backtracking. */
bool cbm_design_glob_match(const char *pattern, const char *text);
bool cbm_design_patterns_match(const cbm_design_patterns_t *patterns, const char *path);

/* Own pattern strings copied from a JSON configuration array. */
int cbm_design_patterns_load(yyjson_val *obj, const char *key, cbm_design_patterns_t *out);
void cbm_design_patterns_free(cbm_design_patterns_t *patterns);

/* Return 1 with an owned source buffer, 0 for a diagnosed skippable input,
 * and -1 for OOM so the staging pass preserves the previous good graph. */
int cbm_design_load_source(const cbm_file_info_t *file, const char *source_kind, char **out_source,
                           size_t *out_len, char out_hash[CBM_SHA256_HEX_LEN + 1]);
int cbm_design_load_source_limited(const cbm_file_info_t *file, const char *source_kind,
                                   size_t max_bytes, char **out_source, size_t *out_len,
                                   char out_hash[CBM_SHA256_HEX_LEN + 1]);

#endif /* CBM_DESIGN_IO_H */
