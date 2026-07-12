/*
 * watcher.c — Git-based file change watcher.
 *
 * Strategy: git status + HEAD tracking (the most reliable approach).
 * For non-git projects, the watcher skips polling (no fsnotify/dirmtime yet).
 *
 *
 * Per-project state tracks:
 *   - Last git HEAD hash (detects commits, checkout, pull)
 *   - Last poll time + adaptive interval
 *   - Whether the project is a git repo
 *
 * Adaptive interval: 5s base + 1s per 500 files, capped at 60s.
 * Matches the Go watcher's `pollInterval()` logic.
 */
#include <stdint.h>
#include "watcher/watcher.h"
#include "store/store.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/hash_table.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "foundation/subprocess.h"
#include "git/git_context.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>

/* ── Per-project state ──────────────────────────────────────────── */

typedef struct {
    char *project_name;
    char *root_path;
    char last_head[CBM_SHA256_HEX_LEN + 1]; /* git HEAD hash (SHA-1 or SHA-256) */
    bool is_git;                            /* false → skip polling */
    bool baseline_done;                     /* true after first poll */
    int missing_root_count;    /* consecutive polls where root was missing (ENOENT/ENOTDIR) */
    uint64_t first_missing_ms; /* cbm_now_ms() of the streak's first miss (0 = no streak) */
    int file_count;            /* approximate, for interval calc */
    int interval_ms;           /* adaptive poll interval */
    int64_t next_poll_ns;      /* next poll time (monotonic ns) */
} project_state_t;

/* ── Watcher struct ─────────────────────────────────────────────── */

struct cbm_watcher {
    cbm_store_t *store;
    cbm_index_fn index_fn;
    void *user_data;
    CBMHashTable *projects; /* name → project_state_t* */
    cbm_mutex_t projects_lock;
    atomic_int stopped;
    /* Deferred-free list: freed after the next poll_once. */
    project_state_t **pending_free;
    int pending_free_count;
    int pending_free_cap;
};

/* ── Constants ─────────────────────────────────────────────────── */

/* Time unit conversions */
#define NS_PER_SEC 1000000000LL
#define US_PER_MS 1000000LL

/* Adaptive poll interval parameters (ms) */
#define POLL_BASE_MS 5000
#define POLL_FILE_STEP 500 /* add 1s per this many files */
#define POLL_MAX_MS 60000

/* Stale-root pruning (#286): a watched project whose root directory stays
 * missing is pruned — its cached DB is deleted and the watch entry removed.
 * Deletion is destructive (the DB can hold user-authored data such as the
 * ADR), so it requires BOTH a streak of consecutive missing polls AND a
 * sustained-absence grace window measured from the streak's first miss. */
#define MISSING_ROOT_DELETE_AFTER 3
#define PRUNE_GRACE_DEFAULT_S 600 /* 10 min; override: CBM_WATCHER_PRUNE_GRACE_S */

/* Sleep chunk for responsive shutdown (ms) */
#define SLEEP_CHUNK_MS 500

/* ── Time helper ────────────────────────────────────────────────── */

static int64_t now_ns(void) {
    struct timespec ts;
    cbm_clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * NS_PER_SEC) + ts.tv_nsec;
}

/* ── Adaptive interval ──────────────────────────────────────────── */

int cbm_watcher_poll_interval_ms(int file_count) {
    if (file_count <= 0) {
        return POLL_BASE_MS;
    }
    int64_t ms =
        (int64_t)POLL_BASE_MS + (((int64_t)file_count / POLL_FILE_STEP) * CBM_MSEC_PER_SEC);
    return ms > POLL_MAX_MS ? POLL_MAX_MS : (int)ms;
}

/* ── Git helpers ────────────────────────────────────────────────── */

/* Every watcher git command is an argv-based child of a resolved absolute git
 * executable. Repository paths therefore never pass through a command shell.
 * The byte ceilings also keep a hostile/corrupt repository from forcing an
 * unbounded capture allocation. */
#define WATCHER_GIT_TIMEOUT_MS 30000
#define WATCHER_GIT_META_MAX_BYTES (16U * 1024U)
#define WATCHER_GIT_STATUS_MAX_BYTES (16U * 1024U * 1024U)
#define WATCHER_GIT_FILES_MAX_BYTES (64U * 1024U * 1024U)
#define WATCHER_GIT_MAX_ARGS 16

typedef enum {
    GIT_PROBE_ERROR = -1,
    GIT_PROBE_NO = 0,
    GIT_PROBE_YES = 1,
} git_probe_t;

static int git_capture(const char *root_path, const char *const *args, size_t max_output_bytes,
                       char **out_data, size_t *out_len, cbm_proc_result_t *out_process) {
    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out_process) {
        out_process->outcome = CBM_PROC_SPAWN_FAILED;
        out_process->exit_code = -1;
        out_process->term_signal = 0;
    }
    if (!root_path || !args || !out_data || !out_len || !out_process || max_output_bytes == 0) {
        return CBM_NOT_FOUND;
    }

    char git_binary[CBM_SZ_4K];
    if (!cbm_git_resolve_binary(git_binary, sizeof(git_binary))) {
        return CBM_NOT_FOUND;
    }

    const char *argv[WATCHER_GIT_MAX_ARGS];
    size_t argc = 0;
    argv[argc++] = git_binary;
    argv[argc++] = "--no-optional-locks";
    argv[argc++] = "-C";
    argv[argc++] = root_path;
    for (size_t i = 0; args[i]; i++) {
        if (argc + 1U >= WATCHER_GIT_MAX_ARGS) {
            return CBM_NOT_FOUND;
        }
        argv[argc++] = args[i];
    }
    argv[argc] = NULL;

    cbm_proc_opts_t opts = {
        .bin = git_binary,
        .argv = argv,
        .quiet_timeout_ms = WATCHER_GIT_TIMEOUT_MS,
        .discard_stderr = true,
    };
    char digest[CBM_SHA256_HEX_LEN + 1];
    return cbm_subprocess_capture(&opts, max_output_bytes, out_data, out_len, digest, out_process);
}

static git_probe_t is_git_repo(const char *root_path) {
    const char *args[] = {"rev-parse", "--git-dir", NULL};
    char *data = NULL;
    size_t len = 0;
    cbm_proc_result_t process;
    int rc = git_capture(root_path, args, WATCHER_GIT_META_MAX_BYTES, &data, &len, &process);
    if (rc == 0) {
        bool valid = len > 0 && memchr(data, '\0', len) == NULL;
        free(data);
        return valid ? GIT_PROBE_YES : GIT_PROBE_ERROR;
    }
    free(data);
    /* rev-parse uses a normal nonzero exit for a directory that simply is not
     * a repository. Spawn, timeout, output-limit, and capture failures are
     * transient/uncertain and must be retried rather than cached as non-git. */
    return process.outcome == CBM_PROC_EXIT_NONZERO ? GIT_PROBE_NO : GIT_PROBE_ERROR;
}

static bool git_object_id(const char *data, size_t len) {
    if (!data || (len != 40U && len != CBM_SHA256_HEX_LEN)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

static int git_head(const char *root_path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return CBM_NOT_FOUND;
    }
    out[0] = '\0';
    const char *args[] = {"rev-parse", "--verify", "HEAD", NULL};
    char *data = NULL;
    size_t len = 0;
    cbm_proc_result_t process;
    if (git_capture(root_path, args, WATCHER_GIT_META_MAX_BYTES, &data, &len, &process) != 0) {
        free(data);
        return CBM_NOT_FOUND;
    }
    while (len > 0 && (data[len - 1U] == '\n' || data[len - 1U] == '\r')) {
        len--;
    }
    if (!git_object_id(data, len) || len + 1U > out_size) {
        free(data);
        return CBM_NOT_FOUND;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    free(data);
    return 0;
}

/* Porcelain v1 -z is locale-independent and unambiguous for every valid path.
 * --ignore-submodules=none makes the superproject record modified/untracked
 * submodule work trees, so no `submodule foreach` shell is needed. Any command,
 * capture, timeout, or output-limit failure is treated as dirty: uncertainty
 * must schedule a conservative reindex rather than silently miss a change. */
static bool git_is_dirty(const char *root_path) {
    const char *args[] = {
        "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=none",
        NULL};
    char *data = NULL;
    size_t len = 0;
    cbm_proc_result_t process;
    int rc = git_capture(root_path, args, WATCHER_GIT_STATUS_MAX_BYTES, &data, &len, &process);
    bool dirty = rc != 0 || len > 0;
    free(data);
    return dirty;
}

/* Count the NUL terminators emitted by `git ls-files -z`, not newlines in path
 * data. If the bounded 64 MiB capture overflows (or otherwise fails), use
 * INT_MAX: the only consumer is adaptive scheduling, and capping the interval
 * at 60 seconds is safer than misclassifying a huge repository as empty. */
static int git_file_count(const char *root_path) {
    const char *args[] = {"ls-files", "-z", NULL};
    char *data = NULL;
    size_t len = 0;
    cbm_proc_result_t process;
    if (git_capture(root_path, args, WATCHER_GIT_FILES_MAX_BYTES, &data, &len, &process) != 0) {
        free(data);
        return INT_MAX;
    }
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\0') {
            if (count == INT_MAX) {
                break;
            }
            count++;
        }
    }
    free(data);
    return count;
}

/* ── Project state lifecycle ────────────────────────────────────── */

static project_state_t *state_new(const char *name, const char *root_path) {
    project_state_t *s = calloc(CBM_ALLOC_ONE, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->project_name = strdup(name);
    s->root_path = strdup(root_path);
    s->interval_ms = POLL_BASE_MS;
    return s;
}

static void state_free(project_state_t *s) {
    if (!s) {
        return;
    }
    free(s->project_name);
    free(s->root_path);
    free(s);
}

/* Move a state onto the deferred-free list (caller holds projects_lock).
 * The state may still be referenced by a poll_once snapshot; poll_once
 * drains the list at the start of its next cycle. Returns false when
 * growing the list fails (OOM): the state is left untouched and the
 * caller must keep it registered — freeing it immediately here could be
 * a use-after-free against an in-flight poll snapshot. */
static bool defer_state_free(cbm_watcher_t *w, project_state_t *s) {
    if (w->pending_free_count >= w->pending_free_cap) {
        int new_cap = w->pending_free_cap ? w->pending_free_cap * 2 : 8;
        project_state_t **tmp =
            realloc(w->pending_free, (size_t)new_cap * sizeof(project_state_t *));
        if (!tmp) {
            cbm_log_warn("watcher.unwatch.oom", "project", s->project_name);
            return false;
        }
        w->pending_free = tmp;
        w->pending_free_cap = new_cap;
    }
    w->pending_free[w->pending_free_count++] = s;
    return true;
}

/* ── Stale-root pruning (#286) ──────────────────────────────────── */

bool cbm_watcher_root_missing_errno(int err) {
    /* Only ENOENT/ENOTDIR mean the root itself is gone. Anything else
     * (EACCES, EIO, ELOOP, a transient network mount, macOS TCC permission
     * revocation) is uncertainty: the directory may still exist even though
     * we cannot see it right now — never treat it as a deletion signal.
     * Windows (mingw/UCRT) maps ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
     * to ENOENT, so the same check holds there (same convention as
     * find_deleted_files in pipeline_incremental.c). */
    return err == ENOENT || err == ENOTDIR;
}

typedef enum {
    ROOT_PRESENT = 0, /* stat succeeded and the root is a directory */
    ROOT_MISSING,     /* genuinely gone: ENOENT/ENOTDIR (or replaced by a non-directory) */
    ROOT_UNCERTAIN,   /* any other stat failure — must NOT count toward pruning */
} root_status_t;

static root_status_t root_status(const char *root_path, int *out_errno) {
    *out_errno = 0;
    if (!root_path) {
        return ROOT_UNCERTAIN;
    }
    struct stat st;
    if (stat(root_path, &st) == 0) {
        /* Exists but is no longer a directory → the root directory is gone. */
        return S_ISDIR(st.st_mode) ? ROOT_PRESENT : ROOT_MISSING;
    }
    *out_errno = errno;
    return cbm_watcher_root_missing_errno(errno) ? ROOT_MISSING : ROOT_UNCERTAIN;
}

/* Sustained-absence window (seconds) before a missing root may be pruned.
 * Generous default: 10 minutes. Override with CBM_WATCHER_PRUNE_GRACE_S
 * (>= 0; 0 prunes as soon as the missing-poll streak is reached). Read on
 * each call so tests/operators can adjust via setenv without a restart —
 * same convention as cbm_max_file_bytes in limits.c. */
static long prune_grace_s(void) {
    const char *raw = getenv("CBM_WATCHER_PRUNE_GRACE_S");
    if (raw && raw[0]) {
        errno = 0;
        char *end = NULL;
        long v = strtol(raw, &end, 10);
        if (errno == 0 && end != raw && *end == '\0' && v >= 0) {
            return v;
        }
        /* Unparseable / negative → fall through to the safe default. */
    }
    return PRUNE_GRACE_DEFAULT_S;
}

/* Format int to string for logging (poll thread only, one use per call). */
static const char *itoa_buf(int v) {
    static CBM_TLS char buf[CBM_SZ_32];
    snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

static void delete_cached_project_db(const char *project_name) {
    if (!cbm_validate_project_name(project_name)) {
        return;
    }

    const char *cache_dir = cbm_resolve_cache_dir();
    if (!cache_dir) {
        return;
    }

    char path[CBM_SZ_1K];
    char wal[CBM_SZ_1K];
    char shm[CBM_SZ_1K];
    snprintf(path, sizeof(path), "%s/%s.db", cache_dir, project_name);
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    (void)cbm_unlink(path);
    (void)cbm_unlink(wal);
    (void)cbm_unlink(shm);
}

/* Hash table foreach callback to free state entries */
static void free_state_entry(const char *key, void *val, void *ud) {
    (void)key;
    (void)ud;
    state_free(val);
}

/* ── Watcher lifecycle ──────────────────────────────────────────── */

cbm_watcher_t *cbm_watcher_new(cbm_store_t *store, cbm_index_fn index_fn, void *user_data) {
    cbm_watcher_t *w = calloc(CBM_ALLOC_ONE, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->store = store;
    w->index_fn = index_fn;
    w->user_data = user_data;
    w->projects = cbm_ht_create(CBM_SZ_32);
    if (!w->projects) {
        free(w);
        return NULL;
    }
    cbm_mutex_init(&w->projects_lock);
    atomic_init(&w->stopped, 0);
    return w;
}

void cbm_watcher_free(cbm_watcher_t *w) {
    if (!w) {
        return;
    }
    /* Safety net: ensure stopped is set before draining pending_free.
     * In production the caller should cbm_watcher_stop() + join first. */
    atomic_store(&w->stopped, 1);
    cbm_mutex_lock(&w->projects_lock);
    cbm_ht_foreach(w->projects, free_state_entry, NULL);
    cbm_ht_free(w->projects);
    for (int i = 0; i < w->pending_free_count; i++) {
        state_free(w->pending_free[i]);
    }
    free(w->pending_free);
    cbm_mutex_unlock(&w->projects_lock);
    cbm_mutex_destroy(&w->projects_lock);
    free(w);
}

/* ── Watch list management ──────────────────────────────────────── */

void cbm_watcher_watch(cbm_watcher_t *w, const char *project_name, const char *root_path) {
    if (!w || !project_name || !root_path) {
        return;
    }

    /* Remove old entry first (key points to state's project_name) */
    cbm_mutex_lock(&w->projects_lock);
    project_state_t *old = cbm_ht_get(w->projects, project_name);
    if (old) {
        cbm_ht_delete(w->projects, project_name);
        state_free(old);
    }

    project_state_t *s = state_new(project_name, root_path);
    if (!s) {
        cbm_mutex_unlock(&w->projects_lock);
        cbm_log_warn("watcher.watch.oom", "project", project_name, "path", root_path);
        return;
    }
    cbm_ht_set(w->projects, s->project_name, s);
    cbm_mutex_unlock(&w->projects_lock);
    cbm_log_info("watcher.watch", "project", project_name, "path", root_path);
}

void cbm_watcher_unwatch(cbm_watcher_t *w, const char *project_name) {
    if (!w || !project_name) {
        return;
    }
    bool removed = false;
    cbm_mutex_lock(&w->projects_lock);
    project_state_t *s = cbm_ht_get(w->projects, project_name);
    if (s && defer_state_free(w, s)) {
        /* The entry leaves the table only once its state is safely on
         * the deferred-free list; on OOM the watch stays registered. */
        cbm_ht_delete(w->projects, project_name);
        removed = true;
    }
    cbm_mutex_unlock(&w->projects_lock);
    if (removed) {
        cbm_log_info("watcher.unwatch", "project", project_name);
    }
}

void cbm_watcher_touch(cbm_watcher_t *w, const char *project_name) {
    if (!w || !project_name) {
        return;
    }
    cbm_mutex_lock(&w->projects_lock);
    project_state_t *s = cbm_ht_get(w->projects, project_name);
    if (s) {
        /* Reset backoff — poll immediately on next cycle */
        s->next_poll_ns = 0;
    }
    cbm_mutex_unlock(&w->projects_lock);
}

int cbm_watcher_watch_count(cbm_watcher_t *w) {
    if (!w) {
        return 0;
    }
    cbm_mutex_lock(&w->projects_lock);
    int count = (int)cbm_ht_count(w->projects);
    cbm_mutex_unlock(&w->projects_lock);
    return count;
}

/* ── Single poll cycle ──────────────────────────────────────────── */

/* Init baseline for a project: check if git, get HEAD, count files */
static void init_baseline(project_state_t *s) {
    struct stat st;
    if (stat(s->root_path, &st) != 0) {
        cbm_log_warn("watcher.root_gone", "project", s->project_name, "path", s->root_path);
        s->baseline_done = true;
        s->is_git = false;
        return;
    }

    git_probe_t probe = is_git_repo(s->root_path);
    if (probe == GIT_PROBE_ERROR) {
        /* Do not permanently downgrade a repository because git was briefly
         * unavailable, timed out, or produced an invalid/oversized response. */
        cbm_log_warn("watcher.git_probe_error", "project", s->project_name, "path", s->root_path);
        return;
    }
    s->is_git = probe == GIT_PROBE_YES;
    s->baseline_done = true;

    if (s->is_git) {
        git_head(s->root_path, s->last_head, sizeof(s->last_head));
        s->file_count = git_file_count(s->root_path);
        s->interval_ms = cbm_watcher_poll_interval_ms(s->file_count);
        cbm_log_info("watcher.baseline", "project", s->project_name, "strategy", "git", "files",
                     s->file_count > 0 ? "yes" : "0");
    } else {
        cbm_log_info("watcher.baseline", "project", s->project_name, "strategy", "none");
    }

    s->next_poll_ns = now_ns() + ((int64_t)s->interval_ms * US_PER_MS);
}

/* Check if a project has changes. Returns true if reindex needed. */
static bool check_changes(project_state_t *s) {
    if (!s->is_git) {
        return false;
    }

    /* Check HEAD movement */
    char head[CBM_SZ_64] = {0};
    if (git_head(s->root_path, head, sizeof(head)) == 0) {
        if (s->last_head[0] != '\0' && strcmp(head, s->last_head) != 0) {
            /* HEAD moved — commit, checkout, pull */
            strncpy(s->last_head, head, sizeof(s->last_head) - 1);
            return true;
        }
        strncpy(s->last_head, head, sizeof(s->last_head) - 1);
    }

    /* Check working tree */
    return git_is_dirty(s->root_path);
}

/* Context for poll_once foreach callback */
typedef struct {
    cbm_watcher_t *w;
    int64_t now;
    int reindexed;
} poll_ctx_t;

static void prune_missing_project(cbm_watcher_t *w, project_state_t *s) {
    if (!w || !s || !s->project_name) {
        return;
    }

    char project_name[CBM_SZ_1K];
    snprintf(project_name, sizeof(project_name), "%s", s->project_name);

    bool removed = false;
    cbm_mutex_lock(&w->projects_lock);
    project_state_t *current = cbm_ht_get(w->projects, project_name);
    /* Deferred free (same discipline as cbm_watcher_unwatch): this state
     * is referenced by the poll_once snapshot iterating us. On OOM the
     * watch stays registered and pruning retries on the next cycle. */
    if (current == s && defer_state_free(w, s)) {
        delete_cached_project_db(project_name);
        cbm_ht_delete(w->projects, project_name);
        removed = true;
    }
    cbm_mutex_unlock(&w->projects_lock);

    if (removed) {
        cbm_log_info("watcher.root_pruned", "project", project_name);
    }
}

static void poll_project(const char *key, void *val, void *ud) {
    (void)key;
    poll_ctx_t *ctx = ud;
    project_state_t *s = val;
    if (!s) {
        return;
    }

    /* Stale-root pruning (#286): classify the root BEFORE the baseline /
     * is_git / interval gates so vanished roots are noticed even for
     * non-git projects and regardless of adaptive backoff. */
    int stat_errno = 0;
    root_status_t rs = root_status(s->root_path, &stat_errno);
    if (rs == ROOT_UNCERTAIN) {
        /* EACCES / EIO / network blip / TCC revocation — the root may still
         * exist. Never count toward pruning; restart the streak so only an
         * uninterrupted run of genuine ENOENT/ENOTDIR observations can
         * delete user data. */
        if (s->missing_root_count > 0) {
            s->missing_root_count = 0;
            s->first_missing_ms = 0;
        }
        cbm_log_warn("watcher.root_stat_error", "project", s->project_name, "path", s->root_path,
                     "errno", itoa_buf(stat_errno));
        return;
    }
    if (rs == ROOT_MISSING) {
        uint64_t now_ms = cbm_now_ms();
        if (s->missing_root_count == 0) {
            s->first_missing_ms = now_ms;
        }
        s->missing_root_count++;
        cbm_log_warn("watcher.root_missing", "project", s->project_name, "path", s->root_path,
                     "polls", itoa_buf(s->missing_root_count));
        if (s->missing_root_count >= MISSING_ROOT_DELETE_AFTER &&
            now_ms - s->first_missing_ms >= (uint64_t)prune_grace_s() * CBM_MSEC_PER_SEC) {
            prune_missing_project(ctx->w, s);
        }
        return;
    }
    if (s->missing_root_count > 0) {
        cbm_log_info("watcher.root_restored", "project", s->project_name, "path", s->root_path);
        s->missing_root_count = 0;
        s->first_missing_ms = 0;
    }

    /* Initialize baseline on first poll */
    if (!s->baseline_done) {
        init_baseline(s);
        return;
    }

    /* Skip non-git projects */
    if (!s->is_git) {
        return;
    }

    /* Respect adaptive interval */
    if (ctx->now < s->next_poll_ns) {
        return;
    }

    /* Check for changes */
    bool changed = check_changes(s);
    if (!changed) {
        s->next_poll_ns = ctx->now + ((int64_t)s->interval_ms * US_PER_MS);
        return;
    }

    /* Trigger reindex */
    cbm_log_info("watcher.changed", "project", s->project_name, "strategy", "git");
    if (ctx->w->index_fn) {
        int rc = ctx->w->index_fn(s->project_name, s->root_path, ctx->w->user_data);
        if (rc == 0) {
            ctx->reindexed++;
            /* Update HEAD after successful reindex */
            git_head(s->root_path, s->last_head, sizeof(s->last_head));
            /* Refresh file count for interval */
            s->file_count = git_file_count(s->root_path);
            s->interval_ms = cbm_watcher_poll_interval_ms(s->file_count);
        } else {
            cbm_log_warn("watcher.index.err", "project", s->project_name);
        }
    }

    s->next_poll_ns = ctx->now + ((int64_t)s->interval_ms * US_PER_MS);
}

/* Callback to snapshot project state pointers into an array. */
typedef struct {
    project_state_t **items;
    int count;
    int cap;
} snapshot_ctx_t;

static void snapshot_project(const char *key, void *val, void *ud) {
    (void)key;
    snapshot_ctx_t *sc = ud;
    if (val && sc->count < sc->cap) {
        sc->items[sc->count++] = val;
    }
}

int cbm_watcher_poll_once(cbm_watcher_t *w) {
    if (!w) {
        return 0;
    }

    /* Snapshot project pointers under lock, then poll without holding it.
     * This keeps the critical section small — poll_project does git I/O
     * and may invoke index_fn which runs the full pipeline. */
    cbm_mutex_lock(&w->projects_lock);

    /* Free deferred entries from the previous cycle. */
    for (int i = 0; i < w->pending_free_count; i++) {
        state_free(w->pending_free[i]);
    }
    w->pending_free_count = 0;

    int n = cbm_ht_count(w->projects);
    if (n == 0) {
        cbm_mutex_unlock(&w->projects_lock);
        return 0;
    }
    project_state_t **snap = malloc(n * sizeof(project_state_t *));
    if (!snap) {
        cbm_mutex_unlock(&w->projects_lock);
        return 0;
    }
    snapshot_ctx_t sc = {.items = snap, .count = 0, .cap = n};
    cbm_ht_foreach(w->projects, snapshot_project, &sc);
    cbm_mutex_unlock(&w->projects_lock);

    poll_ctx_t ctx = {
        .w = w,
        .now = now_ns(),
        .reindexed = 0,
    };
    for (int i = 0; i < sc.count; i++) {
        poll_project(NULL, snap[i], &ctx);
    }
    free(snap);
    return ctx.reindexed;
}

/* ── Blocking run loop ──────────────────────────────────────────── */

void cbm_watcher_stop(cbm_watcher_t *w) {
    if (w) {
        atomic_store(&w->stopped, 1);
    }
}

int cbm_watcher_run(cbm_watcher_t *w, int base_interval_ms) {
    if (!w) {
        return CBM_NOT_FOUND;
    }
    if (base_interval_ms <= 0) {
        base_interval_ms = POLL_BASE_MS;
    }

    cbm_log_info("watcher.start", "interval_ms", base_interval_ms > 999 ? "multi-sec" : "fast");

    while (!atomic_load(&w->stopped)) {
        cbm_watcher_poll_once(w);

        /* Sleep in small increments to allow responsive shutdown */
        int slept = 0;
        while (slept < base_interval_ms && !atomic_load(&w->stopped)) {
            int chunk = base_interval_ms - slept;
            if (chunk > SLEEP_CHUNK_MS) {
                chunk = SLEEP_CHUNK_MS;
            }
            cbm_usleep((unsigned)chunk * CBM_MSEC_PER_SEC);
            slept += chunk;
        }
    }

    cbm_log_info("watcher.stop");
    return 0;
}
