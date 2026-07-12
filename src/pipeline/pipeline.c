/*
 * pipeline.c — Indexing pipeline orchestrator.
 *
 * Coordinates multi-pass indexing:
 *   1. Discover files
 *   2. Build structure (Project/Folder/Package/File nodes)
 *   3. Bulk load sources (read + LZ4 HC compress)
 *   4. Extract definitions (fused: extract + write nodes + build registry)
 *   5. Resolve imports, calls, usages, semantic edges
 *   6. Post-passes: tests, communities, HTTP links, git history
 *   7. Dump graph buffer to SQLite
 */
#include "foundation/constants.h"

enum { CBM_DIR_PERMS = 0755, PL_RING = 4, PL_RING_MASK = 3, PL_SEQ_PASSES = 6, PL_WAL_BUF = 1040 };
#include "pipeline/pipeline.h"
#include "pipeline/artifact.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/worker_pool.h"
#include "graph_buffer/graph_buffer.h"
#include "git/git_context.h"
#include "store/store.h"
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "foundation/platform.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "foundation/hash_table.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/profile.h"
#include "foundation/mem.h"
#include "foundation/sha256.h"
#include "foundation/limits.h"

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define PL_FULL_REINDEX_NEEDED INT_MIN

static inline void *intptr_to_ptr(intptr_t v) {
    void *p;
    memcpy(&p, &v, sizeof(p));
    return p;
}

/* ── Global index lock ─────────────────────────────────────────── */
/* Prevents concurrent pipeline runs on the same DB file.
 * Atomic spinlock: 0 = free, 1 = locked. */
static atomic_int g_pipeline_busy = 0;
static CBM_TLS bool g_pipeline_lock_owned = false;

bool cbm_pipeline_try_lock(void) {
    if (atomic_exchange(&g_pipeline_busy, 1) == 0) {
        g_pipeline_lock_owned = true;
        return true;
    }
    return false;
}

bool cbm_pipeline_lock_held_by_current_thread(void) {
    return g_pipeline_lock_owned;
}

#define LOCK_SPIN_NS 100000000 /* 100ms between lock retries */

void cbm_pipeline_lock(void) {
    while (atomic_exchange(&g_pipeline_busy, 1) != 0) {
        struct timespec ts = {0, LOCK_SPIN_NS};
        cbm_nanosleep(&ts, NULL);
    }
    g_pipeline_lock_owned = true;
}

void cbm_pipeline_unlock(void) {
    g_pipeline_lock_owned = false;
    atomic_store(&g_pipeline_busy, 0);
}

/* ── Internal state ──────────────────────────────────────────────── */

struct cbm_pipeline {
    char *repo_path;
    char *db_path;
    char *project_name;
    cbm_git_context_t git_ctx;
    char *branch_qn;
    cbm_index_mode_t mode;
    cbm_index_mode_t effective_mode;
    char *input_fingerprint_override;
    atomic_int cancelled;
    bool persistence; /* publish a verified .codebase-memory artifact generation */

    /* Indexing state (set during run) */
    cbm_gbuf_t *gbuf;
    cbm_registry_t *registry;

    /* Directory subtrees skipped during discovery (rel paths). Captured from
     * cbm_discover_ex so the MCP layer can report excluded subtrees (#411).
     * Owned by the pipeline; freed in cbm_pipeline_free. */
    char **excluded_dirs;
    int excluded_count;

    /* Individual files dropped by ignore rules during discovery (#963
     * "purposely not indexed" — by design, not failures). Stored entries are
     * capped in discovery; ignored_total keeps the uncapped count so
     * truncation stays explicit. Owned by the pipeline. */
    cbm_ignored_file_t *ignored_files;
    int ignored_count;
    int ignored_total;

    /* Per-file indexing failures (skipped files) surfaced via MCP/CLI/logfile
     * (Stage 2 / Track B). A skip is the expected handled outcome of a bad or
     * oversized file — the run still succeeds ("indexed"). Owned by the
     * pipeline; freed in cbm_pipeline_free. */
    cbm_file_error_t *file_errors;
    int file_errors_count;
    int file_errors_cap;

    /* User-defined extension overrides (loaded once per run) */
    cbm_userconfig_t *userconfig;
    cbm_userconfig_snapshot_t *userconfig_snapshot;
    cbm_discovery_snapshot_t *discovery_snapshot;

    /* Committed graph size at dump time (-1 = dump did not run). #334 gate axis. */
    int committed_nodes;
    int committed_edges;

    /* ADR (project_summaries) captured before a full-reindex DB delete, so it
     * can be restored after the rebuild. NULL when no ADR existed. Issue #516. */
    char *saved_adr;

    /* Content generations captured before a full extraction. The same digest
     * is checked against each CBMFileResult and the live path immediately
     * before snapshot publication. */
    cbm_file_version_snapshot_t *file_versions;
    int file_version_count;
    atomic_bool input_generation_failed;
};

static cbm_pipeline_snapshot_hook_fn g_snapshot_test_hook = NULL;
static void *g_snapshot_test_userdata = NULL;

void cbm_pipeline_set_snapshot_hook_for_test(cbm_pipeline_snapshot_hook_fn hook, void *userdata) {
    g_snapshot_test_hook = hook;
    g_snapshot_test_userdata = userdata;
}

void cbm_pipeline_run_snapshot_hook_for_test(void) {
    cbm_pipeline_snapshot_hook_fn hook = g_snapshot_test_hook;
    void *userdata = g_snapshot_test_userdata;
    /* One-shot by design: a failed assertion cannot leak the hook into the
     * next pipeline test/run. */
    g_snapshot_test_hook = NULL;
    g_snapshot_test_userdata = NULL;
    if (hook) {
        hook(userdata);
    }
}

/* ── Global pkgmap (one active pipeline at a time) ─────────────── */

static CBMHashTable *g_pkgmap = NULL;

CBMHashTable *cbm_pipeline_get_pkgmap(void) {
    return g_pkgmap;
}

void cbm_pipeline_set_pkgmap(CBMHashTable *map) {
    g_pkgmap = map;
}

/* ── Timing helper ──────────────────────────────────────────────── */

static double elapsed_ms(struct timespec start) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)(now.tv_sec - start.tv_sec) * CBM_MS_PER_SEC) +
           ((double)(now.tv_nsec - start.tv_nsec) / CBM_US_PER_SEC_F);
}

/* Format int to string for logging. Thread-safe via TLS rotating buffers. */
static const char *itoa_buf(int val) {
    static CBM_TLS char bufs[PL_RING][CBM_SZ_32];
    static CBM_TLS int idx = 0;
    int i = idx;
    idx = (idx + SKIP_ONE) & PL_RING_MASK;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", val);
    return bufs[i];
}

/* Log current + peak RSS at a pipeline phase boundary (memory profiling). */
static void log_phase_mem(const char *phase) {
    enum { PL_BYTES_PER_MB = 1024 * 1024 };
    cbm_log_info("mem.phase", "phase", phase, "rss_mb",
                 itoa_buf((int)(cbm_mem_rss() / PL_BYTES_PER_MB)), "peak_mb",
                 itoa_buf((int)(cbm_mem_peak_rss() / PL_BYTES_PER_MB)));
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

cbm_pipeline_t *cbm_pipeline_new(const char *repo_path, const char *db_path,
                                 cbm_index_mode_t mode) {
    if (!repo_path) {
        return NULL;
    }

    cbm_pipeline_t *p = calloc(CBM_ALLOC_ONE, sizeof(cbm_pipeline_t));
    if (!p) {
        return NULL;
    }

    p->repo_path = strdup(repo_path);
    p->db_path = db_path ? strdup(db_path) : NULL;
    p->project_name = cbm_project_name_from_path(repo_path);
    if (!p->repo_path || (db_path && !p->db_path) || !p->project_name ||
        cbm_git_context_resolve(repo_path, &p->git_ctx) != 0) {
        free(p->repo_path);
        free(p->db_path);
        free(p->project_name);
        cbm_git_context_free(&p->git_ctx);
        free(p);
        return NULL;
    }
    p->branch_qn = cbm_git_context_branch_qn(p->project_name, &p->git_ctx);
    if (!p->branch_qn) {
        free(p->repo_path);
        free(p->db_path);
        free(p->project_name);
        cbm_git_context_free(&p->git_ctx);
        free(p);
        return NULL;
    }
    p->mode = mode;
    p->effective_mode = mode;
    p->persistence = false;
    p->committed_nodes = -1;
    p->committed_edges = -1;
    atomic_init(&p->cancelled, 0);
    atomic_init(&p->input_generation_failed, false);

    return p;
}

void cbm_pipeline_set_persistence(cbm_pipeline_t *p, bool enabled) {
    if (p) {
        p->persistence = enabled;
    }
}

bool cbm_pipeline_set_project_name(cbm_pipeline_t *p, const char *name) {
    if (!p || !name || !name[0]) {
        return false;
    }

    char *normalized = cbm_project_name_from_path(name);
    if (!normalized) {
        return false;
    }
    if (!cbm_validate_project_name(normalized)) {
        free(normalized);
        return false;
    }

    char *branch_qn = cbm_git_context_branch_qn(normalized, &p->git_ctx);
    if (!branch_qn) {
        free(normalized);
        return false;
    }
    free(p->project_name);
    p->project_name = normalized;
    free(p->branch_qn);
    free(p->input_fingerprint_override);
    p->input_fingerprint_override = NULL;
    p->branch_qn = branch_qn;
    return true;
}

void cbm_pipeline_free(cbm_pipeline_t *p) {
    if (!p) {
        return;
    }
    free(p->repo_path);
    free(p->db_path);
    free(p->project_name);
    cbm_discover_free_excluded(p->excluded_dirs, p->excluded_count);
    p->excluded_dirs = NULL;
    p->excluded_count = 0;
    cbm_discover_free_ignored(p->ignored_files, p->ignored_count);
    p->ignored_files = NULL;
    p->ignored_count = 0;
    p->ignored_total = 0;
    for (int i = 0; i < p->file_errors_count; i++) {
        free(p->file_errors[i].path);
        free(p->file_errors[i].reason);
        free(p->file_errors[i].phase);
    }
    free(p->file_errors);
    p->file_errors = NULL;
    p->file_errors_count = 0;
    p->file_errors_cap = 0;
    free(p->branch_qn);
    free(p->input_fingerprint_override);
    free(p->saved_adr); /* freed here too: error paths can exit before the
                         * restore in dump_and_persist_hashes runs. Issue #516. */
    p->saved_adr = NULL;
    free(p->file_versions);
    p->file_versions = NULL;
    p->file_version_count = 0;
    cbm_git_context_free(&p->git_ctx);
    /* gbuf, store, registry freed during/after run */
    /* Defensively free userconfig in case run() was never called or panicked */
    if (p->userconfig) {
        cbm_set_user_lang_config(NULL);
        cbm_userconfig_free(p->userconfig);
        p->userconfig = NULL;
    }
    cbm_userconfig_snapshot_free(p->userconfig_snapshot);
    p->userconfig_snapshot = NULL;
    cbm_discovery_snapshot_free(p->discovery_snapshot);
    p->discovery_snapshot = NULL;
    free(p);
}

void cbm_pipeline_cancel(cbm_pipeline_t *p) {
    if (p) {
        atomic_store(&p->cancelled, 1);
    }
}

const char *cbm_pipeline_project_name(const cbm_pipeline_t *p) {
    return p ? p->project_name : NULL;
}

const char *cbm_pipeline_repo_path(const cbm_pipeline_t *p) {
    return p ? p->repo_path : NULL;
}

cbm_userconfig_snapshot_t *cbm_pipeline_userconfig_snapshot(cbm_pipeline_t *p) {
    return p ? p->userconfig_snapshot : NULL;
}

const char *cbm_pipeline_userconfig_fingerprint(const cbm_pipeline_t *p) {
    return p ? (p->input_fingerprint_override
                    ? p->input_fingerprint_override
                    : cbm_userconfig_snapshot_fingerprint(p->userconfig_snapshot))
             : NULL;
}

const char *cbm_pipeline_config_fingerprint(const cbm_pipeline_t *p) {
    return p ? cbm_userconfig_snapshot_config_fingerprint(p->userconfig_snapshot) : NULL;
}

bool cbm_pipeline_input_generation_ok(const cbm_pipeline_t *p) {
    return p && !atomic_load(&p->input_generation_failed);
}

int cbm_pipeline_verify_input_snapshot(cbm_pipeline_t *p) {
    if (!p || !p->userconfig_snapshot ||
        cbm_userconfig_snapshot_verify(p->userconfig_snapshot) != 0) {
        return CBM_STORE_ERR;
    }
    const char *expected = cbm_userconfig_snapshot_git_context_sha256(p->userconfig_snapshot);
    if (!expected) {
        return CBM_STORE_ERR;
    }
    cbm_git_context_t live = {0};
    char live_sha256[CBM_SHA256_HEX_LEN + 1] = "";
    int rc = cbm_git_context_resolve(p->repo_path, &live);
    if (rc == 0) {
        rc = cbm_git_context_fingerprint(&live, live_sha256);
    }
    bool matches = rc == 0 && strcmp(expected, live_sha256) == 0;
    cbm_git_context_free(&live);
    if (!matches) {
        atomic_store(&p->input_generation_failed, true);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_pipeline_read_selected_source(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *file,
                                      size_t max_bytes, char **out_source, size_t *out_len,
                                      char out_sha256[CBM_SHA256_HEX_LEN + 1]) {
    if (out_source) {
        *out_source = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!ctx || !file || !file->path || !file->rel_path || !out_source || !out_len || !out_sha256) {
        if (ctx && ctx->pipeline) {
            atomic_store(&ctx->pipeline->input_generation_failed, true);
        }
        return CBM_STORE_ERR;
    }
    if (max_bytes == 0) {
        long configured = cbm_max_file_bytes();
        max_bytes = configured > 0 ? (size_t)configured : 1U;
    }
    cbm_file_version_snapshot_t *selected = NULL;
    if (ctx->source_version_index) {
        selected =
            (cbm_file_version_snapshot_t *)cbm_ht_get(ctx->source_version_index, file->rel_path);
    } else if (ctx->source_version_files && ctx->source_versions && ctx->source_version_count > 0) {
        for (int i = 0; i < ctx->source_version_count; i++) {
            const char *selected_rel = ctx->source_version_files[i].rel_path;
            if (selected_rel && strcmp(selected_rel, file->rel_path) == 0) {
                selected = &ctx->source_versions[i];
                break;
            }
        }
    }
    if (selected && (selected->content_skipped ||
                     (selected->size > 0 && (uint64_t)selected->size > (uint64_t)max_bytes))) {
        return CBM_SHA256_FILE_SKIPPED;
    }
    int read_rc = cbm_sha256_file_read_hex(file->path, max_bytes, out_source, out_len, out_sha256);
    cbm_userconfig_snapshot_t *inputs =
        ctx->pipeline ? cbm_pipeline_userconfig_snapshot(ctx->pipeline) : NULL;
    bool valid = read_rc == CBM_SHA256_FILE_HASHED;
    if (valid && selected) {
        if (selected->verified) {
            valid = strcmp(selected->sha256, out_sha256) == 0;
        } else {
            memcpy(selected->sha256, out_sha256, sizeof(selected->sha256));
            selected->verified = true;
        }
    } else if (valid && inputs) {
        valid = cbm_userconfig_snapshot_verify_auxiliary_source(inputs, file->rel_path,
                                                                out_sha256) == 0;
    } else if (ctx->pipeline) {
        valid = false;
    }
    if (read_rc != CBM_SHA256_FILE_HASHED && inputs && !selected) {
        cbm_userconfig_snapshot_note_auxiliary_read_failure(inputs, file->rel_path);
    }
    if (!valid) {
        free(*out_source);
        *out_source = NULL;
        *out_len = 0;
        if (ctx->pipeline) {
            atomic_store(&ctx->pipeline->input_generation_failed, true);
        }
        return read_rc == CBM_SHA256_FILE_OUT_OF_MEMORY ? CBM_SHA256_FILE_OUT_OF_MEMORY
                                                        : CBM_STORE_ERR;
    }

    if (*out_len > SIZE_MAX - CBM_SOURCE_LOOKAHEAD_PAD) {
        free(*out_source);
        *out_source = NULL;
        *out_len = 0;
        if (ctx->pipeline) {
            atomic_store(&ctx->pipeline->input_generation_failed, true);
        }
        return CBM_SHA256_FILE_OUT_OF_MEMORY;
    }
    char *padded = realloc(*out_source, *out_len + CBM_SOURCE_LOOKAHEAD_PAD);
    if (!padded) {
        free(*out_source);
        *out_source = NULL;
        *out_len = 0;
        if (ctx->pipeline) {
            atomic_store(&ctx->pipeline->input_generation_failed, true);
        }
        return CBM_SHA256_FILE_OUT_OF_MEMORY;
    }
    memset(padded + *out_len, 0, CBM_SOURCE_LOOKAHEAD_PAD);
    *out_source = padded;
    return CBM_STORE_OK;
}

typedef struct {
    const char *path;
    const char *detail;
    int language;
} discovery_selection_t;

static int discovery_selection_cmp(const void *left_ptr, const void *right_ptr) {
    const discovery_selection_t *left = (const discovery_selection_t *)left_ptr;
    const discovery_selection_t *right = (const discovery_selection_t *)right_ptr;
    int path_cmp = strcmp(left->path ? left->path : "", right->path ? right->path : "");
    if (path_cmp != 0) {
        return path_cmp;
    }
    int detail_cmp = strcmp(left->detail ? left->detail : "", right->detail ? right->detail : "");
    if (detail_cmp != 0) {
        return detail_cmp;
    }
    return (left->language > right->language) - (left->language < right->language);
}

static bool discovery_selection_equal(discovery_selection_t *left, int left_count,
                                      discovery_selection_t *right, int right_count) {
    if (left_count != right_count) {
        return false;
    }
    if (left_count > 1) {
        qsort(left, (size_t)left_count, sizeof(*left), discovery_selection_cmp);
        qsort(right, (size_t)right_count, sizeof(*right), discovery_selection_cmp);
    }
    for (int i = 0; i < left_count; i++) {
        if (discovery_selection_cmp(&left[i], &right[i]) != 0) {
            return false;
        }
    }
    return true;
}

int cbm_pipeline_verify_discovery_set(cbm_pipeline_t *p, const cbm_file_info_t *files,
                                      int file_count) {
    if (!p || file_count < 0 || (file_count > 0 && !files)) {
        return CBM_STORE_ERR;
    }
    if (p->discovery_snapshot) {
        if (cbm_discovery_snapshot_verify(p->discovery_snapshot)) {
            return CBM_STORE_OK;
        }
        atomic_store(&p->input_generation_failed, true);
        return CBM_STORE_ERR;
    }
    cbm_discover_opts_t opts = {.mode = p->effective_mode, .ignore_file = NULL, .max_file_size = 0};
    cbm_file_info_t *live_files = NULL;
    int live_file_count = 0;
    char **live_excluded = NULL;
    int live_excluded_count = 0;
    cbm_ignored_file_t *live_ignored = NULL;
    int live_ignored_count = 0;
    int live_ignored_total = 0;
    int rc = cbm_discover_ex2(p->repo_path, &opts, &live_files, &live_file_count, &live_excluded,
                              &live_excluded_count, &live_ignored, &live_ignored_count,
                              &live_ignored_total);
    if (rc != 0) {
        return CBM_STORE_ERR;
    }

    size_t original_cap = (size_t)(file_count + p->excluded_count + p->ignored_count);
    size_t live_cap = (size_t)(live_file_count + live_excluded_count + live_ignored_count);
    discovery_selection_t *original =
        (discovery_selection_t *)calloc(original_cap > 0 ? original_cap : 1, sizeof(*original));
    discovery_selection_t *live =
        (discovery_selection_t *)calloc(live_cap > 0 ? live_cap : 1, sizeof(*live));
    bool matches = original && live && p->ignored_total == live_ignored_total;
    if (matches) {
        int original_count = 0;
        int live_count = 0;
        for (int i = 0; i < file_count; i++) {
            original[original_count++] = (discovery_selection_t){
                .path = files[i].rel_path, .detail = "file", .language = (int)files[i].language};
        }
        for (int i = 0; i < p->excluded_count; i++) {
            original[original_count++] = (discovery_selection_t){
                .path = p->excluded_dirs[i], .detail = "excluded", .language = -1};
        }
        for (int i = 0; i < p->ignored_count; i++) {
            original[original_count++] =
                (discovery_selection_t){.path = p->ignored_files[i].rel_path,
                                        .detail = p->ignored_files[i].reason,
                                        .language = -1};
        }
        for (int i = 0; i < live_file_count; i++) {
            live[live_count++] = (discovery_selection_t){.path = live_files[i].rel_path,
                                                         .detail = "file",
                                                         .language = (int)live_files[i].language};
        }
        for (int i = 0; i < live_excluded_count; i++) {
            live[live_count++] = (discovery_selection_t){
                .path = live_excluded[i], .detail = "excluded", .language = -1};
        }
        for (int i = 0; i < live_ignored_count; i++) {
            live[live_count++] = (discovery_selection_t){
                .path = live_ignored[i].rel_path, .detail = live_ignored[i].reason, .language = -1};
        }
        matches = discovery_selection_equal(original, original_count, live, live_count);
    }
    free(original);
    free(live);
    cbm_discover_free(live_files, live_file_count);
    cbm_discover_free_excluded(live_excluded, live_excluded_count);
    cbm_discover_free_ignored(live_ignored, live_ignored_count);
    return matches ? CBM_STORE_OK : CBM_STORE_ERR;
}

atomic_int *cbm_pipeline_cancelled_ptr(cbm_pipeline_t *p) {
    return p ? &p->cancelled : NULL;
}

int cbm_pipeline_get_mode(const cbm_pipeline_t *p) {
    return p ? (int)p->mode : 0;
}

int cbm_pipeline_get_effective_mode(const cbm_pipeline_t *p) {
    return p ? (int)p->effective_mode : -1;
}

void cbm_pipeline_get_excluded(const cbm_pipeline_t *p, char ***out, int *count) {
    if (out) {
        *out = p ? p->excluded_dirs : NULL;
    }
    if (count) {
        *count = p ? p->excluded_count : 0;
    }
}

/* NULL-safe heap strdup (avoids a strdup dependency + guards NULL inputs). */
static char *fe_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

void cbm_pipeline_add_file_error(cbm_pipeline_t *p, const char *path, const char *reason,
                                 const char *phase) {
    if (!p) {
        return;
    }
    if (p->file_errors_count >= p->file_errors_cap) {
        int ncap = p->file_errors_cap ? p->file_errors_cap * 2 : 16;
        cbm_file_error_t *grown =
            (cbm_file_error_t *)realloc(p->file_errors, (size_t)ncap * sizeof(*grown));
        if (!grown) {
            /* Never abort indexing just to record a skip — drop this record. */
            return;
        }
        p->file_errors = grown;
        p->file_errors_cap = ncap;
    }
    cbm_file_error_t *e = &p->file_errors[p->file_errors_count];
    e->path = fe_strdup(path);
    e->reason = fe_strdup(reason);
    e->phase = fe_strdup(phase);
    p->file_errors_count++;
}

void cbm_pipeline_get_file_errors(const cbm_pipeline_t *p, cbm_file_error_t **out, int *count) {
    if (out) {
        *out = p ? p->file_errors : NULL;
    }
    if (count) {
        *count = p ? p->file_errors_count : 0;
    }
}

void cbm_pipeline_get_ignored(const cbm_pipeline_t *p, cbm_ignored_file_t **out, int *count,
                              int *total) {
    if (out) {
        *out = p ? p->ignored_files : NULL;
    }
    if (count) {
        *count = p ? p->ignored_count : 0;
    }
    if (total) {
        *total = p ? p->ignored_total : 0;
    }
}

void cbm_pipeline_get_committed_counts(const cbm_pipeline_t *p, int *nodes, int *edges) {
    if (nodes) {
        *nodes = p ? p->committed_nodes : -1;
    }
    if (edges) {
        *edges = p ? p->committed_edges : -1;
    }
}

void cbm_pipeline_set_committed_counts(cbm_pipeline_t *p, int nodes, int edges) {
    if (p) {
        p->committed_nodes = nodes;
        p->committed_edges = edges;
    }
}

/* Effective worker count. The crash supervisor re-runs its worker single-
 * threaded (CBM_INDEX_SINGLE_THREAD=1) so a per-file marker can pin the EXACT
 * crasher; a parallel re-run would race the marker. Honour that override
 * everywhere the worker count drives the parallel/sequential decision, so the
 * whole extraction phase collapses to the deterministic sequential path. */
static char *resolve_db_path(const cbm_pipeline_t *p);

static int effective_worker_count(bool initial) {
    const char *st = getenv("CBM_INDEX_SINGLE_THREAD");
    if (st && st[0] == '1') {
        return 1;
    }
    return cbm_default_worker_count(initial);
}

/* A request for a cheaper mode must never replace an already-richer completed
 * graph with a subset when a base input changed. Select the execution
 * capability before discovery: existing Full/Moderate stores continue to be
 * discovered and recomputed at that capability, while p->mode remains the
 * caller's requested mode for compatibility routing. Unknown legacy metadata
 * is treated conservatively as Full. */
static void select_effective_mode(cbm_pipeline_t *p) {
    p->effective_mode = p->mode;
    if (p->mode == CBM_MODE_FULL) {
        return;
    }
    char *db_path = resolve_db_path(p);
    if (!db_path) {
        return;
    }
    if (!cbm_file_exists(db_path)) {
        free(db_path);
        return;
    }
    p->effective_mode = CBM_MODE_FULL;
    cbm_store_t *store = cbm_store_open_path(db_path);
    free(db_path);
    if (!store) {
        return;
    }
    int stored_mode = -1;
    if (cbm_store_get_index_mode(store, p->project_name, &stored_mode) == CBM_STORE_OK &&
        stored_mode >= CBM_MODE_FULL && stored_mode <= CBM_MODE_FAST && stored_mode < p->mode) {
        p->effective_mode = (cbm_index_mode_t)stored_mode;
    } else if (stored_mode == p->mode) {
        p->effective_mode = p->mode;
    }
    cbm_store_close(store);
}

/* Resolve the DB path for this pipeline. Caller must free(). */
static char *resolve_db_path(const cbm_pipeline_t *p) {
    if (p->db_path) {
        return strdup(p->db_path);
    }
    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir || !p->project_name) {
        return NULL;
    }
    size_t dir_len = strlen(cache_dir);
    size_t project_len = strlen(p->project_name);
    if (project_len > SIZE_MAX - sizeof("/.db") ||
        dir_len > SIZE_MAX - project_len - sizeof("/.db")) {
        return NULL;
    }
    size_t path_len = dir_len + project_len + sizeof("/.db");
    char *path = malloc(path_len);
    if (!path) {
        return NULL;
    }
    int written = snprintf(path, path_len, "%s/%s.db", cache_dir, p->project_name);
    if (written < 0 || (size_t)written >= path_len) {
        free(path);
        return NULL;
    }
    return path;
}

static int check_cancel(const cbm_pipeline_t *p) {
    return atomic_load(&p->cancelled) ? CBM_NOT_FOUND : 0;
}

/* ── Hash table cleanup callback ─────────────────────────────────── */

static void free_seen_dir_key(const char *key, void *val, void *ud) {
    (void)val;
    (void)ud;
    free((void *)key);
}

/* ── Pass 1: Structure ──────────────────────────────────────────── */

/* Create Project, Folder/Package, and File nodes in the graph buffer. */
/* Walk directory chain upward, creating Folder nodes and CONTAINS_FOLDER edges. */
static void create_folder_chain(cbm_pipeline_t *p, const char *dir, CBMHashTable *seen_dirs) {
    char *walk = strdup(dir);
    while (walk[0] != '\0' && !cbm_ht_get(seen_dirs, walk)) {
        cbm_ht_set(seen_dirs, strdup(walk), intptr_to_ptr(SKIP_ONE));
        char *folder_qn = cbm_pipeline_fqn_folder(p->project_name, walk);
        const char *dir_base = strrchr(walk, '/');
        dir_base = dir_base ? dir_base + SKIP_ONE : walk;
        cbm_gbuf_upsert_node(p->gbuf, "Folder", dir_base, folder_qn, walk, 0, 0, "{}");

        char *pdir = strdup(walk);
        char *ps = strrchr(pdir, '/');
        if (ps) {
            *ps = '\0';
        } else {
            free(pdir);
            pdir = strdup("");
        }
        const char *pqn;
        char *pqn_heap = NULL;
        if (pdir[0] == '\0') {
            pqn = p->branch_qn ? p->branch_qn : p->project_name;
        } else {
            pqn_heap = cbm_pipeline_fqn_folder(p->project_name, pdir);
            pqn = pqn_heap;
        }
        const cbm_gbuf_node_t *fn = cbm_gbuf_find_by_qn(p->gbuf, folder_qn);
        const cbm_gbuf_node_t *pn = cbm_gbuf_find_by_qn(p->gbuf, pqn);
        if (fn && pn) {
            cbm_gbuf_insert_edge(p->gbuf, pn->id, fn->id, "CONTAINS_FOLDER", "{}");
        }
        free(folder_qn);
        free(pqn_heap);
        char *up = strrchr(walk, '/');
        if (up) {
            *up = '\0';
        } else {
            walk[0] = '\0';
        }
        free(pdir);
    }
    free(walk);
}

static int pass_structure(cbm_pipeline_t *p, const cbm_file_info_t *files, int file_count) {
    cbm_log_info("pass.start", "pass", "structure", "files", itoa_buf(file_count));

    /* Project node */
    cbm_gbuf_upsert_node(p->gbuf, "Project", p->project_name, p->project_name, NULL, 0, 0, "{}");
    const char *branch_qn = p->branch_qn ? p->branch_qn : p->project_name;
    const char *branch_name = p->git_ctx.branch ? p->git_ctx.branch : "working-tree";
    char branch_props[CBM_SZ_2K];
    const char *branch_props_json = "{}";
    if (cbm_git_context_props_json(&p->git_ctx, branch_props, sizeof(branch_props)) > 0) {
        branch_props_json = branch_props;
    }
    if (p->branch_qn) {
        int64_t branch_id = cbm_gbuf_upsert_node(p->gbuf, "Branch", branch_name, branch_qn, NULL, 0,
                                                 0, branch_props_json);
        const cbm_gbuf_node_t *project_node = cbm_gbuf_find_by_qn(p->gbuf, p->project_name);
        if (project_node && branch_id > 0) {
            cbm_gbuf_insert_edge(p->gbuf, project_node->id, branch_id, "HAS_BRANCH",
                                 branch_props_json);
        }
    }

    /* Collect unique directories and create Folder/Package nodes */
    CBMHashTable *seen_dirs = cbm_ht_create(CBM_SZ_256);

    for (int i = 0; i < file_count; i++) {
        const char *rel = files[i].rel_path;
        if (!rel) {
            continue;
        }

        /* Create File node */
        char *file_qn = cbm_pipeline_fqn_compute(p->project_name, rel, "__file__");
        /* Extract basename */
        const char *slash = strrchr(rel, '/');
        const char *basename = slash ? slash + SKIP_ONE : rel;

        char props[CBM_SZ_256];
        const char *ext = strrchr(basename, '.');
        snprintf(props, sizeof(props), "{\"extension\":\"%s\"}", ext ? ext : "");

        const char *qualified_name = file_qn;
        const char *file_path = rel;
        cbm_gbuf_upsert_node(p->gbuf, "File", basename, qualified_name, file_path, 0, 0, props);

        /* CONTAINS_FILE edge: parent dir -> file */
        char *dir = strdup(rel);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            {
                *last_slash = '\0';
            }
        } else {
            free(dir);
            dir = strdup("");
        }

        const char *parent_qn;
        char *parent_qn_heap = NULL;
        if (dir[0] == '\0') {
            parent_qn = branch_qn;
        } else {
            parent_qn_heap = cbm_pipeline_fqn_folder(p->project_name, dir);
            parent_qn = parent_qn_heap;
        }

        /* Walk up directory chain, creating Folder nodes */
        create_folder_chain(p, dir, seen_dirs);

        /* Now create the CONTAINS_FILE edge */
        const cbm_gbuf_node_t *fnode = cbm_gbuf_find_by_qn(p->gbuf, file_qn);
        const cbm_gbuf_node_t *pnode = cbm_gbuf_find_by_qn(p->gbuf, parent_qn);
        if (fnode && pnode) {
            cbm_gbuf_insert_edge(p->gbuf, pnode->id, fnode->id, "CONTAINS_FILE", "{}");
        }

        free(file_qn);
        free(dir);
        free(parent_qn_heap);
    }

    /* Free seen_dirs keys */
    cbm_ht_foreach(seen_dirs, free_seen_dir_key, NULL);
    cbm_ht_free(seen_dirs);

    cbm_log_info("pass.done", "pass", "structure", "nodes", itoa_buf(cbm_gbuf_node_count(p->gbuf)),
                 "edges", itoa_buf(cbm_gbuf_edge_count(p->gbuf)));
    return 0;
}

/* ── Pass 2: Definitions ─────────────────────────────────────────── */

/* Implemented in pass_definitions.c via cbm_pipeline_pass_definitions() */

/* ── Githistory compute thread (for fused post-pass parallelism) ─── */

typedef struct {
    const char *repo_path;
    const char *revision;
    cbm_githistory_result_t *result;
    int rc;
} gh_compute_arg_t;

static void *gh_compute_thread_fn(void *arg) {
    gh_compute_arg_t *a = arg;
    a->rc = cbm_pipeline_githistory_compute_at(a->repo_path, a->revision, a->result);
    return NULL;
}

/* Extract Route nodes from URL strings found in config files (YAML, HCL, TOML).
 * These are infrastructure-defined endpoints (Cloud Scheduler, Terraform). */
/* Process infra bindings: topic→URL pairs from IaC configs.
 * Creates Route nodes for endpoints and HANDLES edges linking
 * topic Routes to endpoint Routes (bridging the gap). */
/* Process one infra binding: create Route node + INFRA_MAPS edge. */
static int process_one_infra_binding(cbm_gbuf_t *gbuf, const CBMInfraBinding *ib,
                                     const char *rel_path) {
    char url_route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(url_route_qn, sizeof(url_route_qn), "__route__infra__%s", ib->target_url);
    int64_t url_route_id = cbm_gbuf_upsert_node(gbuf, "Route", ib->target_url, url_route_qn,
                                                rel_path, 0, 0, "{\"source\":\"infra\"}");
    char topic_route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(topic_route_qn, sizeof(topic_route_qn), "__route__%s__%s",
             ib->broker ? ib->broker : "async", ib->source_name);
    const cbm_gbuf_node_t *topic_route = cbm_gbuf_find_by_qn(gbuf, topic_route_qn);
    int64_t topic_route_id;
    if (topic_route) {
        topic_route_id = topic_route->id;
    } else {
        /* The config file IS the declaration that the topic/queue/schedule exists;
         * upsert its Route node so the binding maps even when no code-side dispatch
         * call created the node first (e.g. a standalone scheduler/subscription
         * manifest). */
        topic_route_id = cbm_gbuf_upsert_node(gbuf, "Route", ib->source_name, topic_route_qn,
                                              rel_path, 0, 0, ib->broker ? ib->broker : "async");
        if (topic_route_id <= 0) {
            return 0;
        }
    }
    char props[CBM_SZ_512];
    snprintf(props, sizeof(props), "{\"broker\":\"%s\",\"topic\":\"%s\",\"endpoint\":\"%s\"}",
             ib->broker ? ib->broker : "async", ib->source_name, ib->target_url);
    cbm_gbuf_insert_edge(gbuf, topic_route_id, url_route_id, "INFRA_MAPS", props);
    return SKIP_ONE;
}

static void cbm_pipeline_process_infra_bindings(cbm_gbuf_t *gbuf, const cbm_file_info_t *files,
                                                CBMFileResult **result_cache, int file_count) {
    int bindings = 0;
    for (int i = 0; i < file_count; i++) {
        if (!result_cache[i]) {
            continue;
        }
        for (int bi = 0; bi < result_cache[i]->infra_bindings.count; bi++) {
            const CBMInfraBinding *ib = &result_cache[i]->infra_bindings.items[bi];
            if (ib->source_name && ib->target_url) {
                bindings += process_one_infra_binding(gbuf, ib, files[i].rel_path);
            }
        }
    }
    if (bindings > 0) {
        char buf[CBM_SZ_16];
        snprintf(buf, sizeof(buf), "%d", bindings);
        cbm_log_info("pass.infra_bindings", "linked", buf);
    }
}

static bool is_infra_file(const char *fp) {
    return fp != NULL &&
           (strstr(fp, ".yaml") != NULL || strstr(fp, ".yml") != NULL ||
            strstr(fp, ".tf") != NULL || strstr(fp, ".hcl") != NULL || strstr(fp, ".toml") != NULL);
}

/* True when a YAML key path denotes an UPSTREAM dependency, CONFIG value, or
 * HEALTHCHECK target rather than an endpoint this service exposes. Such URLs
 * (auth JWKS, downstream service base URLs, package-registry URLs, healthcheck
 * curl targets) are NOT routes the service serves and must not mint Route nodes
 * (#521). Exposed-endpoint keys (push_endpoint, post_url, callback, webhook)
 * are intentionally absent here so they still produce infra Route nodes. */
static bool is_upstream_config_key(const char *key_path) {
    if (!key_path) {
        /* No key context (e.g. flat string) — keep prior behaviour and mint. */
        return false;
    }
    static const char *const deny[] = {"jwks",     "registry",     "registries", "healthcheck",
                                       "upstream", "_service_url", "auth",       NULL};
    for (int i = 0; deny[i]; i++) {
        if (strstr(key_path, deny[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* Try to create an infra Route node from one string_ref. */
static void try_upsert_infra_route(cbm_gbuf_t *gbuf, const CBMStringRef *sr, const char *fp) {
    if (sr->kind != CBM_STRREF_URL || !sr->value || !strstr(sr->value, "://")) {
        return;
    }
    /* Skip upstream/config/healthcheck URLs — they are not exposed routes (#521). */
    if (is_upstream_config_key(sr->key_path)) {
        return;
    }
    char route_qn[CBM_ROUTE_QN_SIZE];
    snprintf(route_qn, sizeof(route_qn), "__route__infra__%s", sr->value);
    char route_props[CBM_SZ_512];
    if (sr->key_path) {
        snprintf(route_props, sizeof(route_props), "{\"source\":\"infra\",\"key_path\":\"%s\"}",
                 sr->key_path);
    } else {
        snprintf(route_props, sizeof(route_props), "{\"source\":\"infra\"}");
    }
    cbm_gbuf_upsert_node(gbuf, "Route", sr->value, route_qn, fp, 0, 0, route_props);
}

/* A URL string_ref that does NOT denote a route the service serves: a value
 * containing whitespace is a command/sentence with an embedded URL (e.g. a
 * Docker healthcheck `curl --fail http://... || exit 1`); a NULL key_path is a
 * context-less/duplicate ref; an upstream/config/healthcheck key is an external
 * dependency, not an exposed route. (#521) */
static bool route_sr_denied(const CBMStringRef *sr) {
    if (!sr->value || strchr(sr->value, ' ')) {
        return true;
    }
    if (!sr->key_path) {
        return true;
    }
    return is_upstream_config_key(sr->key_path);
}

static void cbm_pipeline_extract_infra_routes(cbm_gbuf_t *gbuf, const cbm_file_info_t *files,
                                              CBMFileResult **result_cache, int file_count) {
    /* DENY-WINS-BY-VALUE: the same URL is often extracted as several string_refs
     * at different key_path granularities (full path, leaf key, flat). The Route
     * node is keyed by VALUE, so it would be minted if ANY granularity passed the
     * per-ref guard — e.g. a denied full path `registries.terraform-registry.url`
     * is defeated by a sibling leaf `url`. So pass 1 collects every URL value
     * denied under ANY of its refs; pass 2 mints only values never denied. (#521) */
    CBMHashTable *denied = cbm_ht_create(16);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < file_count; i++) {
            if (!result_cache[i] || !is_infra_file(files[i].rel_path)) {
                continue;
            }
            for (int si = 0; si < result_cache[i]->string_refs.count; si++) {
                const CBMStringRef *sr = &result_cache[i]->string_refs.items[si];
                if (sr->kind != CBM_STRREF_URL || !sr->value || !strstr(sr->value, "://")) {
                    continue;
                }
                if (pass == 0) {
                    if (denied && route_sr_denied(sr)) {
                        cbm_ht_set(denied, sr->value, (void *)1);
                    }
                } else if (!denied || !cbm_ht_has(denied, sr->value)) {
                    try_upsert_infra_route(gbuf, sr, files[i].rel_path);
                }
            }
        }
    }
    cbm_ht_free(denied);
}

/* Run decorator_tags, configlink, and route matching passes. */
typedef void (*predump_pass_fn)(cbm_pipeline_ctx_t *);
static void predump_deco(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_decorator_tags(ctx->gbuf, ctx->project_name);
}
static void predump_route(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_create_route_nodes(ctx->gbuf);
}
static void predump_sim(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_similarity(ctx);
}
static void predump_sem(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_semantic_edges(ctx);
}
static void predump_cfg(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_configlink(ctx);
}
static void predump_complexity(cbm_pipeline_ctx_t *ctx) {
    cbm_pipeline_pass_complexity(ctx);
}

static void run_predump_passes(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx) {
    static const struct {
        predump_pass_fn fn;
        const char *name;
        bool moderate_only; /* true = skip in fast mode */
    } passes[] = {
        {predump_deco, "decorator_tags", false}, {predump_cfg, "configlink", false},
        {predump_route, "route_match", false},   {predump_sim, "similarity", true},
        {predump_sem, "semantic_edges", true},   {predump_complexity, "complexity", false},
    };
    enum { PREDUMP_PASS_COUNT = 6 };
    struct timespec t;
    for (int i = 0; i < PREDUMP_PASS_COUNT && !check_cancel(p); i++) {
        /* "moderate_only" passes (similarity/semantic edges) run in FULL,
         * MODERATE and ADVANCED — they are skipped only in FAST. Compare
         * explicitly against FAST rather than `> MODERATE` so ADVANCED
         * (numerically 3) is not mistaken for a lighter mode than FULL. */
        if (passes[i].moderate_only && p->effective_mode == CBM_MODE_FAST) {
            continue;
        }
        cbm_clock_gettime(CLOCK_MONOTONIC, &t);
        passes[i].fn(ctx);
        cbm_log_info("pass.timing", "pass", passes[i].name, "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t)));
    }
}

/* Adapter that lets cbm_pipeline_pass_lsp_cross slot into the seq_passes
 * dispatch table. The cross-file LSP needs the per-file CBMFileResult cache
 * to read defs/imports without re-extracting; in the sequential path that
 * cache is ctx->result_cache (set up by run_sequential_pipeline before
 * launching the dispatch loop). When the cache is unavailable (e.g. if the
 * pipeline opted out of caching), the pass becomes a no-op since there are
 * no extracted results to feed cross-file resolution. */
static int seq_pass_lsp_cross_dispatch(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                                       int file_count) {
    if (!ctx || !ctx->result_cache)
        return 0;
    /* Cross-file LSP runs in every mode. */
    return cbm_pipeline_pass_lsp_cross(ctx, files, file_count, ctx->result_cache);
}

/* Run the sequential pipeline path: definitions, k8s, lsp_cross, calls, usages, semantic. */
static int run_sequential_pipeline(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                   const cbm_file_info_t *files, int file_count,
                                   struct timespec *t) {
    cbm_log_info("pipeline.mode", "mode", "sequential", "files", itoa_buf(file_count));

    /* Build package map from manifest files (sequential: read manifests directly).
     * Use the repo-walking variant so manifests filtered out by the main
     * discoverer (package.json, composer.json) still feed pkgmap and let
     * workspace imports like `@my/pkg` resolve to their target Module. */
    cbm_pipeline_set_pkgmap(cbm_pkgmap_build_from_repo_snapshot(
        ctx->repo_path, files, file_count, ctx->project_name, ctx->excluded_dirs,
        ctx->excluded_count, cbm_pipeline_userconfig_snapshot(p)));

    CBMFileResult **seq_cache = (CBMFileResult **)calloc(file_count, sizeof(CBMFileResult *));
    if (!seq_cache && file_count > 0) {
        cbm_log_error("pipeline.err", "phase", "cache_alloc");
        return CBM_STORE_ERR;
    }
    ctx->result_cache = seq_cache;
    typedef int (*seq_pass_fn)(cbm_pipeline_ctx_t *, const cbm_file_info_t *, int);
    static const struct {
        seq_pass_fn fn;
        const char *name;
        bool ignore_err;
    } seq_passes[] = {
        {cbm_pipeline_pass_definitions, "definitions", false},
        {cbm_pipeline_pass_k8s, "k8s", true},
        {seq_pass_lsp_cross_dispatch, "lsp_cross", true},
        {cbm_pipeline_pass_calls, "calls", false},
        {cbm_pipeline_pass_usages, "usages", false},
        {cbm_pipeline_pass_semantic, "semantic", false},
    };
    int rc = 0;
    for (int si = 0; si < PL_SEQ_PASSES && rc == 0; si++) {
        cbm_clock_gettime(CLOCK_MONOTONIC, t);
        int pr = seq_passes[si].fn(ctx, files, file_count);
        if (pr != 0 && !seq_passes[si].ignore_err) {
            rc = pr;
        }
        if (si == 0 && rc == 0 &&
            cbm_pipeline_verify_extracted_versions(files, file_count, p->file_versions,
                                                   seq_cache) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
        }
        cbm_log_info("pass.timing", "pass", seq_passes[si].name, "elapsed_ms",
                     itoa_buf((int)elapsed_ms(*t)));
        if (check_cancel(p)) {
            rc = CBM_NOT_FOUND;
        }
    }
    /* Consume infra bindings (YAML/HCL topic/queue/scheduler → endpoint) so
     * INFRA_MAPS edges also form on the sequential path, not just the parallel
     * one. process_one_infra_binding self-creates the topic Route node when no
     * code-side dispatch created it (e.g. a standalone scheduler manifest). */
    if (seq_cache && rc == 0) {
        cbm_pipeline_extract_infra_routes(p->gbuf, files, seq_cache, file_count);
        cbm_pipeline_process_infra_bindings(p->gbuf, files, seq_cache, file_count);
    }
    if (seq_cache) {
        for (int i = 0; i < file_count; i++) {
            if (seq_cache[i]) {
                cbm_free_result(seq_cache[i]);
            }
        }
        free(seq_cache);
        ctx->result_cache = NULL;
    }
    /* Release the lsp_cross pass's shared registries only now: resolved_calls
     * borrowed registry-owned strings that the calls pass read above. */
    if (ctx->seq_cross_arena_live) {
        cbm_arena_destroy(&ctx->seq_cross_arena);
        ctx->seq_cross_arena_live = false;
    }
    /* Destroy this thread's TLS parser: the sequential path parses on the
     * CALLING thread (usually main), and a parser left alive here was
     * allocated in the current tree-sitter allocator epoch. A later
     * parallel run switches the global ts allocator to the slab
     * (cbm_slab_install); destroying the stale parser then frees
     * mimalloc-epoch memory through slab_free -> plain free() and libmalloc
     * aborts — the #773 second-index SIGABRT. */
    cbm_destroy_thread_parser();
    return rc;
}

/* Run the parallel pipeline path: extract, registry, resolve, infra, k8s. */
static int run_parallel_pipeline(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                 const cbm_file_info_t *files, int file_count, int worker_count,
                                 struct timespec *t) {
    cbm_log_info("pipeline.mode", "mode", "parallel", "workers", itoa_buf(worker_count), "files",
                 itoa_buf(file_count));
    _Atomic int64_t shared_ids;
    atomic_init(&shared_ids, cbm_gbuf_next_id(p->gbuf));
    CBMFileResult **cache = (CBMFileResult **)calloc(file_count, sizeof(CBMFileResult *));
    if (!cache) {
        cbm_log_error("pipeline.err", "phase", "cache_alloc");
        return CBM_NOT_FOUND;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    int rc = cbm_parallel_extract(ctx, files, file_count, cache, &shared_ids, worker_count);
    cbm_log_info("pass.timing", "pass", "parallel_extract", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    if (rc == 0 && !check_cancel(p) &&
        cbm_pipeline_verify_extracted_versions(files, file_count, p->file_versions, cache) !=
            CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
    }
    if (rc != 0 || check_cancel(p)) {
        for (int i = 0; i < file_count; i++) {
            if (cache[i]) {
                cbm_free_result(cache[i]);
            }
        }
        free(cache);
        return rc != 0 ? rc : CBM_NOT_FOUND;
    }
    cbm_gbuf_set_next_id(p->gbuf, atomic_load(&shared_ids));
    /* extract -> registry handoff: return the extract phase's freed-but-retained
     * allocator pages to the OS before registry_build allocates. On a 2x Linux
     * index the extract peak holds ~13 GB of reclaimable pages (peak_mb 20.7 vs
     * live rss_mb 7); not returning them pushed the process over the system
     * memory-pressure threshold and got it SIGKILLed at registry entry. */
    cbm_mem_collect();
    cbm_log_info("mem.collect", "phase", "post_extract", "rss_mb",
                 itoa_buf((int)(cbm_mem_rss() / (1024 * 1024))));
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    rc = cbm_build_registry_from_cache(ctx, files, file_count, cache);
    cbm_log_info("pass.timing", "pass", "registry_build", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    log_phase_mem("registry_build");
    if (rc != 0 || check_cancel(p)) {
        for (int i = 0; i < file_count; i++) {
            if (cache[i]) {
                cbm_free_result(cache[i]);
            }
        }
        free(cache);
        return rc != 0 ? rc : CBM_NOT_FOUND;
    }
    /* Cross-file LSP precondition: build a project-wide CBMLSPDef[]
     * once. The fused resolve_worker invokes cbm_pxc_run_one(_ts) per
     * file using these defs + the file's IMPORTS map, so cross-file
     * type-resolved CALLS land in result->resolved_calls before the
     * CALLS-edge emission. This replaces the old sequential
     * cbm_pipeline_pass_lsp_cross pass which re-read every source from
     * disk and re-parsed every tree on a single thread (~520s on
     * kubernetes). Soft-failure: NULL all_defs / NULL def_modules just
     * mean cross-file LSP no-ops; per-file LSP already ran during
     * extract. */
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    /* Cross-file LSP (type-aware call/usage resolution across files) — the
     * most expensive phase. CBM_DISABLE_LSP_CROSS=1 opts out (it can SIGSEGV
     * on large TS projects — see #340/#344); with cross-LSP off, all_defs
     * stays NULL and the fused resolver simply no-ops cross-file resolution
     * (per-file LSP already ran during extract). */
    char cbm_lsp_cross_env[CBM_SZ_16];
    const bool run_cross_lsp = cbm_safe_getenv("CBM_DISABLE_LSP_CROSS", cbm_lsp_cross_env,
                                               sizeof(cbm_lsp_cross_env), NULL) == NULL;
    if (!run_cross_lsp) {
        cbm_log_info("lsp_cross.skipped", "reason", "CBM_DISABLE_LSP_CROSS env set");
    }
    char **def_modules = NULL;
    int def_count = 0;
    CBMLSPDef *all_defs = NULL;
    if (run_cross_lsp) {
        def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
        all_defs = def_modules
                       ? cbm_pxc_collect_all_defs(cache, files, file_count, ctx->project_name,
                                                  def_modules, &def_count)
                       : NULL;
    }
    /* Build inverted index: module_qn → defs. The fused resolve_worker
     * uses this to filter the global all_defs[] down to just the defs
     * each file actually needs (own_module + imported modules) — the
     * gopls "package summary" pattern. Drops per-file registry build
     * cost from O(all_defs) to O(relevant_defs), typically 50-100×
     * smaller per file. */
    CBMModuleDefIndex *module_def_index =
        all_defs ? cbm_pxc_build_module_def_index(all_defs, def_count) : NULL;
    /* Tier 2 full: pre-build per-language cross-LSP registries.
     * Built ONCE here; shared READ-ONLY across all files of that language
     * during resolve. Per-file work is then: parse + AST walk + O(1) lookups
     * — no registry build, no Phase 1b mutations. Languages added so far:
     * Go, Python. Others (C/C++, TS/JS, PHP, C#) fall back to per-file. */
    CBMArena cross_lsp_arena;
    cbm_arena_init(&cross_lsp_arena);
    CBMCrossLspRegistries cross_registries = {0};
    if (all_defs) {
        cross_registries.go = cbm_go_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.python =
            cbm_py_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.c = cbm_c_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.cs = cbm_cs_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        cross_registries.ts = cbm_ts_build_cross_registry(&cross_lsp_arena, all_defs, def_count);
        /* Rust: NOT built here. The shared all_defs registry is built LAZILY on the
         * first NULL-filter rust file (the amplifier files) inside cbm_parallel_resolve
         * — repos whose rust files all filter to subsets never pay the build/RSS. */
    }
    cbm_log_info("pass.timing", "pass", "lsp_cross_prepare", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    log_phase_mem("lsp_cross_prepare");
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    rc = cbm_parallel_resolve(ctx, files, file_count, cache, &shared_ids, worker_count, all_defs,
                              def_count, def_modules, module_def_index, &cross_registries);
    cbm_log_info("pass.timing", "pass", "parallel_resolve", "elapsed_ms",
                 itoa_buf((int)elapsed_ms(*t)));
    log_phase_mem("parallel_resolve");
    cbm_pxc_free_module_def_index(module_def_index);
    cbm_arena_destroy(&cross_lsp_arena); /* releases all per-lang registries */
    free(all_defs);
    if (def_modules) {
        for (int i = 0; i < file_count; i++) {
            free(def_modules[i]);
        }
        free(def_modules);
    }
    cbm_gbuf_set_next_id(p->gbuf, atomic_load(&shared_ids));
    cbm_pipeline_extract_infra_routes(p->gbuf, files, cache, file_count);
    cbm_pipeline_process_infra_bindings(p->gbuf, files, cache, file_count);
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) {
            cbm_free_result(cache[i]);
        }
    }
    free(cache);
    if (rc != 0) {
        return rc;
    }
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    cbm_pipeline_pass_k8s(ctx, files, file_count);
    cbm_log_info("pass.timing", "pass", "k8s", "elapsed_ms", itoa_buf((int)elapsed_ms(*t)));
    return check_cancel(p) ? CBM_NOT_FOUND : 0;
}

/* Try incremental pipeline or select a staged full rebuild.
 * Returns PL_FULL_REINDEX_NEEDED only when the caller should proceed with
 * full; every other value is the completed incremental attempt's
 * success/error code. CBM_STORE_ERR and CBM_NOT_FOUND are both -1, so neither
 * can safely double as this internal routing sentinel. */
static int try_incremental_or_delete_db(cbm_pipeline_t *p, cbm_file_info_t *files, int file_count) {
    char *db_path = resolve_db_path(p);
    if (!db_path) {
        return CBM_STORE_ERR;
    }
    if (!cbm_file_exists(db_path)) {
        free(db_path);
        return PL_FULL_REINDEX_NEEDED;
    }
    cbm_store_t *check_store = cbm_store_open_path(db_path);
    if (check_store && cbm_store_check_integrity(check_store)) {
        char *stored_config_fingerprint = NULL;
        char *stored_input_fingerprint = NULL;
        const char *loaded_config_fingerprint = cbm_pipeline_config_fingerprint(p);
        const char *loaded_input_fingerprint = cbm_pipeline_userconfig_fingerprint(p);
        int config_rc = cbm_store_get_index_config_fingerprint(check_store, p->project_name,
                                                               &stored_config_fingerprint);
        bool config_matches = config_rc == CBM_STORE_OK && loaded_config_fingerprint &&
                              strcmp(stored_config_fingerprint, loaded_config_fingerprint) == 0;
        int stored_mode = -1;
        bool mode_compatible =
            cbm_store_get_index_mode(check_store, p->project_name, &stored_mode) == CBM_STORE_OK &&
            stored_mode >= CBM_MODE_FULL && stored_mode <= CBM_MODE_FAST && p->mode >= stored_mode;
        bool mode_downgrade = mode_compatible && p->mode > stored_mode;
        bool allow_richer_fast_reuse =
            mode_downgrade && p->mode == CBM_MODE_FAST && p->effective_mode == p->mode;
        int input_rc = cbm_store_get_index_input_fingerprint(check_store, p->project_name,
                                                             &stored_input_fingerprint);
        bool input_matches = input_rc == CBM_STORE_OK && loaded_input_fingerprint &&
                             strcmp(stored_input_fingerprint, loaded_input_fingerprint) == 0;
        if (!mode_compatible) {
            cbm_log_info("pipeline.route", "path", "mode_upgrade_reindex", "stored",
                         itoa_buf(stored_mode), "requested", itoa_buf((int)p->mode));
        }
        if (!config_matches) {
            cbm_log_info("pipeline.route", "path", "userconfig_change_reindex", "stored",
                         stored_config_fingerprint ? stored_config_fingerprint : "missing",
                         "loaded",
                         loaded_config_fingerprint ? loaded_config_fingerprint : "missing");
        }
        free(stored_config_fingerprint);
        cbm_file_hash_t *hashes = NULL;
        int hash_count = 0;
        int hash_rc = cbm_store_get_file_hashes(check_store, p->project_name, &hashes, &hash_count);
        cbm_store_free_file_hashes(hashes, hash_count);
        cbm_store_close(check_store);
        bool completed_empty = hash_rc == CBM_STORE_OK && hash_count == 0 && file_count == 0;
        int hash_slack = hash_count > 0 ? hash_count / PAIR_LEN : 0;
        bool hash_bound_valid = hash_count >= 0 && hash_slack <= INT_MAX - hash_count;
        bool hashes_plausible =
            hash_rc == CBM_STORE_OK && hash_bound_valid &&
            (completed_empty || (hash_count > 0 && file_count <= hash_count + hash_slack));
        bool can_increment = config_matches && mode_compatible &&
                             (allow_richer_fast_reuse || input_matches) && hashes_plausible;
        if (can_increment && allow_richer_fast_reuse) {
            p->input_fingerprint_override =
                stored_input_fingerprint ? strdup(stored_input_fingerprint) : NULL;
            if (!p->input_fingerprint_override) {
                can_increment = false;
            } else {
                p->effective_mode = (cbm_index_mode_t)stored_mode;
            }
        }
        if (can_increment && mode_downgrade) {
            p->effective_mode = (cbm_index_mode_t)stored_mode;
        }
        free(stored_input_fingerprint);
        if (can_increment) {
            cbm_log_info("pipeline.route", "path", "incremental", "stored_hashes",
                         itoa_buf(hash_count));
            int rc = cbm_pipeline_run_incremental(p, db_path, files, file_count);
            free(db_path);
            return rc;
        }
        if (hash_count > 0) {
            cbm_log_info("pipeline.route", "path", "mode_change_reindex", "stored_hashes",
                         itoa_buf(hash_count), "discovered", itoa_buf(file_count));
        }
    } else if (check_store) {
        cbm_store_close(check_store);
    }
    cbm_log_info("pipeline.route", "path", "reindex", "action", "staging replacement db");
    /* Capture any ADR before rebuilding so the staged full-reindex can
     * restore it (project_summaries is otherwise lost). Issue #516. */
    {
        cbm_store_t *adr_store = cbm_store_open_path(db_path);
        if (adr_store) {
            cbm_adr_t existing;
            if (cbm_store_adr_get(adr_store, p->project_name, &existing) == CBM_STORE_OK) {
                if (existing.content) {
                    free(p->saved_adr);
                    p->saved_adr = strdup(existing.content);
                }
                cbm_store_adr_free(&existing);
            }
            cbm_store_close(adr_store);
        }
    }
    /* Keep the last completed database available. dump_and_persist_hashes writes
     * the replacement to a sibling staging path and installs it through a SQLite
     * backup transaction only after hashes, coverage, FTS and the completion
     * marker all succeed. */
    free(db_path);
    return PL_FULL_REINDEX_NEEDED;
}

int cbm_pipeline_capture_file_versions(const cbm_file_info_t *files, int file_count,
                                       cbm_file_version_snapshot_t *versions) {
    if (file_count < 0 || (file_count > 0 && (!files || !versions))) {
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < file_count; i++) {
        memset(&versions[i], 0, sizeof(versions[i]));
        versions[i].size = -1;
        long max_file_bytes = cbm_max_file_bytes();
        int status =
            cbm_sha256_file_version_hex(files[i].path, (size_t)max_file_bytes, versions[i].sha256,
                                        &versions[i].mtime_ns, &versions[i].size);
        if (status == CBM_SHA256_FILE_ERROR && versions[i].size < 0) {
            cbm_log_error("pipeline.err", "phase", "capture_file_version", "path",
                          files[i].rel_path ? files[i].rel_path : "?");
            return CBM_STORE_ERR;
        }
        /* Match the extractor's read_file contract. Hashing a multi-gigabyte
         * file merely to discover that extraction will report it as oversized
         * defeats the cap and can turn a bounded skip into unbounded I/O. */
        if (status == CBM_SHA256_FILE_SKIPPED) {
            versions[i].content_skipped = true;
            continue;
        }
        if (status == CBM_SHA256_FILE_ERROR) {
            /* Preserve the pipeline's read-skip behavior. If extraction later
             * succeeds, verify_extracted_versions promotes its exact-byte
             * digest and final verification still binds publication to it. */
            cbm_log_warn("pipeline.file_version_unverified", "path",
                         files[i].rel_path ? files[i].rel_path : "?");
            continue;
        }
        versions[i].verified = true;
    }
    return CBM_STORE_OK;
}

int cbm_pipeline_verify_file_versions(const cbm_file_info_t *files, int file_count,
                                      cbm_file_version_snapshot_t *versions) {
    if (file_count < 0 || (file_count > 0 && (!files || !versions))) {
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < file_count; i++) {
        if (!versions[i].verified) {
            cbm_file_version_snapshot_t live;
            if (cbm_pipeline_capture_file_versions(&files[i], 1, &live) != CBM_STORE_OK) {
                cbm_log_error("pipeline.err", "phase", "verify_unverified_source", "path",
                              files[i].rel_path ? files[i].rel_path : "?");
                return CBM_STORE_ERR;
            }
            if (versions[i].content_skipped && !live.content_skipped) {
                cbm_log_error("pipeline.err", "phase", "oversized_source_became_eligible", "path",
                              files[i].rel_path ? files[i].rel_path : "?");
                return CBM_STORE_ERR;
            }
            versions[i].mtime_ns = live.mtime_ns;
            versions[i].size = live.size;
            continue;
        }
        cbm_file_version_snapshot_t live;
        if (cbm_pipeline_capture_file_versions(&files[i], 1, &live) != CBM_STORE_OK ||
            !live.verified || strcmp(live.sha256, versions[i].sha256) != 0) {
            cbm_log_error("pipeline.err", "phase", "source_changed_during_index", "path",
                          files[i].rel_path ? files[i].rel_path : "?");
            return CBM_STORE_ERR;
        }
        /* A timestamp-only touch does not invalidate a content-derived graph.
         * Persist the final metadata so the next noop classification does not
         * perform an unnecessary re-extraction. */
        versions[i].mtime_ns = live.mtime_ns;
        versions[i].size = live.size;
    }
    return CBM_STORE_OK;
}

int cbm_pipeline_verify_extracted_versions(const cbm_file_info_t *files, int file_count,
                                           cbm_file_version_snapshot_t *versions,
                                           CBMFileResult *const *results) {
    if (file_count < 0 || (file_count > 0 && (!files || !versions || !results))) {
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < file_count; i++) {
        if (!results[i]) {
            continue;
        }
        if (!results[i]->source_sha256[0]) {
            cbm_log_error("pipeline.err", "phase", "missing_extracted_source_hash", "path",
                          files[i].rel_path ? files[i].rel_path : "?");
            return CBM_STORE_ERR;
        }
        if (!versions[i].verified) {
            memcpy(versions[i].sha256, results[i]->source_sha256, sizeof(versions[i].sha256));
            versions[i].verified = true;
            versions[i].content_skipped = false;
        } else if (strcmp(results[i]->source_sha256, versions[i].sha256) != 0) {
            cbm_log_error("pipeline.err", "phase", "extracted_source_changed", "path",
                          files[i].rel_path ? files[i].rel_path : "?");
            return CBM_STORE_ERR;
        }
    }
    return CBM_STORE_OK;
}

/* Dump graph to SQLite and persist file hashes for incremental indexing. */
static int dump_and_persist_hashes(cbm_pipeline_t *p, const cbm_file_info_t *files, int file_count,
                                   struct timespec *t) {
    cbm_clock_gettime(CLOCK_MONOTONIC, t);
    char *db_path = resolve_db_path(p);
    if (!db_path) {
        return CBM_STORE_ERR;
    }
    const char *last_slash = strrchr(db_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash == db_path ? 1U : (size_t)(last_slash - db_path);
        char *db_dir = (char *)malloc(dir_len + 1U);
        if (!db_dir) {
            free(db_path);
            return CBM_STORE_ERR;
        }
        memcpy(db_dir, db_path, dir_len);
        db_dir[dir_len] = '\0';
        bool made_dir = cbm_mkdir_p(db_dir, CBM_DIR_PERMS);
        free(db_dir);
        if (!made_dir) {
            free(db_path);
            return CBM_STORE_ERR;
        }
    }
    if (p->file_version_count != file_count ||
        cbm_pipeline_verify_input_snapshot(p) != CBM_STORE_OK ||
        cbm_pipeline_verify_file_versions(files, file_count, p->file_versions) != CBM_STORE_OK) {
        cbm_log_error("pipeline.err", "phase", "verify_input_versions", "project", p->project_name);
        free(db_path);
        return CBM_STORE_ERR;
    }
    /* Capture committed counts BEFORE the dump. cbm_gbuf_dump_to_sqlite calls
     * release_gbuf_indexes(), which frees node_by_qn (graph_buffer.c), after
     * which cbm_gbuf_node_count() returns 0. Reading these post-dump left
     * committed_nodes at 0, so the #334 plausibility gate never fired. */
    p->committed_nodes = cbm_gbuf_node_count(p->gbuf);
    p->committed_edges = cbm_gbuf_edge_count(p->gbuf);
    size_t db_path_len = strlen(db_path);
    static const char building_suffix[] = ".building.XXXXXX";
    if (db_path_len > SIZE_MAX - sizeof(building_suffix)) {
        free(db_path);
        return CBM_STORE_ERR;
    }
    char *staged_path = (char *)malloc(db_path_len + sizeof(building_suffix));
    if (!staged_path) {
        free(db_path);
        return CBM_STORE_ERR;
    }
    memcpy(staged_path, db_path, db_path_len);
    memcpy(staged_path + db_path_len, building_suffix, sizeof(building_suffix));
    int staged_fd = cbm_mkstemp(staged_path);
    if (staged_fd < 0) {
        free(staged_path);
        free(db_path);
        return CBM_STORE_ERR;
    }
#ifdef _WIN32
    int staged_close_rc = _close(staged_fd);
#else
    int staged_close_rc = close(staged_fd);
#endif
    if (staged_close_rc != 0) {
        cbm_unlink(staged_path);
        free(staged_path);
        free(db_path);
        return CBM_STORE_ERR;
    }

    int rc = cbm_gbuf_dump_to_sqlite(p->gbuf, staged_path);
    if (rc != 0) {
        cbm_log_error("pipeline.err", "phase", "dump");
        cbm_unlink(staged_path);
        cbm_remove_db_sidecars(staged_path);
        free(staged_path);
        free(db_path);
        return rc;
    }
    cbm_log_info("pass.timing", "pass", "dump", "elapsed_ms", itoa_buf((int)elapsed_ms(*t)));
    /* Persist-tail spans (phase "persist"): attribute the ~60s that lands here
     * AFTER cbm_gbuf_dump_to_sqlite returns. Active only under CBM_PROFILE. */
    CBM_PROF_START(t_reopen);
    cbm_store_t *hash_store = cbm_store_open_path(staged_path);
    CBM_PROF_END("persist", "1_reopen", t_reopen);
    if (hash_store) {
        bool persist_ok = true;
        CBM_PROF_START(t_delhash);
        if (cbm_store_delete_file_hashes(hash_store, p->project_name) != CBM_STORE_OK) {
            persist_ok = false;
            cbm_log_error("pipeline.err", "phase", "delete_file_hashes", "project",
                          p->project_name);
        }
        CBM_PROF_END("persist", "2_delete_file_hashes", t_delhash);

        /* Restore the ADR captured before the dump. Surface a failed restore
         * rather than silently dropping the ADR (the original #516 symptom). */
        CBM_PROF_START(t_adr);
        if (p->saved_adr) {
            if (cbm_store_adr_store(hash_store, p->project_name, p->saved_adr) != CBM_STORE_OK) {
                persist_ok = false;
                cbm_log_error("pipeline.err", "phase", "adr_restore", "project", p->project_name);
            }
        }
        CBM_PROF_END("persist", "3_adr_restore", t_adr);

        /* Batch the per-file hash upserts into ONE transaction. The per-file
         * cbm_store_upsert_file_hash path autocommits, i.e. file_count fsyncs
         * (~89k on the kernel); cbm_store_upsert_file_hash_batch wraps the same
         * cached INSERT ... ON CONFLICT upsert in a single begin/commit. Same
         * (project, rel_path, sha256, mtime_ns, size) tuples, same replace
         * semantics — only the transaction boundary changes. */
        CBM_PROF_START(t_fh);
        size_t hash_capacity = (size_t)(file_count > 0 ? file_count : 1);
        cbm_file_hash_t *fhashes =
            (cbm_file_hash_t *)malloc(hash_capacity * sizeof(cbm_file_hash_t));
        if (fhashes) {
            for (int i = 0; i < file_count; i++) {
                fhashes[i].project = p->project_name;
                fhashes[i].rel_path = files[i].rel_path;
                fhashes[i].sha256 = p->file_versions[i].sha256;
                fhashes[i].mtime_ns = p->file_versions[i].mtime_ns;
                fhashes[i].size = p->file_versions[i].size;
            }
            if (cbm_store_upsert_file_hash_batch(hash_store, fhashes, file_count) != CBM_STORE_OK) {
                persist_ok = false;
                cbm_log_error("pipeline.err", "phase", "persist_file_hashes", "project",
                              p->project_name);
            }
            free(fhashes);
        } else {
            /* OOM fallback: persist the already-verified snapshots one row at
             * a time. No live file is re-read in this tail. */
            for (int i = 0; i < file_count; i++) {
                if (cbm_store_upsert_file_hash(
                        hash_store, p->project_name, files[i].rel_path, p->file_versions[i].sha256,
                        p->file_versions[i].mtime_ns, p->file_versions[i].size) != CBM_STORE_OK) {
                    persist_ok = false;
                }
            }
        }
        CBM_PROF_END_N("persist", "4_file_hashes", t_fh, file_count);

        /* Coverage rows (#963): a full run's file_errors plus the by-design
         * discovery exclusions are the complete coverage truth for the
         * project. The dump recreated the DB file, so the separate
         * index_coverage table starts empty — write only when there is
         * something to record (AFTER hashes, so the deleted-file prune inside
         * replace sees the live file set; not_indexed_* kinds are exempt from
         * that prune — deliberately-unindexed paths have no hash rows). */
        int cov_total = p->file_errors_count + p->excluded_count + p->ignored_count;
        if (cov_total > 0) {
            cbm_coverage_row_t *cov =
                (cbm_coverage_row_t *)malloc((size_t)cov_total * sizeof(*cov));
            if (cov) {
                int cn = 0;
                for (int i = 0; i < p->file_errors_count; i++) {
                    cov[cn].rel_path = p->file_errors[i].path;
                    cov[cn].kind = p->file_errors[i].phase;
                    cov[cn].detail = p->file_errors[i].reason;
                    cn++;
                }
                for (int i = 0; i < p->excluded_count; i++) {
                    cov[cn].rel_path = p->excluded_dirs[i];
                    cov[cn].kind = "not_indexed_dir";
                    cov[cn].detail = "excluded subtree";
                    cn++;
                }
                for (int i = 0; i < p->ignored_count; i++) {
                    cov[cn].rel_path = p->ignored_files[i].rel_path;
                    cov[cn].kind = "not_indexed_file";
                    cov[cn].detail = p->ignored_files[i].reason;
                    cn++;
                }
                if (cbm_store_coverage_replace(hash_store, p->project_name, cov, cn) !=
                    CBM_STORE_OK) {
                    persist_ok = false;
                    cbm_log_error("pipeline.err", "phase", "persist_coverage", "project",
                                  p->project_name);
                }
                free(cov);
            } else {
                persist_ok = false;
                cbm_log_error("pipeline.err", "phase", "persist_coverage_oom", "project",
                              p->project_name);
            }
        }
        if (p->ignored_total > p->ignored_count) {
            cbm_log_warn("index.ignored_capped", "stored", itoa_buf(p->ignored_count), "total",
                         itoa_buf(p->ignored_total));
        }

        /* FTS5 backfill: populate nodes_fts with camelCase-split names.
         * Contentless FTS5 requires the special 'delete-all' command instead of
         * DELETE FROM to wipe prior rows (there's no underlying content table).
         * Falls back to plain names if cbm_camel_split is unavailable (which
         * shouldn't happen because we always register it, but we stay defensive). */
        CBM_PROF_START(t_fts);
        if (cbm_store_exec(hash_store, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');") !=
            CBM_STORE_OK) {
            persist_ok = false;
        }
        if (persist_ok &&
            cbm_store_exec(hash_store,
                           "INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path) "
                           "SELECT id, cbm_camel_split(name), qualified_name, label, file_path "
                           "FROM nodes;") != CBM_STORE_OK) {
            if (cbm_store_exec(
                    hash_store,
                    "INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path) "
                    "SELECT id, name, qualified_name, label, file_path FROM nodes;") !=
                CBM_STORE_OK) {
                persist_ok = false;
            }
        }
        CBM_PROF_END("persist", "5_fts_backfill", t_fts);

        /* Publish the completion marker last. Cached MCP readers use this to
         * distinguish a fully persisted graph from a DB being rebuilt. */
        const char *config_fingerprint = cbm_pipeline_config_fingerprint(p);
        const char *input_fingerprint = cbm_pipeline_userconfig_fingerprint(p);
        if (!persist_ok || !cbm_pipeline_input_generation_ok(p) ||
            cbm_pipeline_verify_input_snapshot(p) != CBM_STORE_OK ||
            cbm_pipeline_verify_discovery_set(p, files, file_count) != CBM_STORE_OK ||
            !config_fingerprint || !input_fingerprint ||
            cbm_store_mark_index_complete_with_inputs(hash_store, p->project_name,
                                                      config_fingerprint, input_fingerprint,
                                                      (int)p->effective_mode) != CBM_STORE_OK ||
            cbm_store_checkpoint(hash_store) != CBM_STORE_OK) {
            cbm_log_error("pipeline.err", "phase", "publish_snapshot", "project", p->project_name);
            cbm_store_close(hash_store);
            cbm_unlink(staged_path);
            cbm_remove_db_sidecars(staged_path);
            free(p->saved_adr);
            p->saved_adr = NULL;
            free(staged_path);
            free(db_path);
            return CBM_STORE_ERR;
        }

        cbm_store_close(hash_store);
        if (cbm_store_install_snapshot_file(staged_path, db_path) != CBM_STORE_OK) {
            cbm_log_error("pipeline.err", "phase", "install_snapshot", "project", p->project_name);
            cbm_unlink(staged_path);
            cbm_remove_db_sidecars(staged_path);
            free(p->saved_adr);
            p->saved_adr = NULL;
            free(staged_path);
            free(db_path);
            return CBM_STORE_ERR;
        }
        cbm_unlink(staged_path);
        cbm_remove_db_sidecars(staged_path);
        cbm_log_info("pass.timing", "pass", "persist_hashes", "files", itoa_buf(file_count));
    } else {
        cbm_log_error("pipeline.err", "phase", "reopen_persisted_db", "project", p->project_name);
        cbm_unlink(staged_path);
        cbm_remove_db_sidecars(staged_path);
        free(p->saved_adr);
        p->saved_adr = NULL;
        free(staged_path);
        free(db_path);
        return CBM_STORE_ERR;
    }
    free(p->saved_adr);
    p->saved_adr = NULL;

    /* Export persistent artifact if enabled */
    if (p->persistence) {
        CBM_PROF_START(t_art);
        int arc = cbm_artifact_export(db_path, p->repo_path, p->project_name, CBM_ARTIFACT_BEST);
        CBM_PROF_END("persist", "6_artifact_export", t_art);
        if (arc != 0) {
            const char *err = cbm_artifact_export_last_error();
            /* The DB generation is already atomically installed. Artifact
             * export is a secondary publication channel, so report failure
             * without lying to callers that the committed index failed. */
            cbm_log_warn("pipeline.artifact_export_failed", "err", err ? err : "unknown");
        }
    }

    free(staged_path);
    free(db_path);
    return 0;
}

/* Run githistory pass. */
static int run_githistory(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx) {
    struct timespec t_gh;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t_gh);

    cbm_githistory_result_t gh_result = {0};
    cbm_thread_t gh_thread;
    bool gh_threaded = false;
    const char *selected_head =
        cbm_userconfig_snapshot_git_head(cbm_pipeline_userconfig_snapshot(p));
    gh_compute_arg_t gh_arg = {
        .repo_path = ctx->repo_path, .revision = selected_head, .result = &gh_result, .rc = 0};

    if (p->effective_mode != CBM_MODE_FAST && selected_head) {
        if (effective_worker_count(true) > SKIP_ONE) {
            if (cbm_thread_create(&gh_thread, 0, gh_compute_thread_fn, &gh_arg) == 0) {
                gh_threaded = true;
            }
        }
        if (!gh_threaded) {
            gh_arg.rc =
                cbm_pipeline_githistory_compute_at(ctx->repo_path, selected_head, &gh_result);
            cbm_log_info("pass.timing", "pass", "githistory_compute", "elapsed_ms",
                         itoa_buf((int)elapsed_ms(t_gh)));
        }
    } else {
        cbm_log_info("pass.skip", "pass", "githistory", "reason", "fast_mode");
    }

    if (gh_threaded) {
        cbm_thread_join(&gh_thread);
        cbm_log_info("pass.timing", "pass", "githistory_compute", "elapsed_ms",
                     itoa_buf((int)elapsed_ms(t_gh)));
    }

    const char *expected_history =
        cbm_userconfig_snapshot_git_history_sha256(cbm_pipeline_userconfig_snapshot(p));
    if (p->effective_mode != CBM_MODE_FAST && selected_head &&
        (gh_arg.rc != 0 || !expected_history ||
         strcmp(expected_history, gh_result.input_sha256) != 0)) {
        atomic_store(&p->input_generation_failed, true);
        free(gh_result.couplings);
        free(gh_result.file_temporal);
        cbm_log_error("pipeline.err", "phase", "githistory_input_mismatch", "project",
                      p->project_name);
        return CBM_STORE_ERR;
    }

    int gh_edges = 0;
    if (gh_result.count > 0 || gh_result.file_temporal_count > 0) {
        gh_edges = cbm_pipeline_githistory_apply(ctx, &gh_result);
    }
    cbm_log_info("pass.done", "pass", "githistory", "commits", itoa_buf(gh_result.commit_count),
                 "edges", itoa_buf(gh_edges));
    free(gh_result.couplings);
    free(gh_result.file_temporal);
    return 0;
}

/* ── Pipeline run ────────────────────────────────────────────────── */

/* Run tests + git history. Returns 0 on success. */
static int run_tests_and_history(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                 const cbm_file_info_t *files, int file_count) {
    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    CBM_PROF_START(t_tests);
    int rc = cbm_pipeline_pass_tests(ctx, files, file_count);
    CBM_PROF_END_N("pipeline", "pass_tests", t_tests, file_count);
    cbm_log_info("pass.timing", "pass", "tests", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    if (rc == 0 && !check_cancel(p)) {
        CBM_PROF_START(t_gh);
        rc = run_githistory(p, ctx);
        CBM_PROF_END("pipeline", "pass_githistory", t_gh);
    }
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }
    return rc;
}

/* Run tests, git history, predump passes, and dump+persist. */
static int run_post_extraction(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                               const cbm_file_info_t *files, int file_count) {
    int rc = run_tests_and_history(p, ctx, files, file_count);
    if (rc != 0) {
        return rc;
    }

    CBM_PROF_START(t_design);
    rc = cbm_pipeline_pass_design(ctx, files, file_count);
    CBM_PROF_END_N("pipeline", "pass_design_context", t_design, file_count);
    if (rc != 0 || check_cancel(p)) {
        return rc != 0 ? rc : CBM_NOT_FOUND;
    }

    CBM_PROF_START(t_predump);
    run_predump_passes(p, ctx);
    CBM_PROF_END("pipeline", "3_predump_passes_total", t_predump);

    if (!check_cancel(p)) {
        struct timespec t;
        CBM_PROF_START(t_dump);
        rc = dump_and_persist_hashes(p, files, file_count, &t);
        CBM_PROF_END("pipeline", "4_dump_and_persist", t_dump);
    }
    return rc;
}

#define MIN_FILES_FOR_PARALLEL 50

/* Run structure + extraction passes (parallel or sequential). */
static int run_extraction_phase(cbm_pipeline_t *p, cbm_pipeline_ctx_t *ctx,
                                const cbm_file_info_t *files, int file_count) {
    struct timespec t;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t);
    CBM_PROF_START(t_struct);
    pass_structure(p, files, file_count);
    CBM_PROF_END_N("pipeline", "pass_structure", t_struct, file_count);
    cbm_log_info("pass.timing", "pass", "structure", "elapsed_ms", itoa_buf((int)elapsed_ms(t)));
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }

    int worker_count = effective_worker_count(true);
    CBM_PROF_START(t_extract_total);
    int rc = (worker_count > SKIP_ONE && file_count > MIN_FILES_FOR_PARALLEL)
                 ? run_parallel_pipeline(p, ctx, files, file_count, worker_count, &t)
                 : run_sequential_pipeline(p, ctx, files, file_count, &t);
    CBM_PROF_END_N("pipeline", "2_extraction_total", t_extract_total, file_count);
    if (check_cancel(p)) {
        return CBM_NOT_FOUND;
    }
    return rc;
}

int cbm_pipeline_run(cbm_pipeline_t *p) {
    if (!p) {
        return CBM_NOT_FOUND;
    }
    atomic_store(&p->input_generation_failed, false);
    select_effective_mode(p);
    free(p->input_fingerprint_override);
    p->input_fingerprint_override = NULL;

    cbm_git_context_t selected_git = {0};
    if (cbm_git_context_resolve(p->repo_path, &selected_git) != 0) {
        cbm_git_context_free(&selected_git);
        return CBM_STORE_ERR;
    }
    char *selected_branch_qn = cbm_git_context_branch_qn(p->project_name, &selected_git);
    if (!selected_branch_qn) {
        cbm_git_context_free(&selected_git);
        return CBM_STORE_ERR;
    }
    cbm_git_context_free(&p->git_ctx);
    p->git_ctx = selected_git;
    free(p->branch_qn);
    p->branch_qn = selected_branch_qn;

    CBM_PROF_START(t_pipeline_total);
    struct timespec t0;
    cbm_clock_gettime(CLOCK_MONOTONIC, &t0);
    cbm_path_alias_collection_t *path_aliases = NULL;
    CBMHashTable *source_version_index = NULL;

    /* C/C++ #define Macro nodes (#375) dominate extraction on macro-dense repos
     * (≈49% of nodes on the Linux kernel), so gate them to full mode — moderate
     * and fast skip them entirely. Set before any extraction dispatch. */
    cbm_set_macro_extraction(p->effective_mode == CBM_MODE_FULL);

    /* Load user-defined extension overrides and capture the exact config
     * generation used by discovery. Config parse errors remain fail-open, but
     * allocation failure is fatal because no completed index may omit its
     * config fingerprint. */
    CBM_PROF_START(t_userconfig);
    cbm_userconfig_free(p->userconfig);
    p->userconfig = NULL;
    cbm_userconfig_snapshot_free(p->userconfig_snapshot);
    p->userconfig_snapshot = NULL;
    cbm_discovery_snapshot_free(p->discovery_snapshot);
    p->discovery_snapshot = NULL;
    p->userconfig = cbm_userconfig_load_with_snapshot(p->repo_path, &p->userconfig_snapshot);
    cbm_set_user_lang_config(p->userconfig);
    CBM_PROF_END("pipeline", "0_userconfig_load", t_userconfig);
    if (!p->userconfig || !p->userconfig_snapshot) {
        cbm_set_user_lang_config(NULL);
        cbm_userconfig_free(p->userconfig);
        p->userconfig = NULL;
        cbm_userconfig_snapshot_free(p->userconfig_snapshot);
        p->userconfig_snapshot = NULL;
        return CBM_STORE_ERR;
    }
    char git_context_sha256[CBM_SHA256_HEX_LEN + 1];
    if (cbm_git_context_fingerprint(&p->git_ctx, git_context_sha256) != 0 ||
        cbm_userconfig_snapshot_set_git_context(p->userconfig_snapshot, git_context_sha256) != 0) {
        cbm_set_user_lang_config(NULL);
        cbm_userconfig_free(p->userconfig);
        p->userconfig = NULL;
        cbm_userconfig_snapshot_free(p->userconfig_snapshot);
        p->userconfig_snapshot = NULL;
        return CBM_STORE_ERR;
    }

    /* Phase 1: Discover files */
    CBM_PROF_START(t_discover);
    char *discovery_output_path = resolve_db_path(p);
    cbm_discover_opts_t opts = {
        .mode = p->effective_mode,
        .ignore_file = NULL,
        .max_file_size = 0,
        .exclude_output_path = discovery_output_path,
    };
    cbm_file_info_t *files = NULL;
    int file_count = 0;
    /* Capture skipped subtrees on the pipeline so the MCP layer can report
     * which directories were excluded (#411), plus the individually-ignored
     * files (#963 "purposely not indexed"). Replace any prior lists (e.g. a
     * re-run on the same pipeline) to avoid leaking the previous ones. */
    cbm_discover_free_excluded(p->excluded_dirs, p->excluded_count);
    p->excluded_dirs = NULL;
    p->excluded_count = 0;
    cbm_discover_free_ignored(p->ignored_files, p->ignored_count);
    p->ignored_files = NULL;
    p->ignored_count = 0;
    p->ignored_total = 0;
    int rc = cbm_discover_ex3(p->repo_path, &opts, &files, &file_count, &p->excluded_dirs,
                              &p->excluded_count, &p->ignored_files, &p->ignored_count,
                              &p->ignored_total, &p->discovery_snapshot);
    free(discovery_output_path);
    if (rc != 0) {
        cbm_log_error("pipeline.err", "phase", "discover", "rc", itoa_buf(rc));
    }
    CBM_PROF_END_N("pipeline", "1_discover", t_discover, file_count);
    cbm_log_info("pipeline.discover", "files", itoa_buf(file_count), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t0)));
    if (rc != 0 || check_cancel(p)) {
        rc = CBM_NOT_FOUND;
        goto cleanup;
    }
    const char *discovery_fingerprint = cbm_discovery_snapshot_fingerprint(p->discovery_snapshot);
    if (!discovery_fingerprint ||
        cbm_userconfig_snapshot_set_discovery(p->userconfig_snapshot, discovery_fingerprint) != 0) {
        cbm_log_error("pipeline.err", "phase", "bind_discovery_snapshot", "project",
                      p->project_name);
        rc = CBM_STORE_ERR;
        goto cleanup;
    }

    /* Discovery has now fixed the excluded-subtree semantics used by the
     * auxiliary pkgmap/path-alias walks. Make those ignored inputs part of the
     * same persisted generation before choosing incremental vs full. */
    if (cbm_userconfig_snapshot_capture_auxiliary(p->userconfig_snapshot, p->repo_path,
                                                  p->excluded_dirs, p->excluded_count) != 0) {
        cbm_log_error("pipeline.err", "phase", "capture_auxiliary_inputs", "project",
                      p->project_name);
        rc = CBM_STORE_ERR;
        goto cleanup;
    }
    if (cbm_userconfig_snapshot_capture_git_head(p->userconfig_snapshot, p->repo_path,
                                                 p->effective_mode != CBM_MODE_FAST) != 0) {
        cbm_log_error("pipeline.err", "phase", "capture_git_head", "project", p->project_name);
        rc = CBM_STORE_ERR;
        goto cleanup;
    }

    /* Check for existing DB → try incremental or delete for reindex */
    rc = try_incremental_or_delete_db(p, files, file_count);
    if (rc != PL_FULL_REINDEX_NEEDED) {
        goto cleanup;
    }
    cbm_log_info("pipeline.route", "path", "full");

    /* Select the content generation this full run is allowed to publish.
     * Extraction results are checked against these digests, and the paths are
     * hashed once more immediately before the staged DB is dumped/installed. */
    free(p->file_versions);
    p->file_versions = (cbm_file_version_snapshot_t *)calloc(
        (size_t)(file_count > 0 ? file_count : 1), sizeof(*p->file_versions));
    p->file_version_count = file_count;
    if (!p->file_versions ||
        cbm_pipeline_capture_file_versions(files, file_count, p->file_versions) != CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
        goto cleanup;
    }
    cbm_pipeline_run_snapshot_hook_for_test();
    source_version_index =
        cbm_ht_create(file_count > 0 ? (uint32_t)file_count * PAIR_LEN : CBM_SZ_64);
    if (!source_version_index) {
        rc = CBM_STORE_ERR;
        goto cleanup;
    }
    for (int i = 0; i < file_count; i++) {
        if (files[i].rel_path) {
            (void)cbm_ht_set(source_version_index, files[i].rel_path, &p->file_versions[i]);
            if (cbm_ht_get(source_version_index, files[i].rel_path) != &p->file_versions[i]) {
                rc = CBM_STORE_ERR;
                goto cleanup;
            }
        }
    }

    /* Phase 2: Create graph buffer and registry */
    p->gbuf = cbm_gbuf_new(p->project_name, p->repo_path);
    p->registry = cbm_registry_new();

    /* Phase 2b: Load build-tool path aliases (tsconfig/jsconfig today). NULL
     * when no usable configs are found — non-TS projects pay nothing. */
    path_aliases = cbm_load_path_aliases_excluded_snapshot(
        p->repo_path, p->excluded_dirs, p->excluded_count, p->userconfig_snapshot);

    /* Build shared context for pass functions */
    cbm_pipeline_ctx_t ctx = {
        .project_name = p->project_name,
        .repo_path = p->repo_path,
        .gbuf = p->gbuf,
        .registry = p->registry,
        .cancelled = &p->cancelled,
        .pipeline = p, /* so passes can record per-file skips (Track B) */
        .mode = (int)p->effective_mode,
        .source_version_files = files,
        .source_versions = p->file_versions,
        .source_version_count = file_count,
        .source_version_index = source_version_index,
        .path_aliases = path_aliases,
        .excluded_dirs = p->excluded_dirs,
        .excluded_count = p->excluded_count,
    };

    rc = run_extraction_phase(p, &ctx, files, file_count);
    if (rc != 0) {
        goto cleanup;
    }

    rc = run_post_extraction(p, &ctx, files, file_count);
    if (rc != 0) {
        goto cleanup;
    }

    cbm_log_info("pipeline.done", "nodes", itoa_buf(cbm_gbuf_node_count(p->gbuf)), "edges",
                 itoa_buf(cbm_gbuf_edge_count(p->gbuf)), "elapsed_ms",
                 itoa_buf((int)elapsed_ms(t0)));
    CBM_PROF_END("pipeline", "TOTAL", t_pipeline_total);

cleanup:
    cbm_pkgmap_free(cbm_pipeline_get_pkgmap());
    cbm_pipeline_set_pkgmap(NULL);
    cbm_discover_free(files, file_count);
    free(p->file_versions);
    p->file_versions = NULL;
    p->file_version_count = 0;
    cbm_gbuf_free(p->gbuf);
    p->gbuf = NULL;
    cbm_registry_free(p->registry);
    p->registry = NULL;
    cbm_path_alias_collection_free(path_aliases);
    cbm_ht_free(source_version_index);
    /* Clear and free user extension config */
    cbm_set_user_lang_config(NULL);
    cbm_userconfig_free(p->userconfig);
    p->userconfig = NULL;
    cbm_userconfig_snapshot_free(p->userconfig_snapshot);
    p->userconfig_snapshot = NULL;
    return rc;
}
