/*
 * design_io.c — Bounded and diagnosed Design Context input handling.
 */
#include "design/design_io.h"

#include "foundation/compat_fs.h"
#include "foundation/limits.h"
#include "foundation/log.h"
#include "foundation/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DESIGN_MAX_FILE_SIZE = 8 * 1024 * 1024,
    DESIGN_MAX_GLOB_LENGTH = 4096,
};

static char *design_io_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *copy = (char *)malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n + 1);
    }
    return copy;
}

/* Bounded dynamic-programming glob matcher for repository-relative
 * configuration patterns. Supports `*`, `**`, and `?`; path separators are
 * normalized by discovery already. Recursive backtracking can take
 * exponential time on patterns such as `*a*a*...*b`. */
bool cbm_design_glob_match(const char *pattern, const char *text) {
    if (!pattern || !text) {
        return false;
    }
    size_t pattern_len = strlen(pattern);
    size_t text_len = strlen(text);
    if (pattern_len > DESIGN_MAX_GLOB_LENGTH || text_len > DESIGN_MAX_GLOB_LENGTH) {
        return false;
    }

    unsigned char *previous = (unsigned char *)calloc(text_len + 1, 1);
    unsigned char *current = (unsigned char *)calloc(text_len + 1, 1);
    if (!previous || !current) {
        free(previous);
        free(current);
        return false;
    }
    previous[0] = 1;

    for (size_t i = 0; i < pattern_len;) {
        memset(current, 0, text_len + 1);
        char token = pattern[i++];
        bool double_star = false;
        if (token == '*' && i < pattern_len && pattern[i] == '*') {
            double_star = true;
            i++;
            /* The separator belongs to the recursive wildcard and zero
             * directories are allowed, preserving historical semantics. */
            if (i < pattern_len && pattern[i] == '/') {
                i++;
            }
        }

        if (token == '*') {
            current[0] = previous[0];
            for (size_t j = 1; j <= text_len; j++) {
                bool may_consume = double_star || text[j - 1] != '/';
                current[j] = (unsigned char)(previous[j] || (may_consume && current[j - 1]));
            }
        } else if (token == '?') {
            for (size_t j = 1; j <= text_len; j++) {
                current[j] = (unsigned char)(previous[j - 1] && text[j - 1] != '/');
            }
        } else {
            for (size_t j = 1; j <= text_len; j++) {
                current[j] = (unsigned char)(previous[j - 1] && text[j - 1] == token);
            }
        }

        unsigned char *swap = previous;
        previous = current;
        current = swap;
    }

    bool matched = previous[text_len] != 0;
    free(previous);
    free(current);
    return matched;
}

bool cbm_design_patterns_match(const cbm_design_patterns_t *patterns, const char *path) {
    if (!patterns || patterns->count == 0) {
        return false;
    }
    for (int i = 0; i < patterns->count; i++) {
        if (cbm_design_glob_match(patterns->items[i], path)) {
            return true;
        }
    }
    return false;
}

void cbm_design_patterns_free(cbm_design_patterns_t *patterns) {
    if (!patterns) {
        return;
    }
    for (int i = 0; i < patterns->count; i++) {
        free(patterns->items[i]);
    }
    free(patterns->items);
    patterns->items = NULL;
    patterns->count = 0;
}

int cbm_design_patterns_load(yyjson_val *obj, const char *key, cbm_design_patterns_t *out) {
    yyjson_val *array = yyjson_obj_get(obj, key);
    if (!array || !yyjson_is_arr(array)) {
        return 0;
    }
    size_t idx, max;
    yyjson_val *value;
    yyjson_arr_foreach(array, idx, max, value) {
        const char *s = yyjson_get_str(value);
        if (!s || !s[0]) {
            continue;
        }
        if (strlen(s) > DESIGN_MAX_GLOB_LENGTH) {
            cbm_log_warn("design.pattern_skip", "key", key, "reason", "too_long");
            continue;
        }
        char **grown = (char **)realloc(out->items, (size_t)(out->count + 1) * sizeof(char *));
        if (!grown) {
            return -1;
        }
        out->items = grown;
        out->items[out->count] = design_io_strdup(s);
        if (!out->items[out->count]) {
            return -1;
        }
        out->count++;
    }
    return 0;
}

/* Read and hash one stable regular-file generation from the same handle. A
 * concurrent change is diagnosed rather than partially parsed. */
static char *design_read_file(const cbm_file_info_t *file, size_t *out_len,
                              char out_hash[CBM_SHA256_HEX_LEN + 1], size_t requested_max_bytes,
                              const char **out_failure) {
    if (out_len) {
        *out_len = 0;
    }
    if (out_failure) {
        *out_failure = NULL;
    }
    if (out_hash) {
        out_hash[0] = '\0';
    }
    if (!file || !file->path || !out_hash || file->size == 0) {
        if (out_failure) {
            *out_failure = "empty_or_invalid_size";
        }
        return NULL;
    }
    long global_limit = cbm_max_file_bytes();
    size_t max_bytes = DESIGN_MAX_FILE_SIZE;
    if (requested_max_bytes > 0 && requested_max_bytes < max_bytes) {
        max_bytes = requested_max_bytes;
    }
    if (global_limit > 0 && (size_t)global_limit < max_bytes) {
        max_bytes = (size_t)global_limit;
    }
    if (file->size > 0 && (uint64_t)file->size > (uint64_t)max_bytes) {
        if (out_failure) {
            *out_failure = "oversized";
        }
        return NULL;
    }
    char *buf = NULL;
    size_t n = 0;
    int read_rc = cbm_sha256_file_read_hex(file->path, max_bytes, &buf, &n, out_hash);
    if (read_rc != 0) {
        if (out_failure) {
            *out_failure = read_rc == CBM_SHA256_FILE_OUT_OF_MEMORY ? "out_of_memory"
                                                                    : "incomplete_or_changed_read";
        }
        return NULL;
    }
    if (file->size > 0 && (uint64_t)file->size != (uint64_t)n) {
        free(buf);
        out_hash[0] = '\0';
        if (out_failure) {
            *out_failure = "incomplete_or_changed_read";
        }
        return NULL;
    }
    if (n == 0) {
        free(buf);
        out_hash[0] = '\0';
        if (out_failure) {
            *out_failure = "empty_or_invalid_size";
        }
        return NULL;
    }
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

int cbm_design_load_source_limited(const cbm_file_info_t *file, const char *source_kind,
                                   size_t max_bytes, char **out_source, size_t *out_len,
                                   char out_hash[CBM_SHA256_HEX_LEN + 1]) {
    if (!out_source || !out_hash) {
        return -1;
    }
    *out_source = NULL;
    const char *failure = NULL;
    char *source = design_read_file(file, out_len, out_hash, max_bytes, &failure);
    if (source) {
        *out_source = source;
        return 1;
    }
    cbm_log_warn("design.source_skip", "source_kind", source_kind ? source_kind : "unknown", "path",
                 file && file->rel_path ? file->rel_path : "", "reason",
                 failure ? failure : "unknown");
    return failure && strcmp(failure, "out_of_memory") == 0 ? -1 : 0;
}

int cbm_design_load_source(const cbm_file_info_t *file, const char *source_kind, char **out_source,
                           size_t *out_len, char out_hash[CBM_SHA256_HEX_LEN + 1]) {
    return cbm_design_load_source_limited(file, source_kind, 0, out_source, out_len, out_hash);
}
