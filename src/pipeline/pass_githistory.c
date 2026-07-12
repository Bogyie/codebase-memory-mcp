/*
 * pass_githistory.c — Analyze git log to find change coupling.
 *
 * Runs `git log --name-only --since=6 months ago` and computes
 * file pairs that change together frequently. Creates FILE_CHANGES_WITH
 * edges between File nodes with coupling_score properties.
 *
 * Skips commits with >20 files (refactoring/merge noise).
 * Requires minimum 3 co-changes for an edge.
 *
 * Depends on: pass_structure having created File nodes
 */
#include "foundation/constants.h"

enum { GH_RING = 4, GH_RING_MASK = 3, GH_INIT_CAP = 16, GH_MIN_COMMITS = 3, GH_MAX_FILES = 20 };

#define SLEN(s) (sizeof(s) - 1)
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/hash_table.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/str_util.h"
#include "git/git_context.h"

/* Minimum coupling score to create an edge */
#define MIN_COUPLING_SCORE 0.3

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

static const char *itoa_log(int val) {
    static CBM_TLS char bufs[GH_RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + SKIP_ONE) & GH_RING_MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

static bool ends_with(const char *s, size_t slen, const char *suffix) {
    size_t sflen = strlen(suffix);
    return slen >= sflen && strcmp(s + slen - sflen, suffix) == 0;
}

bool cbm_is_trackable_file(const char *path) {
    if (!path) {
        return false;
    }
    /* Skip directory prefixes */
#define LEN_NODE_MODULES_SLASH 13 /* strlen("node_modules/") */
    if (strncmp(path, ".git/", SLEN(".git/")) == 0 ||
        strncmp(path, "node_modules/", LEN_NODE_MODULES_SLASH) == 0 ||
        strncmp(path, "vendor/", SLEN("vendor/")) == 0 ||
        strncmp(path, "__pycache__/", SLEN("__pycache__/")) == 0 ||
        strncmp(path, ".cache/", SLEN(".cache/")) == 0) {
        return false;
    }
    /* Skip lock/generated file names */
    const char *base = strrchr(path, '/');
    base = base ? base + SKIP_ONE : path;
    if (strcmp(base, "package-lock.json") == 0 || strcmp(base, "yarn.lock") == 0 ||
        strcmp(base, "pnpm-lock.yaml") == 0 || strcmp(base, "Cargo.lock") == 0 ||
        strcmp(base, "poetry.lock") == 0 || strcmp(base, "composer.lock") == 0 ||
        strcmp(base, "Gemfile.lock") == 0 || strcmp(base, "Pipfile.lock") == 0) {
        return false;
    }
    /* Skip non-source file extensions */
    size_t len = strlen(path);
    if (ends_with(path, len, ".lock") || ends_with(path, len, ".sum") ||
        ends_with(path, len, ".min.js") || ends_with(path, len, ".min.css") ||
        ends_with(path, len, ".map") || ends_with(path, len, ".wasm") ||
        ends_with(path, len, ".png") || ends_with(path, len, ".jpg") ||
        ends_with(path, len, ".gif") || ends_with(path, len, ".ico") ||
        ends_with(path, len, ".svg")) {
        return false;
    }
    return true;
}

/* ── Commit parsing ───────────────────────────────────────────────── */

typedef struct {
    char **files;
    int count;
    int cap;
    long long timestamp; /* unix epoch of this commit; 0 when unknown */
} commit_t;

static bool commit_add_file(commit_t *c, const char *file) {
    if (c->count >= c->cap) {
        if (c->cap > INT_MAX / PAIR_LEN) {
            return false;
        }
        int new_cap = c->cap ? c->cap * PAIR_LEN : GH_INIT_CAP;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*c->files)) {
            return false;
        }
        char **grown = realloc(c->files, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        c->files = grown;
        c->cap = new_cap;
    }
    char *copy = strdup(file);
    if (!copy) {
        return false;
    }
    c->files[c->count++] = copy;
    return true;
}

static void commit_free(commit_t *c) {
    for (int i = 0; i < c->count; i++) {
        free(c->files[i]);
    }
    free(c->files);
}

/* ── Exact argv-based git log parsing ─────────────────────────────── */

static bool append_commit(commit_t **commits, int *count, int *cap, commit_t *current) {
    if (current->count == 0) {
        commit_free(current);
        memset(current, 0, sizeof(*current));
        return true;
    }
    if (*count >= *cap) {
        if (*cap > INT_MAX / PAIR_LEN) {
            return false;
        }
        int new_cap = *cap ? *cap * PAIR_LEN : CBM_SZ_64;
        if ((size_t)new_cap > SIZE_MAX / sizeof(**commits)) {
            return false;
        }
        commit_t *grown = realloc(*commits, (size_t)new_cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        *commits = grown;
        *cap = new_cap;
    }
    (*commits)[(*count)++] = *current;
    memset(current, 0, sizeof(*current));
    return true;
}

static bool parse_commit_header(const char *header, long long *out_timestamp) {
    const char *hash = header + SLEN("COMMIT:");
    const char *separator = strchr(hash, ':');
    if (!separator) {
        return false;
    }
    size_t hash_len = (size_t)(separator - hash);
    if (hash_len != 40 && hash_len != CBM_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < hash_len; i++) {
        if (!isxdigit((unsigned char)hash[i])) {
            return false;
        }
    }
    errno = 0;
    char *end = NULL;
    long long timestamp = strtoll(separator + 1, &end, 10);
    if (errno != 0 || end == separator + 1 || !end || *end != '\0' || timestamp < 0) {
        return false;
    }
    *out_timestamp = timestamp;
    return true;
}

static void free_commits(commit_t *commits, int count) {
    for (int i = 0; i < count; i++) {
        commit_free(&commits[i]);
    }
    free(commits);
}

static int parse_git_log(const char *repo_path, const char *revision, commit_t **out,
                         int *out_count, char out_sha256[CBM_SHA256_HEX_LEN + 1]) {
    *out = NULL;
    *out_count = 0;
    out_sha256[0] = '\0';
    char *data = NULL;
    size_t data_len = 0;
    if (cbm_git_history_capture(repo_path, revision, &data, &data_len, out_sha256) != 0) {
        return CBM_NOT_FOUND;
    }

    int cap = CBM_SZ_64;
    commit_t *commits = calloc((size_t)cap, sizeof(*commits));
    if (!commits) {
        free(data);
        return CBM_NOT_FOUND;
    }
    int count = 0;
    commit_t current = {0};
    bool saw_header = false;
    bool header_allowed = true;
    bool strip_record_separator = false;
    size_t offset = 0;
    while (offset < data_len) {
        char *token = data + offset;
        size_t remaining = data_len - offset;
        char *end = memchr(token, '\0', remaining);
        size_t token_len = end ? (size_t)(end - token) : remaining;
        offset += token_len + (end ? 1U : 0U);
        if (token_len == 0) {
            header_allowed = true;
            continue;
        }
        /* git inserts one record-separating LF between the pretty header and
         * the first -z filename. Remove only that separator; a filename that
         * itself starts with LF retains its second byte. */
        if (strip_record_separator && token[0] == '\n') {
            token++;
            token_len--;
        } else if (strip_record_separator && token_len >= 2 && token[0] == '\r' &&
                   token[1] == '\n') {
            token += 2;
            token_len -= 2;
        }
        strip_record_separator = false;
        if (token_len == 0) {
            continue;
        }
        token[token_len] = '\0';

        if (header_allowed && strncmp(token, "COMMIT:", SLEN("COMMIT:")) == 0) {
            if (!append_commit(&commits, &count, &cap, &current)) {
                goto fail;
            }
            if (!parse_commit_header(token, &current.timestamp)) {
                goto fail;
            }
            saw_header = true;
            header_allowed = false;
            strip_record_separator = true;
            continue;
        }
        if (!saw_header || header_allowed) {
            goto fail;
        }
        header_allowed = false;
        if (cbm_is_trackable_file(token) && !commit_add_file(&current, token)) {
            goto fail;
        }
    }
    if (!append_commit(&commits, &count, &cap, &current)) {
        goto fail;
    }
    free(data);
    *out = commits;
    *out_count = count;
    return 0;

fail:
    commit_free(&current);
    free_commits(commits, count);
    free(data);
    out_sha256[0] = '\0';
    return CBM_NOT_FOUND;
}

/* Callback to free hash table entries. */
static void free_counter(const char *key, void *val, void *ud) {
    (void)ud;
    safe_str_free(&key);
    free(val);
}

/* ── Standalone coupling computation (testable) ──────────────────── */

/* Context for collect_coupling_result callback. */
typedef struct {
    CBMHashTable *file_counts;
    CBMHashTable *pair_timestamps; /* pair_key → long long*: max commit ts */
    cbm_change_coupling_t *out;
    int out_count;
    int max_out;
    bool failed;
} collect_coupling_ctx_t;

static void collect_coupling_cb(const char *pair_key, void *val, void *ud) {
    collect_coupling_ctx_t *cctx = ud;
    int co_count = *(int *)val;
    if (co_count < GH_MIN_COMMITS) {
        return;
    }
    if (cctx->out_count >= cctx->max_out) {
        return;
    }

    const char *sep = strchr(pair_key, '\x01');
    if (!sep) {
        return;
    }
    size_t la = sep - pair_key;
    const char *file_b = sep + SKIP_ONE;

    char file_a_buf[CBM_SZ_512];
    if (la >= sizeof(file_a_buf) || strlen(file_b) >= sizeof(cctx->out[0].file_b)) {
        cctx->failed = true;
        return;
    }
    memcpy(file_a_buf, pair_key, la);
    file_a_buf[la] = '\0';

    int *count_a = cbm_ht_get(cctx->file_counts, file_a_buf);
    int *count_b = cbm_ht_get(cctx->file_counts, file_b);
    if (!count_a || !count_b) {
        return;
    }

    int min_total = *count_a < *count_b ? *count_a : *count_b;
    if (min_total == 0) {
        return;
    }

    double score = (double)co_count / (double)min_total;
    if (score < MIN_COUPLING_SCORE) {
        return;
    }

    cbm_change_coupling_t *cc = &cctx->out[cctx->out_count++];
    snprintf(cc->file_a, sizeof(cc->file_a), "%s", file_a_buf);
    snprintf(cc->file_b, sizeof(cc->file_b), "%s", file_b);
    cc->co_change_count = co_count;
    cc->coupling_score = score;
    long long *ts = cbm_ht_get(cctx->pair_timestamps, pair_key);
    cc->last_co_change = ts ? *ts : 0;
}

int cbm_compute_change_coupling(const cbm_commit_files_t *commits, int commit_count,
                                cbm_change_coupling_t *out, int max_out) {
    CBMHashTable *file_counts = cbm_ht_create(CBM_SZ_1K);
    CBMHashTable *pair_counts = cbm_ht_create(CBM_SZ_2K);
    /* Parallel table mapping pair_key → max commit timestamp seen for that
     * pair, so the resulting edge can carry last_co_change. The pair_counts
     * table consumes its key on insert; pair_timestamps gets its own copy. */
    CBMHashTable *pair_timestamps = cbm_ht_create(CBM_SZ_2K);
    if (!file_counts || !pair_counts || !pair_timestamps || commit_count < 0 || max_out < 0 ||
        (max_out > 0 && !out)) {
        cbm_ht_free(file_counts);
        cbm_ht_free(pair_counts);
        cbm_ht_free(pair_timestamps);
        return CBM_NOT_FOUND;
    }

    bool failed = false;
    int output_count = 0;

    for (int c = 0; c < commit_count; c++) {
        if (commits[c].count > GH_MAX_FILES) {
            continue;
        }

        for (int i = 0; i < commits[c].count; i++) {
            int *val = cbm_ht_get(file_counts, commits[c].files[i]);
            if (val) {
                (*val)++;
            } else {
                int *nv = malloc(sizeof(int));
                char *key = strdup(commits[c].files[i]);
                if (!nv || !key) {
                    free(nv);
                    free(key);
                    failed = true;
                    goto cleanup;
                }
                *nv = SKIP_ONE;
                cbm_ht_set(file_counts, key, nv);
                if (cbm_ht_get(file_counts, key) != nv) {
                    free(key);
                    free(nv);
                    failed = true;
                    goto cleanup;
                }
            }
        }

        for (int i = 0; i < commits[c].count; i++) {
            for (int j = i + SKIP_ONE; j < commits[c].count; j++) {
                const char *a = commits[c].files[i];
                const char *b = commits[c].files[j];
                if (strcmp(a, b) > 0) {
                    const char *t = a;
                    a = b;
                    b = t;
                }
                size_t la = strlen(a);
                size_t lb = strlen(b);
                if (la > SIZE_MAX - lb - PAIR_LEN) {
                    failed = true;
                    goto cleanup;
                }
                size_t pk_len = la + SKIP_ONE + lb + SKIP_ONE;
                char *pk = malloc(pk_len);
                if (!pk) {
                    failed = true;
                    goto cleanup;
                }
                memcpy(pk, a, la);
                pk[la] = '\x01';
                memcpy(pk + la + SKIP_ONE, b, lb + SKIP_ONE);

                int *val = cbm_ht_get(pair_counts, pk);
                if (val) {
                    (*val)++;
                    long long *ts = cbm_ht_get(pair_timestamps, pk);
                    if (ts && commits[c].timestamp > *ts) {
                        *ts = commits[c].timestamp;
                    }
                    free(pk);
                } else {
                    int *nv = malloc(sizeof(int));
                    char *pk2 = malloc(pk_len);
                    long long *nts = malloc(sizeof(long long));
                    if (!nv || !pk2 || !nts) {
                        free(nv);
                        free(pk2);
                        free(nts);
                        free(pk);
                        failed = true;
                        goto cleanup;
                    }
                    *nv = SKIP_ONE;
                    /* pair_counts takes ownership of pk; pair_timestamps
                     * needs its own copy. */
                    memcpy(pk2, pk, pk_len);
                    cbm_ht_set(pair_counts, pk, nv);
                    if (cbm_ht_get(pair_counts, pk) != nv) {
                        free(pk);
                        free(pk2);
                        free(nv);
                        free(nts);
                        failed = true;
                        goto cleanup;
                    }
                    *nts = commits[c].timestamp;
                    cbm_ht_set(pair_timestamps, pk2, nts);
                    if (cbm_ht_get(pair_timestamps, pk2) != nts) {
                        free(pk2);
                        free(nts);
                        failed = true;
                        goto cleanup;
                    }
                }
            }
        }
    }

    collect_coupling_ctx_t cctx = {
        .file_counts = file_counts,
        .pair_timestamps = pair_timestamps,
        .out = out,
        .out_count = 0,
        .max_out = max_out,
    };
    cbm_ht_foreach(pair_counts, collect_coupling_cb, &cctx);

    failed = cctx.failed;
    output_count = cctx.out_count;

cleanup:
    cbm_ht_foreach(pair_counts, free_counter, NULL);
    cbm_ht_free(pair_counts);
    cbm_ht_foreach(pair_timestamps, free_counter, NULL);
    cbm_ht_free(pair_timestamps);
    cbm_ht_foreach(file_counts, free_counter, NULL);
    cbm_ht_free(file_counts);

    return failed ? CBM_NOT_FOUND : output_count;
}

/* ── Split pass: compute (I/O-bound) + apply (gbuf writes) ───────── */

/* Pre-computed coupling result buffer for fused post-pass parallelism. */
#define MAX_COUPLINGS 8192
#define MAX_FILE_TEMPORAL 16384

/* Compute change couplings without touching the graph buffer.
 * Can run on a separate thread while other passes use the gbuf. */
int cbm_pipeline_githistory_compute_at(const char *repo_path, const char *revision,
                                       cbm_githistory_result_t *result) {
    if (!result) {
        return CBM_NOT_FOUND;
    }
    result->couplings = NULL;
    result->count = 0;
    result->commit_count = 0;
    result->file_temporal = NULL;
    result->file_temporal_count = 0;
    result->input_sha256[0] = '\0';

    commit_t *commits = NULL;
    int commit_count = 0;
    int rc = parse_git_log(repo_path, revision, &commits, &commit_count, result->input_sha256);
    if (rc != 0) {
        free_commits(commits, commit_count);
        return CBM_NOT_FOUND;
    }
    if (commit_count == 0) {
        free_commits(commits, commit_count);
        return 0;
    }

    result->commit_count = commit_count;

    /* Convert to testable format */
    cbm_commit_files_t *cf = calloc((size_t)commit_count, sizeof(cbm_commit_files_t));
    if (!cf) {
        for (int c = 0; c < commit_count; c++) {
            commit_free(&commits[c]);
        }
        free(commits);
        result->input_sha256[0] = '\0';
        return CBM_NOT_FOUND;
    }
    for (int c = 0; c < commit_count; c++) {
        cf[c].files = commits[c].files;
        cf[c].count = commits[c].count;
        cf[c].timestamp = commits[c].timestamp;
    }

    cbm_change_coupling_t *couplings = malloc(MAX_COUPLINGS * sizeof(cbm_change_coupling_t));
    if (!couplings) {
        free(cf);
        free_commits(commits, commit_count);
        result->input_sha256[0] = '\0';
        return CBM_NOT_FOUND;
    }
    int coupling_count = cbm_compute_change_coupling(cf, commit_count, couplings, MAX_COUPLINGS);
    if (coupling_count < 0) {
        free(couplings);
        free(cf);
        free_commits(commits, commit_count);
        result->input_sha256[0] = '\0';
        return CBM_NOT_FOUND;
    }

    /* Per-file temporal aggregation: change_count + last_modified.
     * Single hash-table pass over the same commit set used for coupling so
     * we don't re-scan history. Any allocation/truncation failure aborts the
     * generation; a completed snapshot may not silently omit temporal data. */
    cbm_file_temporal_t *ft_arr = malloc(MAX_FILE_TEMPORAL * sizeof(cbm_file_temporal_t));
    CBMHashTable *file_idx = cbm_ht_create(CBM_SZ_1K);
    if (!ft_arr || !file_idx) {
        free(ft_arr);
        cbm_ht_free(file_idx);
        free(couplings);
        free(cf);
        free_commits(commits, commit_count);
        result->input_sha256[0] = '\0';
        return CBM_NOT_FOUND;
    }
    int ft_count = 0;
    bool temporal_failed = false;
    for (int c = 0; c < commit_count && !temporal_failed; c++) {
        if (cf[c].count > GH_MAX_FILES) {
            continue;
        }
        for (int f = 0; f < cf[c].count; f++) {
            const char *fp = cf[c].files[f];
            int *idx = cbm_ht_get(file_idx, fp);
            if (idx) {
                if (ft_arr[*idx].change_count == INT_MAX) {
                    temporal_failed = true;
                    break;
                }
                ft_arr[*idx].change_count++;
                if (cf[c].timestamp > ft_arr[*idx].last_modified) {
                    ft_arr[*idx].last_modified = cf[c].timestamp;
                }
            } else {
                if (ft_count >= MAX_FILE_TEMPORAL || strlen(fp) >= sizeof(ft_arr[0].file_path)) {
                    temporal_failed = true;
                    break;
                }
                int *nidx = malloc(sizeof(int));
                char *key = strdup(fp);
                if (!nidx || !key) {
                    free(nidx);
                    free(key);
                    temporal_failed = true;
                    break;
                }
                int new_idx = ft_count;
                memcpy(ft_arr[new_idx].file_path, fp, strlen(fp) + 1U);
                ft_arr[new_idx].change_count = 1;
                ft_arr[new_idx].last_modified = cf[c].timestamp;
                *nidx = new_idx;
                cbm_ht_set(file_idx, key, nidx);
                if (cbm_ht_get(file_idx, key) != nidx) {
                    free(nidx);
                    free(key);
                    temporal_failed = true;
                    break;
                }
                ft_count++;
            }
        }
    }
    cbm_ht_foreach(file_idx, free_counter, NULL);
    cbm_ht_free(file_idx);
    if (temporal_failed) {
        free(ft_arr);
        free(couplings);
        free(cf);
        free_commits(commits, commit_count);
        result->input_sha256[0] = '\0';
        return CBM_NOT_FOUND;
    }
    result->file_temporal = ft_arr;
    result->file_temporal_count = ft_count;

    free(cf);
    free_commits(commits, commit_count);

    result->couplings = couplings;
    result->count = coupling_count;
    return 0;
}

int cbm_pipeline_githistory_compute(const char *repo_path, cbm_githistory_result_t *result) {
    return cbm_pipeline_githistory_compute_at(repo_path, NULL, result);
}

/* Apply pre-computed couplings to the graph buffer (must be on main thread). */
int cbm_pipeline_githistory_apply(cbm_pipeline_ctx_t *ctx, const cbm_githistory_result_t *result) {
    int edge_count = 0;

    for (int i = 0; i < result->count; i++) {
        const cbm_change_coupling_t *cc = &result->couplings[i];

        char *qn_a = cbm_pipeline_fqn_compute(ctx->project_name, cc->file_a, "__file__");
        char *qn_b = cbm_pipeline_fqn_compute(ctx->project_name, cc->file_b, "__file__");

        const cbm_gbuf_node_t *node_a = cbm_gbuf_find_by_qn(ctx->gbuf, qn_a);
        const cbm_gbuf_node_t *node_b = cbm_gbuf_find_by_qn(ctx->gbuf, qn_b);

        free(qn_a);
        free(qn_b);

        if (!node_a || !node_b || node_a->id == node_b->id) {
            continue;
        }

        char props[CBM_SZ_128];
        snprintf(props, sizeof(props),
                 "{\"co_changes\":%d,\"coupling_score\":%.2f,\"last_co_change\":%lld}",
                 cc->co_change_count, cc->coupling_score, cc->last_co_change);

        cbm_gbuf_insert_edge(ctx->gbuf, node_a->id, node_b->id, "FILE_CHANGES_WITH", props);
        edge_count++;
    }

    /* Apply per-file temporal metadata to existing File nodes so callers
     * can query change_count / last_modified for hotspot analysis. The
     * extension is re-derived and JSON-escaped to keep the props blob
     * well-formed even for paths with quotes or backslashes. */
    for (int i = 0; i < result->file_temporal_count; i++) {
        const cbm_file_temporal_t *ft = &result->file_temporal[i];
        char *qn = cbm_pipeline_fqn_compute(ctx->project_name, ft->file_path, "__file__");
        const cbm_gbuf_node_t *node = cbm_gbuf_find_by_qn(ctx->gbuf, qn);
        free(qn);
        if (!node) {
            continue;
        }

        const char *base = strrchr(ft->file_path, '/');
        base = base ? base + SKIP_ONE : ft->file_path;
        const char *ext = strrchr(base, '.');
        char ext_escaped[CBM_SZ_64];
        cbm_json_escape(ext_escaped, (int)sizeof(ext_escaped), ext ? ext : "");

        char props[CBM_SZ_256];
        snprintf(props, sizeof(props),
                 "{\"extension\":\"%s\",\"last_modified\":%lld,\"change_count\":%d}", ext_escaped,
                 ft->last_modified, ft->change_count);

        cbm_gbuf_upsert_node(ctx->gbuf, node->label, node->name, node->qualified_name,
                             node->file_path, node->start_line, node->end_line, props);
    }

    return edge_count;
}

/* ── Main pass (original serial interface) ───────────────────────── */

int cbm_pipeline_pass_githistory(cbm_pipeline_ctx_t *ctx) {
    cbm_log_info("pass.start", "pass", "githistory");

    cbm_githistory_result_t result = {0};
    cbm_pipeline_githistory_compute(ctx->repo_path, &result);

    int edge_count = 0;
    if (result.count > 0 || result.file_temporal_count > 0) {
        edge_count = cbm_pipeline_githistory_apply(ctx, &result);
    }

    free(result.couplings);
    free(result.file_temporal);

    cbm_log_info("pass.done", "pass", "githistory", "commits", itoa_log(result.commit_count),
                 "edges", itoa_log(edge_count));
    return 0;
}
