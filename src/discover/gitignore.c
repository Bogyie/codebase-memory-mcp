
/*
 * gitignore.c — Gitignore-style pattern matching.
 *
 * Implements the core gitignore pattern matching algorithm:
 *   - * matches anything except /
 *   - ** matches any number of path components
 *   - ? matches any single character except /
 *   - [abc] and [a-z] character classes
 *   - ! prefix for negation
 *   - trailing / for directory-only matching
 *   - patterns with / are rooted (anchored to base)
 */
#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

enum {
    GI_INIT_CAP = 16,
    GI_FILE_MAX_BYTES = 4 * 1024 * 1024,
    GI_PATTERN_MAX_BYTES = 16 * 1024,
    GI_PATTERN_COUNT_MAX = 100000,
    GI_MATCH_PATH_MAX_BYTES = 1024 * 1024,
    GI_MATCH_STEP_MAX = 16 * 1024 * 1024,
};
#include "discover/discover.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Pattern representation ──────────────────────────────────────── */

typedef enum {
    GI_TOKEN_LITERAL,
    GI_TOKEN_QUESTION,
    GI_TOKEN_STAR,
    GI_TOKEN_DOUBLESTAR,
    GI_TOKEN_DOUBLESTAR_SLASH,
    GI_TOKEN_CHARCLASS,
} gi_token_kind_t;

typedef struct {
    gi_token_kind_t kind;
    size_t offset; /* literal byte or char-class body in pattern */
    size_t length;
} gi_token_t;

typedef struct {
    char *pattern; /* the glob pattern (normalized) */
    gi_token_t *tokens;
    size_t token_count;
    bool negated;  /* starts with ! */
    bool dir_only; /* ends with / */
    bool rooted;   /* contains / (anchored to root) */
} gi_pattern_t;

struct cbm_gitignore {
    gi_pattern_t *patterns;
    size_t count;
    size_t capacity;
};

/* ── Pattern matching engine ─────────────────────────────────────── */

static bool gi_compile_pattern(gi_pattern_t *pattern, size_t len) {
    if (len == 0 || len > GI_PATTERN_MAX_BYTES || len > SIZE_MAX / sizeof(gi_token_t)) {
        return false;
    }
    gi_token_t *tokens = malloc(len * sizeof(gi_token_t));
    if (!tokens) {
        return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < len;) {
        gi_token_t token = {.kind = GI_TOKEN_LITERAL, .offset = i, .length = 1};
        if (pattern->pattern[i] == '*') {
            if (i + 1 < len && pattern->pattern[i + 1] == '*') {
                if (i + 2 < len && pattern->pattern[i + 2] == '/') {
                    token.kind = GI_TOKEN_DOUBLESTAR_SLASH;
                    token.length = 3;
                } else {
                    token.kind = GI_TOKEN_DOUBLESTAR;
                    token.length = 2;
                }
            } else {
                token.kind = GI_TOKEN_STAR;
            }
        } else if (pattern->pattern[i] == '?') {
            token.kind = GI_TOKEN_QUESTION;
        } else if (pattern->pattern[i] == '[') {
            size_t end = i + 1;
            while (end < len && pattern->pattern[end] != ']') {
                end++;
            }
            token.kind = GI_TOKEN_CHARCLASS;
            token.offset = i + 1;
            token.length = end - token.offset;
            if (end < len) {
                end++;
            }
            tokens[count++] = token;
            i = end;
            continue;
        }
        tokens[count++] = token;
        i += token.length;
    }
    pattern->tokens = tokens;
    pattern->token_count = count;
    return true;
}

static bool gi_charclass_matches(const gi_pattern_t *pattern, const gi_token_t *token,
                                 unsigned char ch) {
    const unsigned char *body = (const unsigned char *)pattern->pattern + token->offset;
    size_t pos = 0;
    bool negate = false;
    if (pos < token->length && (body[pos] == '!' || body[pos] == '^')) {
        negate = true;
        pos++;
    }
    bool matched = false;
    unsigned char previous = 0;
    while (pos < token->length) {
        if (body[pos] == '-' && previous != 0 && pos + 1 < token->length) {
            pos++;
            if (ch >= previous && ch <= body[pos]) {
                matched = true;
            }
            previous = body[pos++];
        } else {
            if (ch == body[pos]) {
                matched = true;
            }
            previous = body[pos++];
        }
    }
    return negate ? !matched : matched;
}

/* Iterative dynamic programming avoids recursive wildcard backtracking. It
 * uses two O(path) rows and caps token×path work, so hostile patterns cannot
 * turn discovery into exponential CPU or a stack overflow. */
static int gi_glob_match(const gi_pattern_t *pattern, const char *str) {
    size_t len = strlen(str);
    if (len > GI_MATCH_PATH_MAX_BYTES || len == SIZE_MAX ||
        pattern->token_count > GI_MATCH_STEP_MAX / (len + SKIP_ONE)) {
        return CBM_NOT_FOUND;
    }
    size_t row_len = len + SKIP_ONE;
    if (row_len > SIZE_MAX / PAIR_LEN) {
        return CBM_NOT_FOUND;
    }
    unsigned char *rows = calloc(row_len * PAIR_LEN, sizeof(unsigned char));
    if (!rows) {
        return CBM_NOT_FOUND;
    }
    unsigned char *next = rows;
    unsigned char *current = rows + row_len;
    next[len] = 1;

    for (size_t token_index = pattern->token_count; token_index > 0; token_index--) {
        const gi_token_t *token = &pattern->tokens[token_index - SKIP_ONE];
        memset(current, 0, row_len);
        switch (token->kind) {
        case GI_TOKEN_LITERAL:
            for (size_t j = 0; j < len; j++) {
                current[j] = (unsigned char)((unsigned char)str[j] ==
                                                 (unsigned char)pattern->pattern[token->offset] &&
                                             next[j + SKIP_ONE]);
            }
            break;
        case GI_TOKEN_QUESTION:
            for (size_t j = 0; j < len; j++) {
                current[j] = (unsigned char)(str[j] != '/' && next[j + SKIP_ONE]);
            }
            break;
        case GI_TOKEN_CHARCLASS:
            for (size_t j = 0; j < len; j++) {
                current[j] =
                    (unsigned char)(gi_charclass_matches(pattern, token, (unsigned char)str[j]) &&
                                    next[j + SKIP_ONE]);
            }
            break;
        case GI_TOKEN_STAR:
            current[len] = next[len];
            for (size_t j = len; j > 0; j--) {
                size_t at = j - SKIP_ONE;
                current[at] =
                    (unsigned char)(next[at] || (str[at] != '/' && current[at + SKIP_ONE]));
            }
            break;
        case GI_TOKEN_DOUBLESTAR:
            current[len] = next[len];
            for (size_t j = len; j > 0; j--) {
                size_t at = j - SKIP_ONE;
                current[at] = (unsigned char)(next[at] || current[at + SKIP_ONE]);
            }
            break;
        case GI_TOKEN_DOUBLESTAR_SLASH: {
            bool suffix_after_slash_matches = false;
            current[len] = next[len];
            for (size_t j = len; j > 0; j--) {
                size_t at = j - SKIP_ONE;
                if (str[at] == '/' && next[at + SKIP_ONE]) {
                    suffix_after_slash_matches = true;
                }
                current[at] = (unsigned char)(next[at] || suffix_after_slash_matches);
            }
            break;
        }
        }
        unsigned char *swap = next;
        next = current;
        current = swap;
    }
    int matched = next[0] ? 1 : 0;
    free(rows);
    return matched;
}

/* ── Pattern parsing ─────────────────────────────────────────────── */

static bool gi_add_pattern(cbm_gitignore_t *gi, const char *line, size_t len) {
    /* Trim trailing whitespace */
    while (len > 0 && (line[len - SKIP_ONE] == ' ' || line[len - SKIP_ONE] == '\t' ||
                       line[len - SKIP_ONE] == '\r')) {
        len--;
    }
    if (len == 0) {
        return true;
    }

    gi_pattern_t p = {0};

    /* Check for negation */
    const char *start = line;
    if (*start == '!') {
        p.negated = true;
        start++;
        len--;
    }

    if (len == 0) {
        return true;
    }

    /* Check for trailing / (directory-only) */
    if (start[len - SKIP_ONE] == '/') {
        p.dir_only = true;
        len--;
    }

    if (len == 0) {
        return true;
    }

    /* Check for leading / (rooted) */
    if (*start == '/') {
        p.rooted = true;
        start++;
        len--;
    }

    if (len == 0) {
        return true;
    }

    /* Check if pattern contains / anywhere (makes it rooted) */
    if (!p.rooted) {
        for (size_t i = 0; i < len; i++) {
            if (start[i] == '/') {
                p.rooted = true;
                break;
            }
        }
    }

    /* Copy pattern */
    if (len == SIZE_MAX) {
        return false;
    }
    p.pattern = malloc(len + SKIP_ONE);
    if (!p.pattern) {
        return false;
    }
    memcpy(p.pattern, start, len);
    p.pattern[len] = '\0';
    if (!gi_compile_pattern(&p, len)) {
        free(p.pattern);
        return false;
    }

    /* Grow array if needed */
    if (gi->count >= GI_PATTERN_COUNT_MAX) {
        free(p.tokens);
        free(p.pattern);
        return false;
    }
    if (gi->count >= gi->capacity) {
        if (gi->capacity > SIZE_MAX / PAIR_LEN) {
            free(p.tokens);
            free(p.pattern);
            return false;
        }
        size_t new_cap = gi->capacity ? gi->capacity * PAIR_LEN : GI_INIT_CAP;
        if (new_cap > SIZE_MAX / sizeof(gi_pattern_t)) {
            free(p.tokens);
            free(p.pattern);
            return false;
        }
        gi_pattern_t *new_patterns = realloc(gi->patterns, new_cap * sizeof(gi_pattern_t));
        if (!new_patterns) {
            free(p.tokens);
            free(p.pattern);
            return false;
        }
        gi->patterns = new_patterns;
        gi->capacity = new_cap;
    }

    gi->patterns[gi->count++] = p;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

cbm_gitignore_t *cbm_gitignore_parse(const char *content) {
    if (!content) {
        return NULL;
    }

    cbm_gitignore_t *gi = calloc(CBM_ALLOC_ONE, sizeof(cbm_gitignore_t));
    if (!gi) {
        return NULL;
    }

    const char *line = content;
    while (*line) {
        /* Find end of line */
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);

        /* Skip comments and blank lines */
        if (len > 0 && line[0] != '#') {
            if (!gi_add_pattern(gi, line, len)) {
                cbm_gitignore_free(gi);
                return NULL;
            }
        }

        if (!eol) {
            break;
        }
        line = eol + SKIP_ONE;
    }

    return gi;
}

cbm_gitignore_load_result_t cbm_gitignore_load_hashed(const char *path, cbm_gitignore_t **out,
                                                      char sha256[65]) {
    if (out) {
        *out = NULL;
    }
    if (sha256) {
        sha256[0] = '\0';
    }
    if (!path || !out || !sha256) {
        return CBM_GITIGNORE_LOAD_ERROR;
    }

    /* The stable reader opens O_NONBLOCK on POSIX, rejects non-regular files,
     * reads and hashes one descriptor generation, and verifies that the live
     * pathname still names it. This prevents FIFO hangs and mixed-byte parses. */
    int probe = cbm_path_probe(path);
    if (probe == 0) {
        return CBM_GITIGNORE_LOAD_MISSING;
    }
    if (probe < 0) {
        return CBM_GITIGNORE_LOAD_ERROR;
    }
    char *buf = NULL;
    size_t size = 0;
    int read_rc = cbm_sha256_file_read_hex(path, GI_FILE_MAX_BYTES, &buf, &size, sha256);
    if (read_rc != CBM_SHA256_FILE_HASHED) {
        return CBM_GITIGNORE_LOAD_ERROR;
    }
    /* The parser consumes C strings. Reject embedded NUL instead of silently
     * treating the suffix (which may contain exclusion rules) as absent. */
    if (memchr(buf, '\0', size) != NULL) {
        free(buf);
        sha256[0] = '\0';
        return CBM_GITIGNORE_LOAD_ERROR;
    }
    *out = cbm_gitignore_parse(buf);
    free(buf);
    if (!*out) {
        sha256[0] = '\0';
        return CBM_GITIGNORE_LOAD_ERROR;
    }
    return CBM_GITIGNORE_LOAD_OK;
}

cbm_gitignore_load_result_t cbm_gitignore_load_ex(const char *path, cbm_gitignore_t **out) {
    char sha256[CBM_SHA256_HEX_LEN + SKIP_ONE];
    return cbm_gitignore_load_hashed(path, out, sha256);
}

cbm_gitignore_t *cbm_gitignore_load(const char *path) {
    cbm_gitignore_t *gi = NULL;
    return cbm_gitignore_load_ex(path, &gi) == CBM_GITIGNORE_LOAD_OK ? gi : NULL;
}

bool cbm_gitignore_match_result_ex(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir,
                                   int *result) {
    if (result) {
        *result = 0;
    }
    if (!result || !rel_path) {
        return false;
    }
    if (!gi) {
        return true;
    }

    /* Extract the basename for non-rooted pattern matching */
    const char *basename = strrchr(rel_path, '/');
    basename = basename ? basename + SKIP_ONE : rel_path;

    int matched = 0;
    size_t total_steps = 0;

    for (size_t i = 0; i < gi->count; i++) {
        const gi_pattern_t *p = &gi->patterns[i];

        if (p->dir_only && !is_dir) {
            continue;
        }

        const char *candidate = p->rooted ? rel_path : basename;
        size_t candidate_len = strlen(candidate);
        if (candidate_len == SIZE_MAX ||
            p->token_count > (GI_MATCH_STEP_MAX - total_steps) / (candidate_len + SKIP_ONE)) {
            return false;
        }
        total_steps += p->token_count * (candidate_len + SKIP_ONE);
        int this_match = gi_glob_match(p, candidate);
        if (this_match == CBM_NOT_FOUND) {
            return false;
        }

        if (this_match > 0) {
            matched = p->negated ? -1 : 1;
        }
    }

    *result = matched;
    return true;
}

int cbm_gitignore_match_result(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir) {
    int result = 0;
    return cbm_gitignore_match_result_ex(gi, rel_path, is_dir, &result) ? result : 0;
}

bool cbm_gitignore_matches(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir) {
    return cbm_gitignore_match_result(gi, rel_path, is_dir) > 0;
}

void cbm_gitignore_free(cbm_gitignore_t *gi) {
    if (!gi) {
        return;
    }
    for (size_t i = 0; i < gi->count; i++) {
        free(gi->patterns[i].tokens);
        free(gi->patterns[i].pattern);
    }
    free(gi->patterns);
    free(gi);
}

/* Test seam: lets a unit test simulate strdup() failure mid-merge so the
 * atomic-rollback path can be exercised without real OOM. NULL = use strdup. */
char *(*cbm_gitignore_merge_dup_hook_for_test)(const char *) = NULL;

bool cbm_gitignore_merge(cbm_gitignore_t *dst, const cbm_gitignore_t *src) {
    if (!dst) {
        return false;
    }
    if (!src || src->count == 0) {
        return true; /* nothing to merge */
    }
    if (src->count > SIZE_MAX - dst->count) {
        return false;
    }
    size_t needed = dst->count + src->count;
    if (needed > SIZE_MAX / sizeof(gi_pattern_t)) {
        return false;
    }
    /* Build the complete result beside dst. This preserves even dst's backing
     * allocation/capacity when a later pattern duplication fails. */
    gi_pattern_t *merged = malloc(needed * sizeof(gi_pattern_t));
    if (!merged) {
        return false;
    }
    if (dst->count > 0) {
        memcpy(merged, dst->patterns, dst->count * sizeof(gi_pattern_t));
    }
    size_t copied = 0;
    for (size_t i = 0; i < src->count; i++) {
        char *pat = cbm_gitignore_merge_dup_hook_for_test
                        ? cbm_gitignore_merge_dup_hook_for_test(src->patterns[i].pattern)
                        : strdup(src->patterns[i].pattern);
        if (!pat) {
            for (size_t j = 0; j < copied; j++) {
                free(merged[dst->count + j].tokens);
                free(merged[dst->count + j].pattern);
            }
            free(merged);
            return false;
        }
        merged[dst->count + copied] = src->patterns[i];
        merged[dst->count + copied].pattern = pat;
        size_t token_bytes = src->patterns[i].token_count * sizeof(gi_token_t);
        merged[dst->count + copied].tokens = malloc(token_bytes);
        if (!merged[dst->count + copied].tokens) {
            free(pat);
            for (size_t j = 0; j < copied; j++) {
                free(merged[dst->count + j].tokens);
                free(merged[dst->count + j].pattern);
            }
            free(merged);
            return false;
        }
        memcpy(merged[dst->count + copied].tokens, src->patterns[i].tokens, token_bytes);
        copied++;
    }
    free(dst->patterns);
    dst->patterns = merged;
    dst->count = needed;
    dst->capacity = needed;
    return true;
}
