/*
 * test_artifact.c — Tests for persistent artifact export/import.
 */
#include "test_framework.h"
#include "store/store.h"
#include "pipeline/artifact.h"
#include "pipeline/pipeline.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/rooted_file.h"

#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <stdio.h>
#include <yyjson/yyjson.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static char g_tmpdir[1024];
static char g_repo[1024];
static char g_db[1024];
enum { ART_TEST_LOG_BUF = 32768 };
static char g_log_capture[ART_TEST_LOG_BUF];
static CBMLogLevel g_prev_log_level;

static void setup_artifact_test(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/cbm_test_artifact_XXXXXX", cbm_tmpdir());
    cbm_mkdtemp(g_tmpdir);

    snprintf(g_repo, sizeof(g_repo), "%s/repo", g_tmpdir);
    cbm_mkdir_p(g_repo, 0755);

    snprintf(g_db, sizeof(g_db), "%s/test.db", g_tmpdir);
}

/* Create a minimal but valid DB with some nodes and edges. */
static void create_test_db(const char *path) {
    cbm_store_t *s = cbm_store_open_path(path);
    if (!s) {
        return;
    }

    cbm_store_exec(s, "INSERT OR IGNORE INTO projects(name, indexed_at, root_path) "
                      "VALUES('test-proj', '2026-01-01', '/tmp/test');");

    cbm_store_exec(s, "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                      "VALUES('test-proj', 'Function', 'foo', 'test-proj.foo', 'main.c');");
    cbm_store_exec(s, "INSERT INTO nodes(project, label, name, qualified_name, file_path) "
                      "VALUES('test-proj', 'Function', 'bar', 'test-proj.bar', 'main.c');");

    cbm_store_exec(s, "INSERT INTO edges(project, source_id, target_id, type) "
                      "VALUES('test-proj', 1, 2, 'CALLS');");

    cbm_store_close(s);
}

static void cleanup_dir(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void write_text_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return;
    }
    fputs(text, fp);
    fclose(fp);
}

static char *read_file_text(const char *path, size_t max_bytes, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END) != 0) {
        if (fp) {
            fclose(fp);
        }
        return NULL;
    }
    long end = ftell(fp);
    if (end < 0 || (size_t)end > max_bytes || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *data = malloc((size_t)end + 1U);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(data, 1, (size_t)end, fp);
    bool ok = n == (size_t)end && !ferror(fp);
    fclose(fp);
    if (!ok) {
        free(data);
        return NULL;
    }
    data[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return data;
}

static bool artifact_payload_path(const char *repo, char *out, size_t out_size) {
    char meta_path[2048];
    int mn = snprintf(meta_path, sizeof(meta_path), "%s/%s/%s", repo, CBM_ARTIFACT_DIR,
                      CBM_ARTIFACT_META);
    if (mn <= 0 || (size_t)mn >= sizeof(meta_path)) {
        return false;
    }
    size_t len = 0;
    char *json = read_file_text(meta_path, 1024 * 1024, &len);
    yyjson_doc *doc = json ? yyjson_read(json, len, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *payload = root ? yyjson_obj_get(root, "payload") : NULL;
    const char *name = payload && yyjson_is_str(payload) ? yyjson_get_str(payload) : NULL;
    int n = name ? snprintf(out, out_size, "%s/%s/%s", repo, CBM_ARTIFACT_DIR, name) : -1;
    yyjson_doc_free(doc);
    free(json);
    return n > 0 && (size_t)n < out_size;
}

static bool artifact_metadata_payload_name(const char *repo, char *out, size_t out_size) {
    char full[4096];
    if (!artifact_payload_path(repo, full, sizeof(full))) {
        return false;
    }
    const char *slash = strrchr(full, '/');
    if (!slash || strlen(slash + 1U) >= out_size) {
        return false;
    }
    memcpy(out, slash + 1U, strlen(slash + 1U) + 1U);
    return true;
}

static void capture_log_sink(const char *line) {
    size_t used = strlen(g_log_capture);
    size_t avail = sizeof(g_log_capture) - used;
    if (avail <= 1) {
        return;
    }
    int n = snprintf(g_log_capture + used, avail, "%s\n", line);
    if (n < 0 || (size_t)n >= avail) {
        g_log_capture[sizeof(g_log_capture) - 1] = '\0';
    }
}

static void capture_logs_start(void) {
    g_log_capture[0] = '\0';
    g_prev_log_level = cbm_log_get_level();
    cbm_log_set_level(CBM_LOG_DEBUG);
    cbm_log_set_sink(capture_log_sink);
}

static const char *capture_logs_end(void) {
    cbm_log_set_sink(NULL);
    cbm_log_set_level(g_prev_log_level);
    return g_log_capture;
}

/* ── Tests ───────────────────────────────────────────────────────── */

/* Rewrite the "original_size" number in an artifact.json in place, adding
 * `delta` to it. Returns false if the field / a digit run isn't found. */
static bool bump_artifact_number(const char *meta_path, const char *field, long delta) {
    FILE *fp = fopen(meta_path, "rb");
    if (!fp) {
        return false;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    char needle[128];
    int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) {
        return false;
    }
    char *key = strstr(buf, needle);
    if (!key) {
        return false;
    }
    char *colon = strchr(key, ':');
    if (!colon) {
        return false;
    }
    char *ds = colon + 1;
    while (*ds == ' ' || *ds == '\t') {
        ds++;
    }
    char *de = ds;
    while (*de >= '0' && *de <= '9') {
        de++;
    }
    if (de == ds) {
        return false;
    }
    long val = strtol(ds, NULL, 10) + delta;
    char out[4096];
    int pre = (int)(ds - buf);
    snprintf(out, sizeof(out), "%.*s%ld%s", pre, buf, val, de);
    fp = fopen(meta_path, "wb");
    if (!fp) {
        return false;
    }
    fwrite(out, 1, strlen(out), fp);
    fclose(fp);
    return true;
}

static bool replace_artifact_number(const char *meta_path, const char *field,
                                    const char *replacement) {
    size_t len = 0;
    char *json = read_file_text(meta_path, 1024 * 1024, &len);
    if (!json) {
        return false;
    }
    char needle[128];
    int nn = snprintf(needle, sizeof(needle), "\"%s\"", field);
    char *key = nn > 0 && (size_t)nn < sizeof(needle) ? strstr(json, needle) : NULL;
    char *colon = key ? strchr(key, ':') : NULL;
    char *start = colon ? colon + 1 : NULL;
    while (start && (*start == ' ' || *start == '\t')) {
        start++;
    }
    char *end = start;
    while (end && *end >= '0' && *end <= '9') {
        end++;
    }
    bool ok = start && end > start;
    size_t prefix = ok ? (size_t)(start - json) : 0;
    size_t suffix = ok ? strlen(end) : 0;
    size_t replacement_len = strlen(replacement);
    char *updated = ok ? malloc(prefix + replacement_len + suffix + 1U) : NULL;
    if (updated) {
        memcpy(updated, json, prefix);
        memcpy(updated + prefix, replacement, replacement_len);
        memcpy(updated + prefix + replacement_len, end, suffix + 1U);
        FILE *fp = fopen(meta_path, "wb");
        ok = fp && fwrite(updated, 1, prefix + replacement_len + suffix, fp) ==
                       prefix + replacement_len + suffix;
        if (fp) {
            ok = fclose(fp) == 0 && ok;
        }
    } else {
        ok = false;
    }
    free(updated);
    free(json);
    return ok;
}

static bool flip_artifact_string_hex(const char *meta_path, const char *field) {
    size_t len = 0;
    char *json = read_file_text(meta_path, 1024 * 1024, &len);
    if (!json) {
        return false;
    }
    char needle[128];
    int nn = snprintf(needle, sizeof(needle), "\"%s\"", field);
    char *key = nn > 0 && (size_t)nn < sizeof(needle) ? strstr(json, needle) : NULL;
    char *colon = key ? strchr(key, ':') : NULL;
    char *quote = colon ? strchr(colon, '"') : NULL;
    bool ok = quote && quote[1] && quote[1] != '"';
    if (ok) {
        quote[1] = quote[1] == '0' ? '1' : '0';
        FILE *fp = fopen(meta_path, "wb");
        ok = fp && fwrite(json, 1, len, fp) == len;
        if (fp) {
            ok = fclose(fp) == 0 && ok;
        }
    }
    free(json);
    return ok;
}

static void add_third_test_node(const char *path) {
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return;
    }
    cbm_store_exec(store, "INSERT INTO nodes(project,label,name,qualified_name,file_path) "
                          "VALUES('test-proj','Function','baz','test-proj.baz','other.c');");
    cbm_store_close(store);
}

static bool fail_metadata_publish_hook(const char *stage, void *context) {
    (void)context;
    return strcmp(stage, "before_metadata_publish") != 0;
}

typedef struct {
    char payload[4096];
    char outside[4096];
    bool swapped;
} payload_swap_t;

static bool swap_payload_hook(const char *stage, void *context) {
    payload_swap_t *swap = (payload_swap_t *)context;
    if (strcmp(stage, "payload_opened") == 0 && !swap->swapped) {
#ifndef _WIN32
        swap->swapped =
            cbm_unlink(swap->payload) == 0 && symlink(swap->outside, swap->payload) == 0;
#else
        swap->swapped = false;
#endif
    }
    return true;
}

typedef struct {
    const char *db;
    const char *repo;
    int result;
} export_thread_args_t;

static void *run_artifact_export_thread(void *opaque) {
    export_thread_args_t *args = (export_thread_args_t *)opaque;
    args->result = cbm_artifact_export(args->db, args->repo, "test-proj", CBM_ARTIFACT_FAST);
    return NULL;
}

/* The decompressed size is driven by the zstd frame's own content-size header,
 * not the separately-stored original_size field (which travels in plaintext
 * artifact.json and is trivially editable). A mismatch between the two must be
 * rejected — this is the check that keeps the destination allocation and the
 * decoder capacity pinned to the same verified size, so a doctored size can
 * never make the decoder write past the buffer. */
TEST(artifact_import_rejects_size_mismatch) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.codebase-memory/artifact.json", g_repo);
    ASSERT_TRUE(bump_artifact_number(meta, "original_size", 4096)); /* claim 4 KiB too much */

    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    int rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_NEQ(rc, 0); /* must reject the mismatch, not import on the doctored size */

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_export_fast_roundtrip) {
    setup_artifact_test();
    create_test_db(g_db);

    /* Export with fast quality (zstd -3, no index stripping) */
    int rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    ASSERT_EQ(rc, 0);

    /* Verify artifact files exist */
    char zst[2048];
    ASSERT_TRUE(artifact_payload_path(g_repo, zst, sizeof(zst)));
    struct stat st;
    ASSERT_EQ(stat(zst, &st), 0);
    ASSERT_GT((int)st.st_size, 0);

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.codebase-memory/artifact.json", g_repo);
    ASSERT_EQ(stat(meta, &st), 0);

    /* Import to a new path */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_EQ(rc, 0);

    /* Verify imported DB has correct data */
    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    int nodes = cbm_store_count_nodes(s, "test-proj");
    int edges = cbm_store_count_edges(s, "test-proj");
    ASSERT_EQ(nodes, 2);
    ASSERT_EQ(edges, 1);
    cbm_store_close(s);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_export_best_roundtrip) {
    setup_artifact_test();
    create_test_db(g_db);

    /* Export with best quality (zstd -9, index stripping + VACUUM) */
    int rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_BEST);
    ASSERT_EQ(rc, 0);

    /* Source DB should be untouched (backup/compaction operates on the snapshot). */
    cbm_store_t *src = cbm_store_open_path(g_db);
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(cbm_store_count_nodes(src, "test-proj"), 2);
    cbm_store_close(src);

    /* Import and verify */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_EQ(rc, 0);

    cbm_store_t *s = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_count_nodes(s, "test-proj"), 2);
    ASSERT_EQ(cbm_store_count_edges(s, "test-proj"), 1);
    cbm_store_close(s);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_exists_check) {
    setup_artifact_test();
    create_test_db(g_db);

    /* No artifact yet */
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    /* Export creates the artifact */
    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    ASSERT_TRUE(cbm_artifact_exists(g_repo));

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_commit_hash) {
    setup_artifact_test();
    create_test_db(g_db);

    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    /* commit hash may be empty if repo is not a git repo, but should not crash */
    char *commit = cbm_artifact_commit(g_repo);
    /* For a non-git directory, commit will be NULL (git rev-parse HEAD fails) */
    free(commit);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_schema_version_mismatch) {
    setup_artifact_test();
    create_test_db(g_db);
    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    /* Overwrite artifact.json with incompatible schema version */
    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/.codebase-memory/artifact.json", g_repo);
    FILE *fp = fopen(meta, "w");
    ASSERT_NOT_NULL(fp);
    fprintf(fp, "{\"schema_version\": 999, \"original_size\": 1000}");
    fclose(fp);

    /* exists should return false for incompatible version */
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    /* Import should fail */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    int rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_NEQ(rc, 0);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_import_missing) {
    setup_artifact_test();

    /* Import from repo without artifact should fail gracefully */
    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported.db", g_tmpdir);
    int rc = cbm_artifact_import(g_repo, import_db);
    ASSERT_NEQ(rc, 0);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_gitattributes_created) {
    setup_artifact_test();
    create_test_db(g_db);

    cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);

    char ga[1024];
    snprintf(ga, sizeof(ga), "%s/.codebase-memory/.gitattributes", g_repo);
    struct stat st;
    ASSERT_EQ(stat(ga, &st), 0);

    /* Attribute ORDER is load-bearing: gitattributes apply left to right and
     * the `binary` macro expands to `-diff -merge -text`, so a trailing
     * `binary` unsets a preceding `merge=ours` (git check-attr merge reports
     * "unset" and concurrent artifact refreshes produce binary conflicts
     * instead of auto-resolving). The driver must come after the macro. */
    FILE *gaf = fopen(ga, "r");
    ASSERT_NOT_NULL(gaf);
    char content[512] = {0};
    size_t rd = fread(content, 1, sizeof(content) - 1, gaf);
    (void)fclose(gaf);
    ASSERT_TRUE(rd > 0);
    ASSERT_NOT_NULL(strstr(content, CBM_ARTIFACT_META " -diff merge=ours"));
    ASSERT_NOT_NULL(strstr(content, CBM_ARTIFACT_PATTERN " binary merge=ours"));
    ASSERT_TRUE(strstr(content, "merge=ours binary") == NULL);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_gitattributes_existing_v2_is_upgraded) {
    setup_artifact_test();
    create_test_db(g_db);
    char art_dir[1024];
    char ga[1024];
    snprintf(art_dir, sizeof(art_dir), "%s/%s", g_repo, CBM_ARTIFACT_DIR);
    snprintf(ga, sizeof(ga), "%s/.gitattributes", art_dir);
    ASSERT_TRUE(cbm_mkdir_p(art_dir, 0755));
    write_text_file(ga, "# user rule\n*.keep text\ngraph.db.zst binary merge=ours\n");

    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    FILE *fp = fopen(ga, "r");
    ASSERT_NOT_NULL(fp);
    char content[1024] = {0};
    ASSERT_TRUE(fread(content, 1, sizeof(content) - 1U, fp) > 0);
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_NOT_NULL(strstr(content, "*.keep text"));
    ASSERT_NOT_NULL(strstr(content, CBM_ARTIFACT_META " -diff merge=ours"));
    ASSERT_NOT_NULL(strstr(content, CBM_ARTIFACT_PATTERN " binary merge=ours"));
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_export_rename_failure_logs_specific_error) {
    setup_artifact_test();
    create_test_db(g_db);

    char art_dir[1024];
    snprintf(art_dir, sizeof(art_dir), "%s/.codebase-memory", g_repo);
    cbm_mkdir_p(art_dir, 0755);

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/%s", art_dir, CBM_ARTIFACT_META);
    cbm_mkdir_p(meta, 0755);

    capture_logs_start();
    int rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    const char *logs = capture_logs_end();

    ASSERT_NEQ(rc, 0);
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    ASSERT_NOT_NULL(cbm_artifact_export_last_error());
    ASSERT(strstr(cbm_artifact_export_last_error(), "write_metadata") != NULL);
    ASSERT(strstr(cbm_artifact_export_last_error(), "atomic_publish_failed") != NULL);
    ASSERT(strstr(logs, "msg=artifact.export") != NULL);
    ASSERT(strstr(logs, "stage=write_metadata") != NULL);
    ASSERT(strstr(logs, "err=atomic_publish_failed") != NULL);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(pipeline_persistence_export_failure_is_warning_after_db_publish) {
    setup_artifact_test();

    char src[1024];
    snprintf(src, sizeof(src), "%s/main.c", g_repo);
    write_text_file(src, "int main(void) { return 0; }\n");

    char art_dir[1024];
    snprintf(art_dir, sizeof(art_dir), "%s/.codebase-memory", g_repo);
    cbm_mkdir_p(art_dir, 0755);

    char meta[1024];
    snprintf(meta, sizeof(meta), "%s/%s", art_dir, CBM_ARTIFACT_META);
    cbm_mkdir_p(meta, 0755);

    cbm_pipeline_t *p = cbm_pipeline_new(g_repo, g_db, CBM_MODE_FAST);
    ASSERT_NOT_NULL(p);
    cbm_pipeline_set_persistence(p, true);

    capture_logs_start();
    int rc = cbm_pipeline_run(p);
    const char *logs = capture_logs_end();
    cbm_pipeline_free(p);

    ASSERT_EQ(rc, 0);
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    ASSERT(strstr(logs, "msg=pipeline.artifact_export_failed") != NULL);
    cbm_store_t *published = cbm_store_open_path_query(g_db);
    ASSERT_NOT_NULL(published);
    cbm_store_close(published);

    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_null_safety) {
    ASSERT_NEQ(cbm_artifact_export(NULL, "/tmp", "p", 0), 0);
    ASSERT_NEQ(cbm_artifact_export("/tmp/x.db", NULL, "p", 0), 0);
    ASSERT_NEQ(cbm_artifact_import(NULL, "/tmp/x.db"), 0);
    ASSERT_NEQ(cbm_artifact_import("/tmp", NULL), 0);
    ASSERT_FALSE(cbm_artifact_exists(NULL));
    ASSERT_NULL(cbm_artifact_commit(NULL));
    PASS();
}

/* ── git shell-out path safety ────────────────────────────────────────────────
 *
 * Artifact git operations now use a securely resolved executable and an argv
 * subprocess, so shell-looking repository bytes are passed literally. */
TEST(artifact_repo_path_argv_accepts_all_nonempty_paths) {
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("/home/user/repo"));
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("C:/Users/me/repo"));
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("/home/user/my repo"));
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("it's"));
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("a\";$(touch nope);`id`|repo"));
    ASSERT_TRUE(cbm_artifact_repo_path_is_shell_safe("/tmp/한글-프로젝트"));
    ASSERT_FALSE(cbm_artifact_repo_path_is_shell_safe(NULL));
    ASSERT_FALSE(cbm_artifact_repo_path_is_shell_safe(""));
    PASS();
}

/* #895: the FAST export path (watcher/incremental auto-update) read the
 * raw main-file bytes of a live WAL-mode store — committed rows still in
 * the -wal were missing and mid-checkpoint reads produced torn snapshots
 * that imported as page-corrupted caches. Export must snapshot
 * consistently (SQLite backup API) on BOTH quality levels. */
TEST(artifact_fast_export_snapshots_live_wal_store) {
    setup_artifact_test();
    enum { WAL_NODES = 60 };

    /* Live store: rows committed but NOT checkpointed into the main file —
     * exactly the state the watcher export runs against. */
    cbm_store_t *s = cbm_store_open_path(g_db);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "test-proj", "/tmp/test");
    for (int i = 0; i < WAL_NODES; i++) {
        char name[64];
        char qn[128];
        snprintf(name, sizeof(name), "walnode_%03d", i);
        snprintf(qn, sizeof(qn), "test-proj.mod.%s", name);
        cbm_node_t n = {.project = "test-proj",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "mod.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }

    /* Export WHILE the writer connection is still open. */
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    cbm_store_close(s);

    char import_db[1024];
    snprintf(import_db, sizeof(import_db), "%s/imported_wal.db", g_tmpdir);
    ASSERT_EQ(cbm_artifact_import(g_repo, import_db), 0);

    cbm_store_t *imp = cbm_store_open_path(import_db);
    ASSERT_NOT_NULL(imp);
    /* Torn snapshot = the WAL-resident rows are missing. */
    ASSERT_EQ(cbm_store_count_nodes(imp, "test-proj"), WAL_NODES);
    cbm_store_close(imp);
    PASS();
}

TEST(artifact_v3_metadata_binds_generation_hash_and_exact_sizes) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);

    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/%s/%s", g_repo, CBM_ARTIFACT_DIR, CBM_ARTIFACT_META);
    size_t len = 0;
    char *json = read_file_text(meta_path, 1024 * 1024, &len);
    ASSERT_NOT_NULL(json);
    yyjson_doc *doc = yyjson_read(json, len, 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *version = yyjson_obj_get(root, "schema_version");
    yyjson_val *payload = yyjson_obj_get(root, "payload");
    yyjson_val *generation = yyjson_obj_get(root, "generation");
    yyjson_val *hash = yyjson_obj_get(root, "compressed_sha256");
    yyjson_val *original = yyjson_obj_get(root, "original_size");
    yyjson_val *compressed = yyjson_obj_get(root, "compressed_size");
    ASSERT_TRUE(yyjson_is_uint(version));
    ASSERT_EQ(yyjson_get_uint(version), CBM_ARTIFACT_SCHEMA_VERSION);
    ASSERT_TRUE(yyjson_is_str(payload));
    ASSERT_TRUE(yyjson_is_str(generation));
    ASSERT_TRUE(yyjson_is_str(hash));
    ASSERT_STR_EQ(yyjson_get_str(generation), yyjson_get_str(hash));
    ASSERT_NOT_NULL(strstr(yyjson_get_str(payload), yyjson_get_str(hash)));
    ASSERT_TRUE(yyjson_is_uint(original));
    ASSERT_TRUE(yyjson_is_uint(compressed));
    ASSERT_GT(yyjson_get_uint(original), 0);

    char payload_path[4096];
    ASSERT_TRUE(artifact_payload_path(g_repo, payload_path, sizeof(payload_path)));
    struct stat st;
    ASSERT_EQ(stat(payload_path, &st), 0);
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_EQ((uint64_t)st.st_size, yyjson_get_uint(compressed));
    yyjson_doc_free(doc);
    free(json);
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_rejects_compressed_size_hash_and_generation_mismatches) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char meta[2048];
    snprintf(meta, sizeof(meta), "%s/%s/%s", g_repo, CBM_ARTIFACT_DIR, CBM_ARTIFACT_META);
    ASSERT_TRUE(bump_artifact_number(meta, "compressed_size", 1));
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/size-mismatch.db", g_tmpdir);
    ASSERT_NEQ(cbm_artifact_import(g_repo, dest), 0);

    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_TRUE(flip_artifact_string_hex(meta, "compressed_sha256"));
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_TRUE(flip_artifact_string_hex(meta, "generation"));
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_payload_tamper_fails_without_modifying_destination) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char payload[4096];
    ASSERT_TRUE(artifact_payload_path(g_repo, payload, sizeof(payload)));
    FILE *fp = fopen(payload, "rb+");
    ASSERT_NOT_NULL(fp);
    ASSERT_EQ(fseek(fp, 12, SEEK_SET), 0);
    int byte = fgetc(fp);
    ASSERT_NEQ(byte, EOF);
    ASSERT_EQ(fseek(fp, 12, SEEK_SET), 0);
    ASSERT_NEQ(fputc(byte ^ 0x5a, fp), EOF);
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/existing.db", g_tmpdir);
    cbm_store_t *old = cbm_store_open_path(dest);
    ASSERT_NOT_NULL(old);
    ASSERT_EQ(cbm_store_upsert_project(old, "test-proj", "/old"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(old, "INSERT INTO nodes(project,label,name,qualified_name,file_path) "
                                  "VALUES('test-proj','Function','old','test-proj.old','old.c');"),
              CBM_STORE_OK);
    cbm_store_close(old);
    ASSERT_NEQ(cbm_artifact_import(g_repo, dest), 0);
    old = cbm_store_open_path_query(dest);
    ASSERT_NOT_NULL(old);
    ASSERT_EQ(cbm_store_count_nodes(old, "test-proj"), 1);
    cbm_store_close(old);
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_rejects_v2_oversized_metadata_and_uint64_limits) {
    setup_artifact_test();
    char art_dir[2048];
    snprintf(art_dir, sizeof(art_dir), "%s/%s", g_repo, CBM_ARTIFACT_DIR);
    ASSERT_TRUE(cbm_mkdir_p(art_dir, 0755));
    char meta[2048];
    snprintf(meta, sizeof(meta), "%s/%s", art_dir, CBM_ARTIFACT_META);
    write_text_file(meta, "{\"schema_version\":2,\"original_size\":1,"
                          "\"compressed_size\":1}");
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/v2.db", g_tmpdir);
    ASSERT_NEQ(cbm_artifact_import(g_repo, dest), 0);

    FILE *fp = fopen(meta, "wb");
    ASSERT_NOT_NULL(fp);
    char block[1024];
    memset(block, ' ', sizeof(block));
    for (int i = 0; i < 1025; i++) {
        ASSERT_EQ(fwrite(block, 1, sizeof(block), fp), sizeof(block));
    }
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_FALSE(cbm_artifact_exists(g_repo));

    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_TRUE(replace_artifact_number(meta, "original_size", "18446744073709551615"));
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_metadata_publish_failure_preserves_previous_pair) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char first_payload[256];
    ASSERT_TRUE(artifact_metadata_payload_name(g_repo, first_payload, sizeof(first_payload)));
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/%s/%s", g_repo, CBM_ARTIFACT_DIR, CBM_ARTIFACT_META);
    size_t before_len = 0;
    char *before = read_file_text(meta_path, 1024 * 1024, &before_len);
    ASSERT_NOT_NULL(before);

    add_third_test_node(g_db);
    cbm_artifact_set_test_hook(fail_metadata_publish_hook, NULL);
    int export_rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    cbm_artifact_set_test_hook(NULL, NULL);
    ASSERT_NEQ(export_rc, 0);
    size_t after_len = 0;
    char *after = read_file_text(meta_path, 1024 * 1024, &after_len);
    ASSERT_NOT_NULL(after);
    ASSERT_EQ(after_len, before_len);
    ASSERT_MEM_EQ(after, before, before_len);
    char still_payload[256];
    ASSERT_TRUE(artifact_metadata_payload_name(g_repo, still_payload, sizeof(still_payload)));
    ASSERT_STR_EQ(still_payload, first_payload);
    ASSERT_TRUE(cbm_artifact_exists(g_repo));
    char imported[2048];
    snprintf(imported, sizeof(imported), "%s/preserved.db", g_tmpdir);
    ASSERT_EQ(cbm_artifact_import(g_repo, imported), 0);
    cbm_store_t *store = cbm_store_open_path_query(imported);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_count_nodes(store, "test-proj"), 2);
    cbm_store_close(store);
    free(before);
    free(after);
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_import_uses_sqlite_publish_with_active_reader) {
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/live-reader.db", g_tmpdir);
    cbm_store_t *seed = cbm_store_open_path(dest);
    ASSERT_NOT_NULL(seed);
    ASSERT_EQ(cbm_store_upsert_project(seed, "test-proj", "/old"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_exec(seed, "INSERT INTO nodes(project,label,name,qualified_name,file_path) "
                                   "VALUES('test-proj','Function','old','test-proj.old','old.c');"),
              CBM_STORE_OK);
    cbm_store_close(seed);
    struct stat before;
    ASSERT_EQ(stat(dest, &before), 0);

    cbm_store_t *reader = cbm_store_open_path_query(dest);
    ASSERT_NOT_NULL(reader);
    sqlite3 *raw = cbm_store_get_db(reader);
    ASSERT_NOT_NULL(raw);
    ASSERT_EQ(sqlite3_exec(raw, "BEGIN;", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(cbm_store_count_nodes(reader, "test-proj"), 1);
    ASSERT_EQ(cbm_artifact_import(g_repo, dest), 0);
    ASSERT_EQ(cbm_store_count_nodes(reader, "test-proj"), 1);
    ASSERT_EQ(sqlite3_exec(raw, "COMMIT;", NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(cbm_store_count_nodes(reader, "test-proj"), 2);
    cbm_store_close(reader);
    struct stat after;
    ASSERT_EQ(stat(dest, &after), 0);
#ifndef _WIN32
    ASSERT_EQ(before.st_dev, after.st_dev);
    ASSERT_EQ(before.st_ino, after.st_ino);
#endif
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_concurrent_exports_publish_one_complete_generation) {
    setup_artifact_test();
    create_test_db(g_db);
    char second_db[2048];
    snprintf(second_db, sizeof(second_db), "%s/second.db", g_tmpdir);
    create_test_db(second_db);
    add_third_test_node(second_db);
    export_thread_args_t first = {.db = g_db, .repo = g_repo, .result = -99};
    export_thread_args_t second = {.db = second_db, .repo = g_repo, .result = -99};
    pthread_t first_thread;
    pthread_t second_thread;
    ASSERT_EQ(pthread_create(&first_thread, NULL, run_artifact_export_thread, &first), 0);
    ASSERT_EQ(pthread_create(&second_thread, NULL, run_artifact_export_thread, &second), 0);
    ASSERT_EQ(pthread_join(first_thread, NULL), 0);
    ASSERT_EQ(pthread_join(second_thread, NULL), 0);
    ASSERT_EQ(first.result, 0);
    ASSERT_EQ(second.result, 0);
    ASSERT_TRUE(cbm_artifact_exists(g_repo));
    char imported[2048];
    snprintf(imported, sizeof(imported), "%s/concurrent.db", g_tmpdir);
    ASSERT_EQ(cbm_artifact_import(g_repo, imported), 0);
    cbm_store_t *store = cbm_store_open_path_query(imported);
    ASSERT_NOT_NULL(store);
    int nodes = cbm_store_count_nodes(store, "test-proj");
    ASSERT_TRUE(nodes == 2 || nodes == 3);
    ASSERT_EQ(cbm_store_count_edges(store, "test-proj"), 1);
    cbm_store_close(store);
    cleanup_dir(g_tmpdir);
    PASS();
}

TEST(artifact_rejects_symlink_fifo_and_payload_swap) {
#ifndef _WIN32
    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    char payload[4096];
    ASSERT_TRUE(artifact_payload_path(g_repo, payload, sizeof(payload)));
    char outside[4096];
    snprintf(outside, sizeof(outside), "%s/outside.zst", g_tmpdir);
    write_text_file(outside, "not an artifact");
    payload_swap_t swap = {0};
    snprintf(swap.payload, sizeof(swap.payload), "%s", payload);
    snprintf(swap.outside, sizeof(swap.outside), "%s", outside);
    cbm_artifact_set_test_hook(swap_payload_hook, &swap);
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/swapped.db", g_tmpdir);
    int swap_rc = cbm_artifact_import(g_repo, dest);
    cbm_artifact_set_test_hook(NULL, NULL);
    ASSERT_TRUE(swap.swapped);
    ASSERT_NEQ(swap_rc, 0);
    ASSERT_FALSE(cbm_file_exists(dest));
    cleanup_dir(g_tmpdir);

    setup_artifact_test();
    create_test_db(g_db);
    ASSERT_EQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    ASSERT_TRUE(artifact_payload_path(g_repo, payload, sizeof(payload)));
    ASSERT_EQ(cbm_unlink(payload), 0);
    ASSERT_EQ(mkfifo(payload, 0600), 0);
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    cleanup_dir(g_tmpdir);

    setup_artifact_test();
    char external_dir[2048];
    snprintf(external_dir, sizeof(external_dir), "%s/external", g_tmpdir);
    ASSERT_TRUE(cbm_mkdir_p(external_dir, 0755));
    char art_link[2048];
    snprintf(art_link, sizeof(art_link), "%s/%s", g_repo, CBM_ARTIFACT_DIR);
    ASSERT_EQ(symlink(external_dir, art_link), 0);
    create_test_db(g_db);
    ASSERT_NEQ(cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST), 0);
    cleanup_dir(g_tmpdir);
#endif
    PASS();
}

TEST(artifact_gitattributes_nofollow_and_git_resolver_fail_closed) {
#ifndef _WIN32
    setup_artifact_test();
    create_test_db(g_db);
    char art_dir[2048];
    snprintf(art_dir, sizeof(art_dir), "%s/%s", g_repo, CBM_ARTIFACT_DIR);
    ASSERT_TRUE(cbm_mkdir_p(art_dir, 0755));
    char sentinel[2048];
    char attributes[2048];
    snprintf(sentinel, sizeof(sentinel), "%s/sentinel", g_tmpdir);
    snprintf(attributes, sizeof(attributes), "%s/.gitattributes", art_dir);
    write_text_file(sentinel, "unchanged\n");
    ASSERT_EQ(symlink(sentinel, attributes), 0);

    char fake_git[2048];
    char marker[2048];
    snprintf(fake_git, sizeof(fake_git), "%s/git", g_tmpdir);
    snprintf(marker, sizeof(marker), "%s/ran", g_tmpdir);
    char script[4096];
    snprintf(script, sizeof(script), "#!/bin/sh\ntouch '%s'\nexit 0\n", marker);
    write_text_file(fake_git, script);
    ASSERT_EQ(chmod(fake_git, 0755), 0);
    const char *old_path_env = getenv("PATH");
    const char *old_bin_env = getenv("CBM_GIT_BIN");
    char *old_path = old_path_env ? strdup(old_path_env) : NULL;
    char *old_bin = old_bin_env ? strdup(old_bin_env) : NULL;
    ASSERT_EQ(cbm_setenv("PATH", ".", 1), 0);
    ASSERT_EQ(cbm_setenv("CBM_GIT_BIN", "git", 1), 0);
    int export_rc = cbm_artifact_export(g_db, g_repo, "test-proj", CBM_ARTIFACT_FAST);
    if (old_path) {
        (void)cbm_setenv("PATH", old_path, 1);
    } else {
        (void)cbm_unsetenv("PATH");
    }
    if (old_bin) {
        (void)cbm_setenv("CBM_GIT_BIN", old_bin, 1);
    } else {
        (void)cbm_unsetenv("CBM_GIT_BIN");
    }
    free(old_path);
    free(old_bin);
    ASSERT_EQ(export_rc, 0);
    ASSERT_FALSE(cbm_file_exists(marker));
    size_t sentinel_len = 0;
    char *sentinel_data = read_file_text(sentinel, 128, &sentinel_len);
    ASSERT_NOT_NULL(sentinel_data);
    ASSERT_STR_EQ(sentinel_data, "unchanged\n");
    free(sentinel_data);
    struct stat lst;
    ASSERT_EQ(lstat(attributes, &lst), 0);
    ASSERT_TRUE(S_ISLNK(lst.st_mode));
    cleanup_dir(g_tmpdir);
#endif
    PASS();
}

TEST(artifact_long_path_and_payload_escape_fail_gracefully) {
    char *long_repo = malloc(20000);
    ASSERT_NOT_NULL(long_repo);
    memset(long_repo, 'a', 19999);
    long_repo[0] = '/';
    long_repo[19999] = '\0';
    ASSERT_NEQ(cbm_artifact_export("/tmp/missing.db", long_repo, "p", CBM_ARTIFACT_FAST), 0);
    ASSERT_FALSE(cbm_artifact_exists(long_repo));
    free(long_repo);

    setup_artifact_test();
    char art_dir[2048];
    snprintf(art_dir, sizeof(art_dir), "%s/%s", g_repo, CBM_ARTIFACT_DIR);
    ASSERT_TRUE(cbm_mkdir_p(art_dir, 0755));
    char meta[2048];
    snprintf(meta, sizeof(meta), "%s/%s", art_dir, CBM_ARTIFACT_META);
    write_text_file(
        meta, "{\"schema_version\":3,\"payload\":\"../outside\","
              "\"generation\":\"0000000000000000000000000000000000000000000000000000000000000000\","
              "\"compressed_sha256\":"
              "\"0000000000000000000000000000000000000000000000000000000000000000\","
              "\"original_size\":1,\"compressed_size\":1}");
    ASSERT_FALSE(cbm_artifact_exists(g_repo));
    cleanup_dir(g_tmpdir);
    PASS();
}

/* #895 (import half): page-level corruption must be refused at import.
 * The shallow integrity check only sanity-checks the projects table; the
 * deep variant runs PRAGMA quick_check and catches corrupt pages. */
TEST(store_deep_integrity_detects_page_corruption) {
    setup_artifact_test();
    enum { DEEP_NODES = 800, PAGE = 4096, ZERO_PAGES = 10 };
    char db2[1024];
    snprintf(db2, sizeof(db2), "%s/deep.db", g_tmpdir);
    cbm_store_t *s = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(s);
    cbm_store_upsert_project(s, "deep", "/tmp/deep");
    for (int i = 0; i < DEEP_NODES; i++) {
        char name[64];
        char qn[192];
        snprintf(name, sizeof(name), "deep_probe_%04d", i);
        snprintf(qn, sizeof(qn), "deep.rather.long.module.path.for.page.fill.%s_pad_pad_pad", name);
        cbm_node_t n = {.project = "deep",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "deep.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }
    cbm_store_close(s);

    /* Healthy file passes the deep check. */
    cbm_store_t *ok = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(ok);
    ASSERT_TRUE(cbm_store_check_integrity_deep(ok));
    cbm_store_close(ok);

    /* Zero a mid-file band and the deep check must refuse. */
    FILE *f = fopen(db2, "rb+");
    ASSERT_NOT_NULL(f);
    (void)fseek(f, 0, SEEK_END);
    long pages = ftell(f) / PAGE;
    ASSERT_TRUE(pages > ZERO_PAGES + 6);
    char zero[PAGE];
    memset(zero, 0, sizeof(zero));
    (void)fseek(f, (pages / 2) * (long)PAGE, SEEK_SET);
    for (int i = 0; i < ZERO_PAGES; i++) {
        ASSERT_EQ(fwrite(zero, 1, PAGE, f), (size_t)PAGE);
    }
    (void)fclose(f);

    cbm_store_t *bad = cbm_store_open_path(db2);
    ASSERT_NOT_NULL(bad);
    ASSERT_FALSE(cbm_store_check_integrity_deep(bad));
    cbm_store_close(bad);
    PASS();
}

SUITE(artifact) {
    RUN_TEST(artifact_fast_export_snapshots_live_wal_store);
    RUN_TEST(artifact_v3_metadata_binds_generation_hash_and_exact_sizes);
    RUN_TEST(artifact_rejects_compressed_size_hash_and_generation_mismatches);
    RUN_TEST(artifact_payload_tamper_fails_without_modifying_destination);
    RUN_TEST(artifact_rejects_v2_oversized_metadata_and_uint64_limits);
    RUN_TEST(artifact_metadata_publish_failure_preserves_previous_pair);
    RUN_TEST(artifact_import_uses_sqlite_publish_with_active_reader);
    RUN_TEST(artifact_concurrent_exports_publish_one_complete_generation);
    RUN_TEST(artifact_rejects_symlink_fifo_and_payload_swap);
    RUN_TEST(artifact_gitattributes_nofollow_and_git_resolver_fail_closed);
    RUN_TEST(artifact_long_path_and_payload_escape_fail_gracefully);
    RUN_TEST(store_deep_integrity_detects_page_corruption);
    RUN_TEST(artifact_repo_path_argv_accepts_all_nonempty_paths);
    RUN_TEST(artifact_export_fast_roundtrip);
    RUN_TEST(artifact_export_best_roundtrip);
    RUN_TEST(artifact_exists_check);
    RUN_TEST(artifact_commit_hash);
    RUN_TEST(artifact_schema_version_mismatch);
    RUN_TEST(artifact_import_missing);
    RUN_TEST(artifact_gitattributes_created);
    RUN_TEST(artifact_gitattributes_existing_v2_is_upgraded);
    RUN_TEST(artifact_export_rename_failure_logs_specific_error);
    RUN_TEST(pipeline_persistence_export_failure_is_warning_after_db_publish);
    RUN_TEST(artifact_import_rejects_size_mismatch);
    RUN_TEST(artifact_null_safety);
}
