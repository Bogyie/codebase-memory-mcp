/*
 * discover.h — File discovery, language detection, and gitignore matching.
 *
 * Provides:
 *   - Language detection from filename/extension (CBM_SZ_64 languages)
 *   - .m file disambiguation (Objective-C vs Magma vs MATLAB)
 *   - Gitignore-style pattern parsing and matching
 *   - Recursive directory walk with hardcoded + gitignore filtering
 *
 * Depends on: foundation (platform.h for file ops), cbm.h (CBMLanguage enum)
 */
#ifndef CBM_DISCOVER_H
#define CBM_DISCOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Use the existing CBMLanguage enum from extraction layer */
#include "cbm.h"

/* ── Language detection ──────────────────────────────────────────── */

/* Detect language from a filename (basename only, not full path).
 * Checks special filenames first (Makefile, CMakeLists.txt, etc.),
 * then falls back to extension-based lookup.
 * Returns CBM_LANG_COUNT if unknown. */
CBMLanguage cbm_language_for_filename(const char *filename);

/* Detect language from a file extension (including the dot, e.g. ".go").
 * Returns CBM_LANG_COUNT if unknown. */
CBMLanguage cbm_language_for_extension(const char *ext);

/* Get the human-readable name for a language enum value.
 * Returns "Unknown" for CBM_LANG_COUNT or out-of-range values. */
const char *cbm_language_name(CBMLanguage lang);

/* Disambiguate .m files by reading first 4KB of content.
 * Returns CBM_LANG_OBJC, CBM_LANG_MAGMA, or CBM_LANG_MATLAB.
 * On read failure, defaults to CBM_LANG_MATLAB. */
CBMLanguage cbm_disambiguate_m(const char *path);

/* Classify an already captured .m source generation. The buffer need not be
 * NUL-terminated; at most the first 4 KiB are inspected. */
CBMLanguage cbm_disambiguate_m_content(const char *source, size_t source_len);

/* ── Gitignore pattern matching ──────────────────────────────────── */

typedef struct cbm_gitignore cbm_gitignore_t;

typedef enum {
    CBM_GITIGNORE_LOAD_ERROR = -1,
    CBM_GITIGNORE_LOAD_MISSING = 0,
    CBM_GITIGNORE_LOAD_OK = 1,
} cbm_gitignore_load_result_t;

/* Parse gitignore patterns from a file. Returns NULL on error (file not found, etc.).
 * Caller must call cbm_gitignore_free(). */
cbm_gitignore_t *cbm_gitignore_load(const char *path);

/* Load while distinguishing an absent optional file from an allocation or I/O
 * failure. On MISSING and ERROR, *out is NULL. Discovery uses this form so an
 * unreadable or partially parsed ignore file cannot silently become an empty
 * matcher and publish an over-inclusive index. */
cbm_gitignore_load_result_t cbm_gitignore_load_ex(const char *path, cbm_gitignore_t **out);

/* Stable regular-file variant used by discovery snapshots. On OK, sha256 is
 * the digest of the exact bytes parsed into *out. Optional absent files return
 * MISSING with an empty digest. */
cbm_gitignore_load_result_t cbm_gitignore_load_hashed(const char *path, cbm_gitignore_t **out,
                                                      char sha256[65]);

/* Allocation-/complexity-aware tri-state matching. Returns false only when a
 * bounded matcher cannot safely evaluate the path; *result is -1 (explicit
 * include), 0 (no rule), or 1 (ignore) on success. */
bool cbm_gitignore_match_result_ex(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir,
                                   int *result);

/* Parse gitignore patterns from a string (for testing).
 * Caller must call cbm_gitignore_free(). */
cbm_gitignore_t *cbm_gitignore_parse(const char *content);

/* Check if a relative path matches any gitignore pattern.
 * rel_path should use '/' separators. is_dir indicates if path is a directory. */
bool cbm_gitignore_matches(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir);

/* Free a gitignore matcher. NULL-safe. */
void cbm_gitignore_free(cbm_gitignore_t *gi);

/* Append all patterns from src into dst. dst takes ownership of deep copies
 * of each src pattern; src is unchanged and must still be freed by the caller.
 * NULL-safe on either argument.
 * Returns true on success (or when there is nothing to merge). Returns false on
 * allocation failure, in which case dst is left exactly as it was (atomic) — no
 * partial merge. Callers must treat failure as fatal rather than dropping src. */
bool cbm_gitignore_merge(cbm_gitignore_t *dst, const cbm_gitignore_t *src);

/* ── Directory skip / suffix filters ─────────────────────────────── */

/* Index mode controls filtering aggressiveness.
 * IMPORTANT: these values MUST match pipeline.h exactly.  A previous
 * mismatch (this header had FAST=1, pipeline.h has FAST=2) caused
 * fast-mode filtering to silently no-op depending on include order —
 * the pipeline passed value 2, discover.c compared against 1, and no
 * files got filtered. */
#ifndef CBM_INDEX_MODE_T_DEFINED
#define CBM_INDEX_MODE_T_DEFINED
typedef enum {
    CBM_MODE_FULL = 0,     /* parse everything supported */
    CBM_MODE_MODERATE = 1, /* aggressive filtering + similarity/semantic edges */
    CBM_MODE_FAST = 2,     /* aggressive filtering + no similarity/semantic edges */
} cbm_index_mode_t;
#endif

/* Check if a directory name should always be skipped (e.g. .git, node_modules).
 * Only checks the basename, not the full path. */
bool cbm_should_skip_dir(const char *dirname, cbm_index_mode_t mode);

/* Check if a file has a suffix that should be skipped (e.g. .pyc, .png). */
bool cbm_has_ignored_suffix(const char *filename, cbm_index_mode_t mode);

/* Check if a specific filename should be skipped in fast mode (e.g. LICENSE, go.sum). */
bool cbm_should_skip_filename(const char *filename, cbm_index_mode_t mode);

/* Check if a path matches fast-mode substring patterns (e.g. .d.ts, .pb.go). */
bool cbm_matches_fast_pattern(const char *filename, cbm_index_mode_t mode);

/* ── File discovery ──────────────────────────────────────────────── */

typedef struct {
    char *path;           /* absolute path (heap-allocated) */
    char *rel_path;       /* relative to repo root (heap-allocated) */
    CBMLanguage language; /* detected language */
    int64_t size;         /* file size in bytes */
    /* Non-empty only when language selection depends on content (.m). It
     * hashes the exact stable generation passed to the classifier. */
    char selection_sha256[65];
} cbm_file_info_t;

typedef struct {
    cbm_index_mode_t mode;   /* CBM_MODE_FULL or CBM_MODE_FAST */
    const char *ignore_file; /* path to .cbmignore file, or NULL */
    int64_t max_file_size;   /* 0 = no limit */
    /* Optional exact pipeline output path. That file and only its SQLite
     * sidecars/private staging siblings are omitted from discovery entirely,
     * so publishing an index inside the repository cannot invalidate its own
     * next input fingerprint. The snapshot owns a copy during verification. */
    const char *exclude_output_path;
} cbm_discover_opts_t;

/* Walk a repository directory tree and discover all source files.
 * Applies hardcoded filters, gitignore patterns, and language detection.
 * Returns 0 on success, -1 on error.
 * Caller must call cbm_discover_free() on the results. */
int cbm_discover(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                 int *count);

/* Like cbm_discover(), but also reports the directory subtrees that were
 * skipped during the walk (hardcoded ALWAYS_SKIP/FAST_SKIP dirs + gitignore
 * matches), so callers can surface which subtrees were dropped (#411).
 * On success, *excluded_out receives a heap-allocated array of strdup'd
 * relative directory paths and *excluded_count_out its length; the caller
 * owns it and must free via cbm_discover_free_excluded(). Pass NULL for
 * excluded_out (and/or excluded_count_out) to discard the list — the internal
 * accumulator is freed in that case (no leak).
 * Returns 0 on success, -1 on error. */
int cbm_discover_ex(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out);

/* One deliberately-not-indexed file (#963): an individual file dropped by an
 * ignore mechanism during the walk (its parent directory was NOT excluded —
 * whole subtrees are reported separately as excluded dirs). BY DESIGN, not a
 * failure. */
typedef struct {
    char *rel_path; /* heap-allocated, relative to repo root */
    char *reason;   /* heap-allocated: "gitignore" | "cbmignore" |
                     * "skip-list" | "ignored-suffix" | "fast-pattern" |
                     * "size-cap" */
} cbm_ignored_file_t;

typedef struct cbm_discovery_snapshot cbm_discovery_snapshot_t;

/* Stored per-file ignore entries are capped (the walk still counts ALL of
 * them in *ignored_total_out, so truncation is always explicit, never
 * silent). Whole excluded subtrees stay exhaustive via excluded_out. */
enum { CBM_DISCOVER_IGNORED_CAP = 2000 };

/* Like cbm_discover_ex(), but additionally reports the individual files that
 * ignore rules dropped (#963 "purposely not indexed"). *ignored_out receives
 * a heap array (caller frees via cbm_discover_free_ignored),
 * *ignored_count_out its stored length (<= CBM_DISCOVER_IGNORED_CAP), and
 * *ignored_total_out the TOTAL number of ignored files seen. Pass NULL to
 * skip the collection entirely. */
int cbm_discover_ex2(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     cbm_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out);

/* Generation-aware discovery. Requested-mode outputs match ex2, while the
 * snapshot always fingerprints a mode-independent FULL selection: selected
 * path/language/content-dependent classification, excluded coverage, ignored
 * path/reason/total, and every loaded ignore-source digest. */
int cbm_discover_ex3(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     cbm_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out, cbm_discovery_snapshot_t **snapshot_out);

const char *cbm_discovery_snapshot_fingerprint(const cbm_discovery_snapshot_t *snapshot);
/* Rerun FULL discovery and compare its exact selection/coverage generation. */
bool cbm_discovery_snapshot_verify(const cbm_discovery_snapshot_t *snapshot);
void cbm_discovery_snapshot_free(cbm_discovery_snapshot_t *snapshot);

/* Free an array of file info results. NULL-safe. */
void cbm_discover_free(cbm_file_info_t *files, int count);

/* Free the excluded-directory list returned by cbm_discover_ex(). NULL-safe. */
void cbm_discover_free_excluded(char **excluded, int count);

/* Free the ignored-file list returned by cbm_discover_ex2(). NULL-safe. */
void cbm_discover_free_ignored(cbm_ignored_file_t *ignored, int count);

#endif /* CBM_DISCOVER_H */
