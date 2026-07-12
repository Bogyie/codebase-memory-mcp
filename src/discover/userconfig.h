/*
 * userconfig.h — User-defined file extension → language mappings.
 *
 * Reads extra_extensions from two optional JSON config files:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 *
 * Format:
 *   {"extra_extensions": {".blade.php": "php", ".mjs": "javascript"}}
 *
 * The language string matching is case-insensitive.
 */
#ifndef CBM_USERCONFIG_H
#define CBM_USERCONFIG_H

#include "cbm.h" /* CBMLanguage */

/* Bump whenever extraction, grammar, graph, or persistence semantics change in
 * a way that requires unchanged repositories to receive one full rebuild. */
#define CBM_INDEXER_SEMANTICS_VERSION "4"

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    char *ext;        /* file extension including dot, e.g. ".blade.php" */
    CBMLanguage lang; /* resolved language enum */
} cbm_userext_t;

typedef struct {
    cbm_userext_t *entries; /* heap-allocated array */
    int count;              /* number of entries */
} cbm_userconfig_t;

/* Opaque content-generation snapshot for the two config files.  The
 * pipeline uses this to make the configuration selected for discovery part
 * of the completed index generation without exposing path/hash bookkeeping
 * through the long-standing cbm_userconfig_t ABI. */
typedef struct cbm_userconfig_snapshot cbm_userconfig_snapshot_t;

typedef enum {
    CBM_USERCONFIG_AUX_PATH_ALIAS = 1,
    CBM_USERCONFIG_AUX_PKGMAP = 2,
} cbm_userconfig_aux_kind_t;

/* Presence selected when the auxiliary-input snapshot was captured. UNKNOWN
 * means that no complete snapshot is available and must be treated as an
 * input-generation failure by pipeline consumers. */
typedef enum {
    CBM_USERCONFIG_AUX_SOURCE_UNKNOWN = -1,
    CBM_USERCONFIG_AUX_SOURCE_ABSENT = 0,
    CBM_USERCONFIG_AUX_SOURCE_PRESENT = 1,
    CBM_USERCONFIG_AUX_SOURCE_UNREADABLE = 2,
} cbm_userconfig_aux_source_state_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Load user config from global + project files, merge (project wins).
 * repo_path: absolute path to the repository root (for project config).
 * Returns a heap-allocated cbm_userconfig_t (caller must free via
 * cbm_userconfig_free). Returns NULL only on allocation failure.
 * Missing config files are silently ignored.
 */
cbm_userconfig_t *cbm_userconfig_load(const char *repo_path);

/* Load the same effective configuration as cbm_userconfig_load(), while also
 * returning a snapshot of the global/project files that produced it.  Each
 * readable file is parsed from the exact byte buffer hashed by the stable
 * file helper; missing-file state is captured explicitly.  Both returned
 * objects are caller-owned. */
cbm_userconfig_t *cbm_userconfig_load_with_snapshot(const char *repo_path,
                                                    cbm_userconfig_snapshot_t **out_snapshot);

/* Deterministic SHA-256 over the global/project presence states and content
 * hashes.  The returned pointer is owned by snapshot and remains valid until
 * cbm_userconfig_snapshot_free(). */
const char *cbm_userconfig_snapshot_fingerprint(const cbm_userconfig_snapshot_t *snapshot);
const char *cbm_userconfig_snapshot_config_fingerprint(const cbm_userconfig_snapshot_t *snapshot);

/* SHA-256 of the selected project-config bytes, or NULL when that source was
 * absent/unreadable. This lets other consumers of .codebase-memory.json bind
 * their independently parsed bytes to the exact user-config generation. */
const char *cbm_userconfig_snapshot_project_sha256(const cbm_userconfig_snapshot_t *snapshot);

/* Return 1 and expose the selected project's stable content hash/length when
 * it was readable, otherwise return 0 and clear the outputs. */
int cbm_userconfig_snapshot_project_source(const cbm_userconfig_snapshot_t *snapshot,
                                           const char **out_sha256, size_t *out_len);

/* After discovery has resolved excluded subtrees, capture every auxiliary
 * repository input consumed outside the normal file-hash set (package
 * manifests and ts/jsconfig path-alias files). Recomputes the fingerprint. */
int cbm_userconfig_snapshot_capture_auxiliary(cbm_userconfig_snapshot_t *snapshot,
                                              const char *repo_path, char **excluded_dirs,
                                              int excluded_count);
int cbm_userconfig_snapshot_capture_git_head(cbm_userconfig_snapshot_t *snapshot,
                                             const char *repo_path, bool enabled);
const char *cbm_userconfig_snapshot_git_head(const cbm_userconfig_snapshot_t *snapshot);
const char *cbm_userconfig_snapshot_git_history_sha256(const cbm_userconfig_snapshot_t *snapshot);
int cbm_userconfig_snapshot_set_git_context(cbm_userconfig_snapshot_t *snapshot,
                                            const char *context_sha256);
/* Bind the mode-independent FULL discovery selection/coverage generation into
 * both the base-routing and complete input fingerprints. */
int cbm_userconfig_snapshot_set_discovery(cbm_userconfig_snapshot_t *snapshot,
                                          const char *discovery_sha256);
const char *cbm_userconfig_snapshot_git_context_sha256(const cbm_userconfig_snapshot_t *snapshot);

/* Query the immutable presence state selected for one optional repository
 * input. A path omitted from a successfully captured scan is ABSENT; this is
 * distinct from UNKNOWN so optional loaders can skip it without weakening
 * fail-closed generation verification. */
cbm_userconfig_aux_source_state_t cbm_userconfig_snapshot_auxiliary_source_state(
    const cbm_userconfig_snapshot_t *snapshot, const char *rel_path);

/* Bind an auxiliary loader's same-descriptor source hash to the generation
 * selected above. Returns 0 only for an exact tracked relpath/hash match. */
int cbm_userconfig_snapshot_verify_auxiliary_source(cbm_userconfig_snapshot_t *snapshot,
                                                    const char *rel_path,
                                                    const char *source_sha256);
void cbm_userconfig_snapshot_note_auxiliary_read_failure(cbm_userconfig_snapshot_t *snapshot,
                                                         const char *rel_path);
void cbm_userconfig_snapshot_note_auxiliary_consumer_failure(cbm_userconfig_snapshot_t *snapshot,
                                                             cbm_userconfig_aux_kind_t kind);
void cbm_userconfig_snapshot_finish_auxiliary_consumer(cbm_userconfig_snapshot_t *snapshot,
                                                       cbm_userconfig_aux_kind_t kind);

/* Re-read both paths through the stable file helper and verify that their
 * effective presence/content still matches the loaded snapshot.  Returns 0
 * on equality and -1 on a changed or unverifiable generation. */
int cbm_userconfig_snapshot_verify(const cbm_userconfig_snapshot_t *snapshot);

/* Free a snapshot returned by cbm_userconfig_load_with_snapshot(). NULL-safe. */
void cbm_userconfig_snapshot_free(cbm_userconfig_snapshot_t *snapshot);

/*
 * Look up a file extension in the user config.
 * ext: extension including dot, e.g. ".blade.php"
 * Returns the mapped CBMLanguage, or CBM_LANG_COUNT if not found.
 */
CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext);

/* Free a cbm_userconfig_t returned by cbm_userconfig_load. NULL-safe. */
void cbm_userconfig_free(cbm_userconfig_t *cfg);

/* ── Integration hook ───────────────────────────────────────────── */

/*
 * Set the process-global user config that cbm_language_for_extension()
 * will consult before the built-in table.
 * cfg may be NULL to clear the override.
 * Not thread-safe — call before spawning worker threads.
 */
void cbm_set_user_lang_config(const cbm_userconfig_t *cfg);

/*
 * Get the currently active process-global user config.
 * Returns NULL if none has been set.
 * Called internally by cbm_language_for_extension().
 */
const cbm_userconfig_t *cbm_get_user_lang_config(void);

#endif /* CBM_USERCONFIG_H */
