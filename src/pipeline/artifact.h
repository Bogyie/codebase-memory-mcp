/*
 * artifact.h — Persistent artifact export/import for team sharing.
 *
 * Exports the SQLite knowledge graph as a zstd-compressed, immutable
 * generation in .codebase-memory/. artifact.json is the atomic generation
 * pointer; teammates import only when its size and SHA-256 agree with the
 * referenced payload.
 */
#ifndef CBM_ARTIFACT_H
#define CBM_ARTIFACT_H

#include <stdbool.h>

/* v3 makes artifact.json an authenticated generation manifest. v2 artifacts
 * are deliberately rejected: they have neither a compressed-payload digest
 * nor an immutable payload name, so a reader cannot bind metadata and bytes
 * to one generation during concurrent publication. */
#define CBM_ARTIFACT_SCHEMA_VERSION 3

/* Legacy v2 name, retained for source compatibility only. v3 payloads are
 * named graph.db.<compressed-sha256>.zst and selected by artifact.json. */
#define CBM_ARTIFACT_FILENAME "graph.db.zst"
#define CBM_ARTIFACT_PATTERN "graph.db.*.zst"
#define CBM_ARTIFACT_META "artifact.json"
#define CBM_ARTIFACT_DIR ".codebase-memory"

/* Export quality levels */
enum {
    CBM_ARTIFACT_FAST = 0, /* zstd -3, no index stripping (watcher path) */
    CBM_ARTIFACT_BEST = 1, /* zstd -9 + checked index stripping/VACUUM */
};

/* Export DB to a generation-addressed .codebase-memory artifact.
 * quality: CBM_ARTIFACT_FAST or CBM_ARTIFACT_BEST.
 * Creates .codebase-memory/ dir, .gitattributes, and artifact.json.
 * Returns 0 on success, -1 on error. */
int cbm_artifact_export(const char *db_path, const char *repo_path, const char *project_name,
                        int quality);

/* Get details for the most recent export failure on this thread.
 * Returns NULL if no export error is recorded. */
const char *cbm_artifact_export_last_error(void);

/* Import the generation referenced by artifact.json to cache_db_path.
 * Decompresses to an exclusive same-directory temp, verifies exact sizes and
 * SHA-256, runs a deep integrity check, recreates indexes, then publishes via
 * SQLite backup (safe for existing readers and WAL state).
 * Returns 0 on success, -1 on error. */
int cbm_artifact_import(const char *repo_path, const char *cache_db_path);

/* Check if a compatible, internally consistent artifact generation exists. */
bool cbm_artifact_exists(const char *repo_path);

/* Get the git commit hash from artifact metadata. Caller must free().
 * Returns NULL if artifact doesn't exist or has no commit field. */
char *cbm_artifact_commit(const char *repo_path);

/* Compatibility predicate retained for callers/tests. Artifact git operations
 * are argv-based and never invoke a shell, so every non-empty path is accepted,
 * including quotes, metacharacters, spaces, and Unicode. */
bool cbm_artifact_repo_path_is_shell_safe(const char *repo_path);

/* Deterministic race/failure injection for regression tests. Production code
 * must leave this unset. Returning false aborts the named stage; known stages
 * are "payload_opened" and "before_metadata_publish". */
typedef bool (*cbm_artifact_test_hook_fn)(const char *stage, void *context);
void cbm_artifact_set_test_hook(cbm_artifact_test_hook_fn hook, void *context);

#endif /* CBM_ARTIFACT_H */
