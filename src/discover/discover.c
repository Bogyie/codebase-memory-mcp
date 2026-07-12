/*
 * discover.c — Recursive directory walk with filtering.
 *
 * Walks a repository directory tree, applying:
 *   1. Hardcoded directory skip patterns (60+ dirs like .git, node_modules)
 *   2. Hardcoded suffix filters (.pyc, .png, .wasm, etc.)
 *   3. Fast-mode additional filters (docs, examples, lock files, etc.)
 *   4. Gitignore-style pattern matching
 *   5. Language detection for accepted files
 */
#include "discover/discover.h"
#include "cbm.h" // CBMLanguage, CBM_LANG_COUNT, CBM_LANG_JSON

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif
#include <ctype.h>
#include <limits.h>
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strdup
#include <sys/stat.h>

int cbm_gitignore_match_result(const cbm_gitignore_t *gi, const char *rel_path, bool is_dir);

/* ── Hardcoded always-skip directories ──────────────────────────── */

static const char *ALWAYS_SKIP_DIRS[] = {
    /* VCS */
    ".git", ".hg", ".svn", ".worktrees",
    /* IDE */
    ".idea", ".vs", ".vscode", ".eclipse", ".claude", ".claude-worktrees", "Antigravity",
    /* Python */
    ".cache", ".eggs", ".env", ".mypy_cache", ".nox", ".pytest_cache", ".ruff_cache", ".tox",
    ".venv", "__pycache__", "env", "htmlcov", "site-packages", "venv",
    /* JS/TS */
    ".npm", ".nyc_output", ".pnpm-store", ".yarn", "bower_components", "coverage", "node_modules",
    ".next", ".nuxt", ".svelte-kit", ".angular", ".turbo", ".parcel-cache", ".docusaurus", ".expo",
    /* Build artifacts */
    "dist", "obj", "Pods", "target", "temp", "tmp", ".terraform", ".serverless", "bazel-bin",
    "bazel-out", "bazel-testlogs",
    /* Language caches */
    ".cargo", ".stack-work", ".dart_tool", "zig-cache", "zig-out", ".metals", ".bloop", ".bsp",
    ".ccls-cache", ".clangd", "elm-stuff", "_opam", ".cpcache", ".shadow-cljs",
    /* Deploy */
    ".vercel", ".netlify", "deploy", "deployed",
    /* Misc */
    ".qdrant_code_embeddings", ".tmp", "vendor", "vendored", NULL};

static const char *FAST_SKIP_DIRS[] = {
    "generated", "gen",           "auto-generated", "fixtures",     "testdata",    "test_data",
    "__tests__", "__mocks__",     "__snapshots__",  "__fixtures__", "__test__",    "docs",
    "doc",       "documentation", "examples",       "example",      "samples",     "sample",
    "assets",    "static",        "public",         "media",        "third_party", "thirdparty",
    "3rdparty",  "external",      "migrations",     "seeds",        "e2e",         "integration",
    "locale",    "locales",       "i18n",           "l10n",         "scripts",     "tools",
    "hack",      "bin",           "build",          "out",          NULL};

/* ── Ignored suffixes ───────────────────────────────── */

static const char *ALWAYS_IGNORED_SUFFIXES[] = {
    ".tmp",    "~",        ".pyc",  ".pyo",   ".o",   ".a",   ".so",  ".dll",
    ".class",  ".png",     ".jpg",  ".jpeg",  ".gif", ".ico", ".bmp", ".tiff",
    ".webp",   ".svg",     ".wasm", ".node",  ".exe", ".bin", ".dat", ".db",
    ".sqlite", ".sqlite3", ".woff", ".woff2", ".ttf", ".eot", ".otf", NULL};

static const char *FAST_IGNORED_SUFFIXES[] = {
    ".zip", ".tar",  ".gz",       ".bz2",  ".xz",  ".rar",    ".7z",      ".jar",
    ".war", ".ear",  ".mp3",      ".mp4",  ".avi", ".mov",    ".wav",     ".flac",
    ".ogg", ".mkv",  ".webm",     ".pdf",  ".doc", ".docx",   ".xls",     ".xlsx",
    ".ppt", ".pptx", ".odt",      ".ods",  ".map", ".min.js", ".min.css", ".pem",
    ".crt", ".key",  ".cer",      ".p12",  ".pb",  ".avro",   ".parquet", ".beam",
    ".elc", ".rlib", ".coverage", ".prof", ".out", ".patch",  ".diff",    NULL};

/* ── Fast-mode skip filenames ─────────────────────── */

static const char *FAST_SKIP_FILENAMES[] = {
    "LICENSE",        "LICENSE.txt",     "LICENSE.md",   "LICENSE-MIT",   "LICENSE-APACHE",
    "LICENCE",        "LICENCE.txt",     "LICENCE.md",   "CHANGELOG",     "CHANGELOG.md",
    "CHANGES.md",     "HISTORY",         "HISTORY.md",   "AUTHORS",       "AUTHORS.md",
    "CONTRIBUTORS",   "CONTRIBUTORS.md", "CODEOWNERS",   "go.sum",        "yarn.lock",
    "pnpm-lock.yaml", "Pipfile.lock",    "poetry.lock",  "Gemfile.lock",  "Cargo.lock",
    "mix.lock",       "flake.lock",      "pubspec.lock", "composer.lock", "package-lock.json",
    "configure",      "Makefile.in",     "config.guess", "config.sub",    NULL};

/* ── Fast-mode substring patterns ───────────────────── */

static const char *FAST_PATTERNS[] = {".d.ts",      ".bundle.", ".chunk.", ".generated.",
                                      ".pb.go",     "_pb2.py",  ".pb2.py", "_grpc.pb.go",
                                      "_string.go", "mock_",    "_mock.",  "_test_helpers.",
                                      ".stories.",  ".spec.",   ".test.",  NULL};

/* ── Ignored JSON filenames ──────────────────────── */

static const char *IGNORED_JSON_FILES[] = {
    "package.json",       "package-lock.json", "tsconfig.json",
    "jsconfig.json",      "composer.json",     "composer.lock",
    "yarn.lock",          "openapi.json",      "swagger.json",
    "jest.config.json",   ".eslintrc.json",    ".prettierrc.json",
    ".babelrc.json",      "tslint.json",       "angular.json",
    "firebase.json",      "renovate.json",     "lerna.json",
    "turbo.json",         ".stylelintrc.json", "pnpm-lock.json",
    "deno.json",          "biome.json",        "devcontainer.json",
    ".devcontainer.json", "launch.json",       "settings.json",
    "extensions.json",    "tasks.json",        NULL};

/* ── Helper: check if string is in NULL-terminated array ─────────── */

static bool str_in_list(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Helper: check if string ends with suffix ────────────── */

static bool ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) {
        return false;
    }
    return strcmp(s + slen - sufflen, suffix) == 0;
}

/* ── Helper: check if string contains substring ───────────── */

static bool str_contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

/* ── Git global excludes resolution ───────────────────────────── */

enum { GIT_TILDE_PREFIX_LEN = 2 }; /* "~/". */

static bool ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void strip_inline_comment(char *s) {
    bool in_quote = false;
    char quote = '\0';
    for (char *p = s; *p; p++) {
        if ((*p == '"' || *p == '\'') && (p == s || p[-1] != '\\')) {
            if (!in_quote) {
                in_quote = true;
                quote = *p;
            } else if (*p == quote) {
                in_quote = false;
            }
            continue;
        }
        if (!in_quote && (*p == '#' || *p == ';') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static char *strip_matching_quotes(char *s) {
    size_t len = strlen(s);
    if (len >= CBM_QUOTE_PAIR && ((s[0] == '"' && s[len - SKIP_ONE] == '"') ||
                                  (s[0] == '\'' && s[len - SKIP_ONE] == '\''))) {
        s[len - SKIP_ONE] = '\0';
        return s + SKIP_ONE;
    }
    return s;
}

static bool has_trailing_sep(const char *path) {
    size_t len = strlen(path);
    return len > 0 && (path[len - SKIP_ONE] == '/' || path[len - SKIP_ONE] == '\\');
}

static bool copy_path_checked(char *out, size_t out_sz, const char *value) {
    if (!out || out_sz == 0 || !value) {
        return false;
    }
    size_t len = strlen(value);
    if (len >= out_sz) {
        return false;
    }
    memcpy(out, value, len + SKIP_ONE);
    cbm_normalize_path_sep(out);
    return true;
}

static bool path_join(char *out, size_t out_sz, const char *base, const char *rel) {
    if (!out || out_sz == 0) {
        return false;
    }
    const char *safe_base = base ? base : "";
    const char *safe_rel = rel ? rel : "";
    size_t base_len = strlen(safe_base);
    size_t rel_len = strlen(safe_rel);
    bool separator = base_len > 0 && rel_len > 0 && !has_trailing_sep(safe_base);
    size_t separator_len = separator ? SKIP_ONE : 0;
    if (base_len > SIZE_MAX - separator_len || base_len + separator_len > SIZE_MAX - rel_len ||
        base_len + separator_len + rel_len >= out_sz) {
        return false;
    }
    memcpy(out, safe_base, base_len);
    if (separator) {
        out[base_len] = '/';
    }
    memcpy(out + base_len + separator_len, safe_rel, rel_len);
    out[base_len + separator_len + rel_len] = '\0';
    cbm_normalize_path_sep(out);
    return true;
}

static bool expand_git_path(const char *path, char *out, size_t out_sz, bool *error) {
    if (!path || !path[0] || !out || out_sz == 0) {
        return false;
    }
    char normalized[CBM_SZ_4K];
    if (!copy_path_checked(normalized, sizeof(normalized), path)) {
        *error = true;
        return false;
    }

    if (normalized[0] != '~') {
        if (!copy_path_checked(out, out_sz, normalized)) {
            *error = true;
            return false;
        }
        return out[0] != '\0';
    }

    if (normalized[1] != '\0' && normalized[1] != '/') {
        return false; /* ~user expansion is intentionally not supported. */
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    if (normalized[1] == '\0') {
        if (!copy_path_checked(out, out_sz, home)) {
            *error = true;
            return false;
        }
    } else {
        if (!path_join(out, out_sz, home, normalized + GIT_TILDE_PREFIX_LEN)) {
            *error = true;
            return false;
        }
    }
    return out[0] != '\0';
}

static bool read_core_excludes_file(const char *config_path, char *out, size_t out_sz,
                                    bool *error) {
    enum { GIT_CONFIG_MAX_BYTES = 1024 * 1024 };
    int probe = cbm_path_probe(config_path);
    if (probe == 0) {
        return false;
    }
    if (probe < 0) {
        *error = true;
        return false;
    }
    char *content = NULL;
    size_t content_len = 0;
    char sha256[65];
    if (cbm_sha256_file_read_hex(config_path, GIT_CONFIG_MAX_BYTES, &content, &content_len,
                                 sha256) != 0 ||
        memchr(content, '\0', content_len) != NULL) {
        free(content);
        *error = true;
        return false;
    }
    bool in_core = false;
    bool found = false;
    char *line = content;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *s = trim_ws(line);
        if (s[0] == '\0' || s[0] == '#' || s[0] == ';') {
            line = next;
            continue;
        }

        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                in_core = false;
                line = next;
                continue;
            }
            *end = '\0';
            in_core = ascii_ieq(trim_ws(s + SKIP_ONE), "core");
            line = next;
            continue;
        }

        if (!in_core) {
            line = next;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            line = next;
            continue;
        }
        *eq = '\0';
        char *key = trim_ws(s);
        char *value = trim_ws(eq + SKIP_ONE);
        strip_inline_comment(value);
        value = strip_matching_quotes(trim_ws(value));

        if (ascii_ieq(key, "excludesfile") && value[0] != '\0' &&
            expand_git_path(value, out, out_sz, error)) {
            found = true;
        }
        if (*error) {
            free(content);
            return false;
        }
        line = next;
    }
    free(content);
    return found;
}

static bool resolve_xdg_git_config_dir(char *out, size_t out_sz, bool *error) {
    const char *env = getenv("XDG_CONFIG_HOME");
    if (env && env[0] != '\0') {
        if (!copy_path_checked(out, out_sz, env)) {
            *error = true;
            return false;
        }
        return true;
    }

    const char *home = cbm_get_home_dir();
    if (!home || home[0] == '\0') {
        return false;
    }
    if (!path_join(out, out_sz, home, ".config")) {
        *error = true;
        return false;
    }
    return out[0] != '\0';
}

static bool resolve_global_excludes_path(char *out, size_t out_sz, bool *error) {
    char config_path[CBM_SZ_4K];

    const char *home = cbm_get_home_dir();
    if (home && home[0] != '\0') {
        if (!path_join(config_path, sizeof(config_path), home, ".gitconfig")) {
            *error = true;
            return false;
        }
        if (read_core_excludes_file(config_path, out, out_sz, error)) {
            return true;
        }
        if (*error) {
            return false;
        }
    }

    char xdg_config[CBM_SZ_4K];
    if (resolve_xdg_git_config_dir(xdg_config, sizeof(xdg_config), error)) {
        if (!path_join(config_path, sizeof(config_path), xdg_config, "git/config")) {
            *error = true;
            return false;
        }
        if (read_core_excludes_file(config_path, out, out_sz, error)) {
            return true;
        }
        if (*error) {
            return false;
        }
        if (!path_join(out, out_sz, xdg_config, "git/ignore")) {
            *error = true;
            return false;
        }
        return out[0] != '\0';
    }

    if (*error) {
        return false;
    }

    return false;
}

/* ── Public filter functions ─────────────────────── */

bool cbm_should_skip_dir(const char *dirname, cbm_index_mode_t mode) {
    if (!dirname) {
        return false;
    }

    if (str_in_list(dirname, ALWAYS_SKIP_DIRS)) {
        return true;
    }

    /* Fast discovery applies to both MODERATE and FAST — only FULL keeps everything. */
    if (mode != CBM_MODE_FULL) {
        if (str_in_list(dirname, FAST_SKIP_DIRS)) {
            return true;
        }
    }

    return false;
}

bool cbm_has_ignored_suffix(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    for (int i = 0; ALWAYS_IGNORED_SUFFIXES[i]; i++) {
        if (ends_with(filename, ALWAYS_IGNORED_SUFFIXES[i])) {
            return true;
        }
    }

    if (mode != CBM_MODE_FULL) {
        for (int i = 0; FAST_IGNORED_SUFFIXES[i]; i++) {
            if (ends_with(filename, FAST_IGNORED_SUFFIXES[i])) {
                return true;
            }
        }
    }

    return false;
}

bool cbm_should_skip_filename(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    if (mode != CBM_MODE_FULL) {
        if (str_in_list(filename, FAST_SKIP_FILENAMES)) {
            return true;
        }
    }

    return false;
}

bool cbm_matches_fast_pattern(const char *filename, cbm_index_mode_t mode) {
    if (!filename || mode == CBM_MODE_FULL) {
        return false;
    }

    for (int i = 0; FAST_PATTERNS[i]; i++) {
        if (str_contains(filename, FAST_PATTERNS[i])) {
            return true;
        }
    }

    return false;
}

/* ── Dynamic file list ────────────────────────── */

typedef enum {
    DISCOVERY_SOURCE_ROOT_GITIGNORE,
    DISCOVERY_SOURCE_INFO_EXCLUDE,
    DISCOVERY_SOURCE_GLOBAL_EXCLUDE,
    DISCOVERY_SOURCE_CBMIGNORE,
    DISCOVERY_SOURCE_NESTED_GITIGNORE,
    DISCOVERY_SOURCE_GITLINK,
    DISCOVERY_SOURCE_COMMONDIR,
    DISCOVERY_SOURCE_GLOBAL_CONFIG,
} discovery_source_kind_t;

typedef struct {
    char *path;
    char sha256[CBM_SHA256_HEX_LEN + SKIP_ONE];
    discovery_source_kind_t kind;
} discovery_source_t;

typedef struct {
    cbm_file_info_t *files;
    size_t count;
    size_t capacity;
    /* Directories skipped during the walk (rel paths), so callers can surface
     * which subtrees were dropped (#411). strdup'd; freed by the caller via
     * cbm_discover_free_excluded or internally when not requested. */
    char **excluded;
    size_t excluded_count;
    size_t excluded_cap;
    /* Individual files dropped by ignore rules (#963 "purposely not
     * indexed"). Collected only when requested (collect_ignored); stored
     * entries are capped at CBM_DISCOVER_IGNORED_CAP while ignored_total
     * keeps counting so truncation is explicit. */
    bool collect_ignored;
    bool collect_all_ignored;
    cbm_ignored_file_t *ignored;
    size_t ignored_count;
    size_t ignored_cap;
    size_t ignored_total;
    discovery_source_t *sources;
    size_t source_count;
    size_t source_capacity;
    bool failed;
} file_list_t;

/* Test seams for deterministic allocation-failure coverage. Production leaves
 * both NULL. */
void *(*cbm_discover_realloc_hook_for_test)(void *, size_t) = NULL;
char *(*cbm_discover_strdup_hook_for_test)(const char *) = NULL;
void (*cbm_discover_after_ignore_load_hook_for_test)(void) = NULL;
void (*cbm_discover_before_ignore_verify_hook_for_test)(void) = NULL;

static void *discover_realloc(void *ptr, size_t size) {
    return cbm_discover_realloc_hook_for_test ? cbm_discover_realloc_hook_for_test(ptr, size)
                                              : realloc(ptr, size);
}

static char *discover_strdup(const char *value) {
    return cbm_discover_strdup_hook_for_test ? cbm_discover_strdup_hook_for_test(value)
                                             : strdup(value);
}

static bool grow_array(void **items, size_t *capacity, size_t needed, size_t initial,
                       size_t item_size) {
    if (needed <= *capacity) {
        return true;
    }
    size_t next = *capacity ? *capacity : initial;
    while (next < needed) {
        if (next > SIZE_MAX / PAIR_LEN) {
            return false;
        }
        next *= PAIR_LEN;
    }
    if (next > SIZE_MAX / item_size) {
        return false;
    }
    void *grown = discover_realloc(*items, next * item_size);
    if (!grown) {
        return false;
    }
    *items = grown;
    *capacity = next;
    return true;
}

static bool file_list_add_excluded(file_list_t *fl, const char *rel_path) {
    if (fl->failed) {
        return false;
    }
    if (!rel_path || rel_path[0] == '\0') {
        return true;
    }
    char *copy = discover_strdup(rel_path);
    if (!copy) {
        fl->failed = true;
        return false;
    }
    if (!grow_array((void **)&fl->excluded, &fl->excluded_cap, fl->excluded_count + SKIP_ONE,
                    CBM_SZ_64, sizeof(char *))) {
        free(copy);
        fl->failed = true;
        return false;
    }
    fl->excluded[fl->excluded_count++] = copy;
    return true;
}

static bool file_list_add_ignored(file_list_t *fl, const char *rel_path, const char *reason) {
    if (fl->failed) {
        return false;
    }
    if (!fl->collect_ignored || !rel_path || rel_path[0] == '\0') {
        return true;
    }
    if (fl->ignored_total == SIZE_MAX) {
        fl->failed = true;
        return false;
    }
    fl->ignored_total++;
    if (!fl->collect_all_ignored && fl->ignored_count >= CBM_DISCOVER_IGNORED_CAP) {
        return true; /* counted above — truncation stays visible via the total */
    }
    char *path_copy = discover_strdup(rel_path);
    char *reason_copy = discover_strdup(reason ? reason : "");
    if (!path_copy || !reason_copy) {
        free(path_copy);
        free(reason_copy);
        fl->failed = true;
        return false;
    }
    if (!grow_array((void **)&fl->ignored, &fl->ignored_cap, fl->ignored_count + SKIP_ONE,
                    CBM_SZ_64, sizeof(cbm_ignored_file_t))) {
        free(path_copy);
        free(reason_copy);
        fl->failed = true;
        return false;
    }
    fl->ignored[fl->ignored_count].rel_path = path_copy;
    fl->ignored[fl->ignored_count].reason = reason_copy;
    fl->ignored_count++;
    return true;
}

static bool fl_add(file_list_t *fl, const char *abs_path, const char *rel_path, CBMLanguage lang,
                   int64_t size, const char *selection_sha256) {
    if (fl->failed) {
        return false;
    }
    char *path_copy = discover_strdup(abs_path);
    char *rel_copy = discover_strdup(rel_path);
    if (!path_copy || !rel_copy) {
        free(path_copy);
        free(rel_copy);
        fl->failed = true;
        return false;
    }
    if (!grow_array((void **)&fl->files, &fl->capacity, fl->count + SKIP_ONE, CBM_SZ_256,
                    sizeof(cbm_file_info_t))) {
        free(path_copy);
        free(rel_copy);
        fl->failed = true;
        return false;
    }

    cbm_file_info_t *fi = &fl->files[fl->count];
    fi->path = path_copy;
    fi->rel_path = rel_copy;
    fi->language = lang;
    fi->size = size;
    fi->selection_sha256[0] = '\0';
    if (selection_sha256 && selection_sha256[0]) {
        memcpy(fi->selection_sha256, selection_sha256, sizeof(fi->selection_sha256));
    }
    fl->count++;
    return true;
}

static bool file_list_add_source(file_list_t *fl, const char *path, const char *sha256,
                                 discovery_source_kind_t kind) {
    if (fl->failed || !path || !sha256 || strlen(sha256) != CBM_SHA256_HEX_LEN) {
        fl->failed = true;
        return false;
    }
    char *copy = discover_strdup(path);
    if (!copy) {
        fl->failed = true;
        return false;
    }
    if (!grow_array((void **)&fl->sources, &fl->source_capacity, fl->source_count + SKIP_ONE,
                    CBM_SZ_16, sizeof(discovery_source_t))) {
        free(copy);
        fl->failed = true;
        return false;
    }
    discovery_source_t *source = &fl->sources[fl->source_count++];
    source->path = copy;
    memcpy(source->sha256, sha256, sizeof(source->sha256));
    source->kind = kind;
    return true;
}

/* ── Recursive walk ─────────────────────────────── */

typedef struct ignore_scope {
    cbm_gitignore_t *matcher;
    char *prefix;
    struct ignore_scope *parent;
    struct ignore_scope *next_owned;
} ignore_scope_t;

/* Compute path relative to a nested .gitignore's directory.
 * "webapp/src/foo.js" with prefix "webapp" → "src/foo.js". */
static const char *local_rel_path(const char *rel_path, const char *local_prefix) {
    if (!local_prefix || local_prefix[0] == '\0') {
        return rel_path;
    }
    size_t prefix_len = strlen(local_prefix);
    size_t rel_len = strlen(rel_path);
    if (rel_len > prefix_len && strncmp(rel_path, local_prefix, prefix_len) == 0 &&
        rel_path[prefix_len] == '/') {
        return rel_path + prefix_len + SKIP_ONE;
    }
    return rel_path;
}

static bool matcher_result(const cbm_gitignore_t *matcher, const char *path, bool is_dir,
                           int *result, file_list_t *out) {
    if (!cbm_gitignore_match_result_ex(matcher, path, is_dir, result)) {
        out->failed = true;
        return false;
    }
    return true;
}

/* Git precedence: the nearest per-directory .gitignore wins, then its
 * ancestors, then the root .gitignore, info/exclude, and finally the global
 * excludes file. This helper covers every source except global, which remains
 * separate so the established .cbmignore-negates-global contract is kept. */
static bool repository_ignore_result(const ignore_scope_t *scope,
                                     const cbm_gitignore_t *root_gitignore,
                                     const cbm_gitignore_t *info_exclude, const char *rel_path,
                                     bool is_dir, int *result, file_list_t *out) {
    for (const ignore_scope_t *at = scope; at; at = at->parent) {
        int local = 0;
        if (!matcher_result(at->matcher, local_rel_path(rel_path, at->prefix), is_dir, &local,
                            out)) {
            return false;
        }
        if (local != 0) {
            *result = local;
            return true;
        }
    }
    if (!matcher_result(root_gitignore, rel_path, is_dir, result, out)) {
        return false;
    }
    if (*result != 0) {
        return true;
    }
    return matcher_result(info_exclude, rel_path, is_dir, result, out);
}

/* Non-negatable safety core: built-in skip dirs that a .cbmignore negation
 * can NEVER un-skip. A repo-committed .cbmignore must not be able to defeat
 * OOM/safety skips: .git holds VCS internals (and the info/exclude sources,
 * #489), node_modules explodes discovery, and the worktree-internal dirs
 * (.worktrees / .claude-worktrees, the worktree entries in ALWAYS_SKIP_DIRS)
 * contain parallel checkouts of the same repo whose indexing would duplicate
 * the whole codebase (#802). */
static bool is_safety_core_dir(const char *name) {
    static const char *const SAFETY_CORE_DIRS[] = {".git", "node_modules", ".worktrees",
                                                   ".claude-worktrees", NULL};
    return str_in_list(name, SAFETY_CORE_DIRS);
}

/* Check if a directory entry should be skipped (hardcoded dirs + gitignore). */
static bool should_skip_directory(const char *entry_name, const char *rel_path,
                                  const cbm_discover_opts_t *opts,
                                  const cbm_gitignore_t *root_gitignore,
                                  const cbm_gitignore_t *info_exclude,
                                  const cbm_gitignore_t *global_gi,
                                  const cbm_gitignore_t *cbmignore, const ignore_scope_t *scope,
                                  bool *skip, file_list_t *out) {
    *skip = false;
    if (cbm_should_skip_dir(entry_name, opts ? opts->mode : CBM_MODE_FULL)) {
        /* #500: a .cbmignore negation (e.g. "!obj/") whose rule is the last
         * match for this dir un-skips a built-in skip-list dir — except the
         * non-negatable safety core. Fall through so .gitignore/global/local
         * rules still apply to the un-skipped dir. */
        int cbm_result = 0;
        if (!matcher_result(cbmignore, rel_path, true, &cbm_result, out)) {
            return false;
        }
        bool unskipped = cbmignore && !is_safety_core_dir(entry_name) && cbm_result < 0;
        if (!unskipped) {
            *skip = true;
            return true;
        }
    }
    int repo_result = 0;
    if (!repository_ignore_result(scope, root_gitignore, info_exclude, rel_path, true, &repo_result,
                                  out)) {
        return false;
    }
    if (repo_result > 0) {
        *skip = true;
        return true;
    }
    int global_result = 0;
    if (repo_result == 0 && !matcher_result(global_gi, rel_path, true, &global_result, out)) {
        return false;
    }
    int cbm_result = 0;
    if (!matcher_result(cbmignore, rel_path, true, &cbm_result, out)) {
        return false;
    }
    if (cbm_result > 0) {
        *skip = true;
    } else if (cbm_result < 0) {
        *skip = false;
    } else {
        *skip = global_result > 0;
    }
    return true;
}

/* Check if a regular file should be skipped (filters + gitignore + size). */
/* Classify why a file is skipped. Returns a static reason string (the #963
 * "purposely not indexed" class) or NULL to keep the file. Check order and
 * semantics — including the .cbmignore negation un-ignoring a global-gitignore
 * match — are IDENTICAL to the original boolean predicate. */
static bool file_skip_reason(const char *entry_name, const char *rel_path,
                             const cbm_discover_opts_t *opts, const cbm_gitignore_t *root_gitignore,
                             const cbm_gitignore_t *info_exclude, const cbm_gitignore_t *global_gi,
                             const cbm_gitignore_t *cbmignore, const ignore_scope_t *scope,
                             off_t file_size, const char **reason, file_list_t *out) {
    *reason = NULL;
    cbm_index_mode_t mode = opts ? opts->mode : CBM_MODE_FULL;
    if (cbm_has_ignored_suffix(entry_name, mode)) {
        *reason = "ignored-suffix";
        return true;
    }
    if (cbm_should_skip_filename(entry_name, mode)) {
        *reason = "skip-list";
        return true;
    }
    if (cbm_matches_fast_pattern(entry_name, mode)) {
        *reason = "fast-pattern";
        return true;
    }
    int repo_result = 0;
    if (!repository_ignore_result(scope, root_gitignore, info_exclude, rel_path, false,
                                  &repo_result, out)) {
        return false;
    }
    if (repo_result > 0) {
        *reason = "gitignore";
        return true;
    }
    int global_result = 0;
    if (repo_result == 0 && !matcher_result(global_gi, rel_path, false, &global_result, out)) {
        return false;
    }
    int cbm_result = 0;
    if (!matcher_result(cbmignore, rel_path, false, &cbm_result, out)) {
        return false;
    }
    if (cbm_result > 0) {
        *reason = "cbmignore";
        return true;
    }
    if (opts && opts->max_file_size > 0 && file_size > opts->max_file_size) {
        *reason = "size-cap";
        return true;
    }
    if (cbm_result == 0 && global_result > 0) {
        *reason = "gitignore";
    }
    return true;
}

/* Detect language for a file, handling .m disambiguation and JSON filtering. */
static CBMLanguage detect_file_language(const char *entry_name, const char *abs_path, off_t size,
                                        char selection_sha256[65], file_list_t *out) {
    selection_sha256[0] = '\0';
    CBMLanguage lang = cbm_language_for_filename(entry_name);
    if (lang == CBM_LANG_COUNT) {
        return CBM_LANG_COUNT;
    }
    /* Special: .m files need content-based disambiguation */
    const char *dot = strrchr(entry_name, '.');
    if (dot && strcmp(dot, ".m") == 0) {
        if (size < 0 || (uintmax_t)size > SIZE_MAX - SKIP_ONE) {
            out->failed = true;
            return CBM_LANG_COUNT;
        }
        char *source = NULL;
        size_t source_len = 0;
        size_t max_bytes = (size_t)size + SKIP_ONE;
        int read_result =
            cbm_sha256_file_read_hex(abs_path, max_bytes, &source, &source_len, selection_sha256);
        if (read_result != CBM_SHA256_FILE_HASHED || source_len != (size_t)size) {
            free(source);
            out->failed = true;
            return CBM_LANG_COUNT;
        }
        lang = cbm_disambiguate_m_content(source, source_len);
        free(source);
    }
    /* Check ignored JSON files */
    if (lang == CBM_LANG_JSON && str_in_list(entry_name, IGNORED_JSON_FILES)) {
        return CBM_LANG_COUNT;
    }
    return lang;
}

/* UTF-8-safe stat: wide API on Windows, regular stat on POSIX. */
static int wide_stat(const char *path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    struct _stat64 wst;
    int ret = _wstat64(wpath, &wst);
    free(wpath);
    if (ret != 0) {
        return CBM_NOT_FOUND;
    }
    st->st_mode = wst.st_mode;
    st->st_size = wst.st_size;
    st->st_mtime = wst.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

/* Stat a path, skipping symlinks (POSIX) and junctions / reparse points
 * (Windows). Returns 0 on success, -1 to skip. Skipping reparse points keeps
 * discovery from walking through a junction that points outside the project
 * root, mirroring the POSIX S_ISLNK skip. */
enum { SAFE_STAT_ERROR = -1, SAFE_STAT_OK = 0, SAFE_STAT_SKIP = 1 };

static int safe_stat(const char *abs_path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(abs_path);
    if (!wpath) {
        return SAFE_STAT_ERROR;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return SAFE_STAT_ERROR;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        return SAFE_STAT_SKIP;
    }
    return wide_stat(abs_path, st) == 0 ? SAFE_STAT_OK : SAFE_STAT_ERROR;
#else
    if (lstat(abs_path, st) != 0) {
        return SAFE_STAT_ERROR;
    }
    if (S_ISLNK(st->st_mode)) {
        return SAFE_STAT_SKIP;
    }
    return SAFE_STAT_OK;
#endif
}

/* Process a single regular file entry during directory walk. */
static void walk_dir_process_file(const char *abs_path, const char *rel_path, const char *name,
                                  const cbm_discover_opts_t *opts,
                                  const cbm_gitignore_t *root_gitignore,
                                  const cbm_gitignore_t *info_exclude,
                                  const cbm_gitignore_t *global_gi,
                                  const cbm_gitignore_t *cbmignore, const ignore_scope_t *scope,
                                  off_t size, file_list_t *out) {
    const char *skip_reason = NULL;
    if (!file_skip_reason(name, rel_path, opts, root_gitignore, info_exclude, global_gi, cbmignore,
                          scope, size, &skip_reason, out)) {
        return;
    }
    if (skip_reason) {
        /* Deliberately not indexed (#963) — record so callers can surface it.
         * Unsupported-language files below are NOT recorded: "no grammar for
         * this extension" is not an ignore decision, and listing every
         * README/asset would drown the signal. */
        (void)file_list_add_ignored(out, rel_path, skip_reason);
        return;
    }
    char selection_sha256[65];
    CBMLanguage lang = detect_file_language(name, abs_path, size, selection_sha256, out);
    if (out->failed) {
        return;
    }
    if (lang == CBM_LANG_COUNT) {
        return;
    }
    (void)fl_add(out, abs_path, rel_path, lang, size, selection_sha256);
}

typedef struct {
    char *dir;
    char *prefix;
    ignore_scope_t *scope;
} walk_frame_t;

typedef struct {
    walk_frame_t *items;
    size_t count;
    size_t capacity;
} walk_stack_t;

static bool join_path_alloc(const char *base, const char *name, char **out) {
    *out = NULL;
    if (!base || !name) {
        return false;
    }
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    bool add_separator =
        base_len > 0 && base[base_len - SKIP_ONE] != '/' && base[base_len - SKIP_ONE] != '\\';
    size_t separator = add_separator ? SKIP_ONE : 0;
    if (base_len > SIZE_MAX - separator || base_len + separator > SIZE_MAX - name_len - SKIP_ONE) {
        return false;
    }
    size_t total = base_len + separator + name_len;
    char *joined = malloc(total + SKIP_ONE);
    if (!joined) {
        return false;
    }
    memcpy(joined, base, base_len);
    if (add_separator) {
        joined[base_len] = '/';
    }
    memcpy(joined + base_len + separator, name, name_len);
    joined[total] = '\0';
    cbm_normalize_path_sep(joined);
    *out = joined;
    return true;
}

static bool walk_stack_push(walk_stack_t *stack, char *dir, char *prefix, ignore_scope_t *scope) {
    if (!grow_array((void **)&stack->items, &stack->capacity, stack->count + SKIP_ONE, CBM_SZ_64,
                    sizeof(walk_frame_t))) {
        return false;
    }
    stack->items[stack->count++] = (walk_frame_t){.dir = dir, .prefix = prefix, .scope = scope};
    return true;
}

static void free_scope_list(ignore_scope_t *scope) {
    while (scope) {
        ignore_scope_t *next = scope->next_owned;
        cbm_gitignore_free(scope->matcher);
        free(scope->prefix);
        free(scope);
        scope = next;
    }
}

static bool load_nested_scope(walk_frame_t *frame, ignore_scope_t **owned, file_list_t *out) {
    if (frame->prefix[0] == '\0') {
        return true; /* the root matcher was loaded before the walk */
    }
    char *path = NULL;
    if (!join_path_alloc(frame->dir, ".gitignore", &path)) {
        out->failed = true;
        return false;
    }
    cbm_gitignore_t *matcher = NULL;
    char sha256[65];
    cbm_gitignore_load_result_t load = cbm_gitignore_load_hashed(path, &matcher, sha256);
    if (load == CBM_GITIGNORE_LOAD_MISSING) {
        free(path);
        return true;
    }
    if (load != CBM_GITIGNORE_LOAD_OK) {
        free(path);
        out->failed = true;
        return false;
    }
    if (!file_list_add_source(out, path, sha256, DISCOVERY_SOURCE_NESTED_GITIGNORE)) {
        free(path);
        cbm_gitignore_free(matcher);
        return false;
    }
    free(path);
    ignore_scope_t *scope = calloc(CBM_ALLOC_ONE, sizeof(ignore_scope_t));
    char *prefix = discover_strdup(frame->prefix);
    if (!scope || !prefix) {
        free(scope);
        free(prefix);
        cbm_gitignore_free(matcher);
        out->failed = true;
        return false;
    }
    scope->matcher = matcher;
    scope->prefix = prefix;
    scope->parent = frame->scope;
    scope->next_owned = *owned;
    *owned = scope;
    frame->scope = scope;
    return true;
}

static void walk_dir_process_entry(cbm_dirent_t *entry, const walk_frame_t *frame,
                                   const cbm_discover_opts_t *opts,
                                   const cbm_gitignore_t *root_gitignore,
                                   const cbm_gitignore_t *info_exclude,
                                   const cbm_gitignore_t *global_gi,
                                   const cbm_gitignore_t *cbmignore, walk_stack_t *stack,
                                   file_list_t *out) {
    if (entry->is_reparse || out->failed) {
        return;
    }
    char *abs_path = NULL;
    char *rel_path = NULL;
    if (!join_path_alloc(frame->dir, entry->name, &abs_path) ||
        !join_path_alloc(frame->prefix, entry->name, &rel_path)) {
        free(abs_path);
        free(rel_path);
        out->failed = true;
        return;
    }

    struct stat st;
    int stat_result = safe_stat(abs_path, &st);
    if (stat_result != SAFE_STAT_OK) {
        free(abs_path);
        free(rel_path);
        if (stat_result == SAFE_STAT_ERROR) {
            out->failed = true;
        }
        return; /* symlink/reparse point policy: skip, never traverse */
    }

    if (S_ISDIR(st.st_mode)) {
        bool skip = false;
        if (!should_skip_directory(entry->name, rel_path, opts, root_gitignore, info_exclude,
                                   global_gi, cbmignore, frame->scope, &skip, out)) {
            free(abs_path);
            free(rel_path);
            return;
        }
        if (skip) {
            (void)file_list_add_excluded(out, rel_path);
            free(abs_path);
            free(rel_path);
        } else if (!walk_stack_push(stack, abs_path, rel_path, frame->scope)) {
            free(abs_path);
            free(rel_path);
            out->failed = true;
        }
    } else {
        if (S_ISREG(st.st_mode)) {
            walk_dir_process_file(abs_path, rel_path, entry->name, opts, root_gitignore,
                                  info_exclude, global_gi, cbmignore, frame->scope, st.st_size,
                                  out);
        }
        free(abs_path);
        free(rel_path);
    }
}

static bool walk_dir(const char *dir_path, const char *rel_prefix, const cbm_discover_opts_t *opts,
                     const cbm_gitignore_t *root_gitignore, const cbm_gitignore_t *info_exclude,
                     const cbm_gitignore_t *global_gi, const cbm_gitignore_t *cbmignore,
                     file_list_t *out) {
    walk_stack_t stack = {0};
    ignore_scope_t *owned_scopes = NULL;
    char *root_dir = discover_strdup(dir_path);
    char *root_prefix = discover_strdup(rel_prefix);
    if (!root_dir || !root_prefix || !walk_stack_push(&stack, root_dir, root_prefix, NULL)) {
        free(root_dir);
        free(root_prefix);
        free(stack.items);
        out->failed = true;
        return false;
    }

    while (stack.count > 0 && !out->failed) {
        walk_frame_t frame = stack.items[--stack.count];
        if (!load_nested_scope(&frame, &owned_scopes, out)) {
            free(frame.dir);
            free(frame.prefix);
            break;
        }

        cbm_dir_t *d = cbm_opendir(frame.dir);
        if (!d) {
            out->failed = true;
            free(frame.dir);
            free(frame.prefix);
            break;
        }
        cbm_dirent_t *entry;
        while (!out->failed && (entry = cbm_readdir(d)) != NULL) {
            walk_dir_process_entry(entry, &frame, opts, root_gitignore, info_exclude, global_gi,
                                   cbmignore, &stack, out);
        }
        if (cbm_dir_had_error(d)) {
            out->failed = true;
        }
        cbm_closedir(d);
        free(frame.dir);
        free(frame.prefix);
    }

    while (stack.count > 0) {
        free(stack.items[stack.count - SKIP_ONE].dir);
        free(stack.items[stack.count - SKIP_ONE].prefix);
        stack.count--;
    }
    free(stack.items);
    free_scope_list(owned_scopes);
    return !out->failed;
}

struct cbm_discovery_snapshot {
    char *repo_path;
    cbm_discover_opts_t opts;
    char *ignore_file;
    char fingerprint[CBM_SHA256_HEX_LEN + SKIP_ONE];
};

static void discovery_hash_u64(cbm_sha256_ctx *hash, uint64_t value) {
    unsigned char encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++) {
        encoded[sizeof(encoded) - i - SKIP_ONE] = (unsigned char)(value & 0xffU);
        value >>= 8U;
    }
    cbm_sha256_update(hash, encoded, sizeof(encoded));
}

static void discovery_hash_bytes(cbm_sha256_ctx *hash, const void *data, size_t len) {
    discovery_hash_u64(hash, (uint64_t)len);
    if (len > 0) {
        cbm_sha256_update(hash, data, len);
    }
}

static int compare_file_ptrs(const void *left, const void *right) {
    const cbm_file_info_t *a = *(const cbm_file_info_t *const *)left;
    const cbm_file_info_t *b = *(const cbm_file_info_t *const *)right;
    return strcmp(a->rel_path, b->rel_path);
}

static int compare_string_ptrs(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static int compare_ignored_ptrs(const void *left, const void *right) {
    const cbm_ignored_file_t *a = *(const cbm_ignored_file_t *const *)left;
    const cbm_ignored_file_t *b = *(const cbm_ignored_file_t *const *)right;
    int path_order = strcmp(a->rel_path, b->rel_path);
    return path_order != 0 ? path_order : strcmp(a->reason, b->reason);
}

static int compare_source_ptrs(const void *left, const void *right) {
    const discovery_source_t *a = *(const discovery_source_t *const *)left;
    const discovery_source_t *b = *(const discovery_source_t *const *)right;
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    int path_order = strcmp(a->path, b->path);
    return path_order != 0 ? path_order : strcmp(a->sha256, b->sha256);
}

static bool discovery_sources_still_selected(const file_list_t *fl) {
    enum { DISCOVERY_IGNORE_MAX_BYTES = 4 * 1024 * 1024 };
    for (size_t i = 0; i < fl->source_count; i++) {
        char current[CBM_SHA256_HEX_LEN + SKIP_ONE];
        if (cbm_sha256_file_hex_limited(fl->sources[i].path, DISCOVERY_IGNORE_MAX_BYTES, current) !=
                0 ||
            strcmp(current, fl->sources[i].sha256) != 0) {
            return false;
        }
    }
    return true;
}

static bool discovery_compute_fingerprint(const file_list_t *fl, char out[65]) {
    cbm_file_info_t **files = NULL;
    char **excluded = NULL;
    cbm_ignored_file_t **ignored = NULL;
    discovery_source_t **sources = NULL;
    if ((fl->count > 0 && fl->count > SIZE_MAX / sizeof(*files)) ||
        (fl->excluded_count > 0 && fl->excluded_count > SIZE_MAX / sizeof(*excluded)) ||
        (fl->ignored_count > 0 && fl->ignored_count > SIZE_MAX / sizeof(*ignored)) ||
        (fl->source_count > 0 && fl->source_count > SIZE_MAX / sizeof(*sources))) {
        return false;
    }
    if (fl->count > 0) {
        files = malloc(fl->count * sizeof(*files));
    }
    if (fl->excluded_count > 0) {
        excluded = malloc(fl->excluded_count * sizeof(*excluded));
    }
    if (fl->ignored_count > 0) {
        ignored = malloc(fl->ignored_count * sizeof(*ignored));
    }
    if (fl->source_count > 0) {
        sources = malloc(fl->source_count * sizeof(*sources));
    }
    if ((fl->count > 0 && !files) || (fl->excluded_count > 0 && !excluded) ||
        (fl->ignored_count > 0 && !ignored) || (fl->source_count > 0 && !sources)) {
        free(files);
        free(excluded);
        free(ignored);
        free(sources);
        return false;
    }
    for (size_t i = 0; i < fl->count; i++) {
        files[i] = &fl->files[i];
    }
    for (size_t i = 0; i < fl->excluded_count; i++) {
        excluded[i] = fl->excluded[i];
    }
    for (size_t i = 0; i < fl->ignored_count; i++) {
        ignored[i] = &fl->ignored[i];
    }
    for (size_t i = 0; i < fl->source_count; i++) {
        sources[i] = &fl->sources[i];
    }
    if (fl->count > 1) {
        qsort(files, fl->count, sizeof(*files), compare_file_ptrs);
    }
    if (fl->excluded_count > 1) {
        qsort(excluded, fl->excluded_count, sizeof(*excluded), compare_string_ptrs);
    }
    if (fl->ignored_count > 1) {
        qsort(ignored, fl->ignored_count, sizeof(*ignored), compare_ignored_ptrs);
    }
    if (fl->source_count > 1) {
        qsort(sources, fl->source_count, sizeof(*sources), compare_source_ptrs);
    }

    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    static const char version[] = "cbm-discovery-full-v1";
    discovery_hash_bytes(&hash, version, sizeof(version) - SKIP_ONE);
    discovery_hash_u64(&hash, fl->count);
    for (size_t i = 0; i < fl->count; i++) {
        discovery_hash_bytes(&hash, files[i]->rel_path, strlen(files[i]->rel_path));
        discovery_hash_u64(&hash, (uint64_t)files[i]->language);
        discovery_hash_u64(&hash, (uint64_t)files[i]->size);
        discovery_hash_bytes(&hash, files[i]->selection_sha256, strlen(files[i]->selection_sha256));
    }
    discovery_hash_u64(&hash, fl->excluded_count);
    for (size_t i = 0; i < fl->excluded_count; i++) {
        discovery_hash_bytes(&hash, excluded[i], strlen(excluded[i]));
    }
    discovery_hash_u64(&hash, fl->ignored_count);
    discovery_hash_u64(&hash, fl->ignored_total);
    for (size_t i = 0; i < fl->ignored_count; i++) {
        discovery_hash_bytes(&hash, ignored[i]->rel_path, strlen(ignored[i]->rel_path));
        discovery_hash_bytes(&hash, ignored[i]->reason, strlen(ignored[i]->reason));
    }
    discovery_hash_u64(&hash, fl->source_count);
    for (size_t i = 0; i < fl->source_count; i++) {
        discovery_hash_u64(&hash, (uint64_t)sources[i]->kind);
        discovery_hash_bytes(&hash, sources[i]->path, strlen(sources[i]->path));
        discovery_hash_bytes(&hash, sources[i]->sha256, strlen(sources[i]->sha256));
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * PAIR_LEN] = hex[digest[i] >> 4U];
        out[i * PAIR_LEN + SKIP_ONE] = hex[digest[i] & 0x0fU];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
    free(files);
    free(excluded);
    free(ignored);
    free(sources);
    return true;
}

static void discovery_free_sources(file_list_t *fl) {
    for (size_t i = 0; i < fl->source_count; i++) {
        free(fl->sources[i].path);
    }
    free(fl->sources);
    fl->sources = NULL;
    fl->source_count = 0;
}

static void discovery_discard_results(file_list_t *fl) {
    for (size_t i = 0; i < fl->count; i++) {
        free(fl->files[i].path);
        free(fl->files[i].rel_path);
    }
    for (size_t i = 0; i < fl->excluded_count; i++) {
        free(fl->excluded[i]);
    }
    for (size_t i = 0; i < fl->ignored_count; i++) {
        free(fl->ignored[i].rel_path);
        free(fl->ignored[i].reason);
    }
    free(fl->files);
    free(fl->excluded);
    free(fl->ignored);
    fl->files = NULL;
    fl->excluded = NULL;
    fl->ignored = NULL;
    discovery_free_sources(fl);
}

static cbm_discovery_snapshot_t *discovery_snapshot_create(const char *repo_path,
                                                           const cbm_discover_opts_t *opts,
                                                           const char *fingerprint) {
    cbm_discovery_snapshot_t *snapshot = calloc(CBM_ALLOC_ONE, sizeof(*snapshot));
    if (!snapshot) {
        return NULL;
    }
    snapshot->repo_path = strdup(repo_path);
    snapshot->opts = opts ? *opts : (cbm_discover_opts_t){.mode = CBM_MODE_FULL};
    if (snapshot->opts.ignore_file) {
        snapshot->ignore_file = strdup(snapshot->opts.ignore_file);
    }
    if (!snapshot->repo_path || (snapshot->opts.ignore_file && !snapshot->ignore_file)) {
        cbm_discovery_snapshot_free(snapshot);
        return NULL;
    }
    snapshot->opts.ignore_file = snapshot->ignore_file;
    memcpy(snapshot->fingerprint, fingerprint, sizeof(snapshot->fingerprint));
    return snapshot;
}

/* ── Public API ───────────────────────────────── */

static bool discover_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

/* Resolve the shared "common" git directory for repo_path.
 * Handles three layouts:
 *   1. <repo>/.git is a directory    - ordinary repo; common_dir == <repo>/.git
 *   2. <repo>/.git is a regular file - linked worktree gitlink "gitdir: <path>";
 *      the common dir is read from <git_dir>/commondir (git stores info/exclude +
 *      config there, shared across worktrees). Falls back to git_dir when no
 *      commondir file exists.
 *   3. neither - not a git repo.
 * Returns true when a git dir was resolved. Fixes the worktree case where
 * .git/info/exclude and core.excludesfile were silently dropped because the old
 * check required .git to be a directory (issue #489 only covered ordinary repos). */
static int resolve_git_common_dir(const char *repo_path, char *common_dir, size_t cd_sz,
                                  file_list_t *sources) {
    char dot_git[CBM_SZ_4K];
    if (!path_join(dot_git, sizeof(dot_git), repo_path, ".git")) {
        return CBM_NOT_FOUND;
    }
    struct stat st;
    if (wide_stat(dot_git, &st) != 0) {
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        return copy_path_checked(common_dir, cd_sz, dot_git) ? 1 : CBM_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        return CBM_NOT_FOUND;
    }

    /* Linked worktree: parse one stable, bounded regular-file generation. */
    char *gitlink = NULL;
    size_t gitlink_len = 0;
    char gitlink_sha[65];
    if (cbm_sha256_file_read_hex(dot_git, CBM_SZ_4K, &gitlink, &gitlink_len, gitlink_sha) != 0 ||
        memchr(gitlink, '\0', gitlink_len) != NULL ||
        !file_list_add_source(sources, dot_git, gitlink_sha, DISCOVERY_SOURCE_GITLINK)) {
        free(gitlink);
        return CBM_NOT_FOUND;
    }
    char git_dir[CBM_SZ_4K];
    bool got_git_dir = false;
    char *line = gitlink;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *gs = trim_ws(line);
        if (strncmp(gs, "gitdir:", 7) == 0) {
            char *val = trim_ws(gs + 7);
            if (val[0] != '\0') {
                if (discover_path_is_absolute(val)) {
                    got_git_dir = copy_path_checked(git_dir, sizeof(git_dir), val);
                } else {
                    got_git_dir = path_join(git_dir, sizeof(git_dir), repo_path, val);
                }
            }
            break;
        }
        line = next;
    }
    free(gitlink);
    if (!got_git_dir) {
        return CBM_NOT_FOUND;
    }

    /* The shared dir holding info/exclude + config is named in <git_dir>/commondir
     * (typically a relative path like "../.."). Absent in single-worktree gitdirs. */
    char commondir_path[CBM_SZ_4K];
    if (!path_join(commondir_path, sizeof(commondir_path), git_dir, "commondir")) {
        return CBM_NOT_FOUND;
    }
    int commondir_probe = cbm_path_probe(commondir_path);
    if (commondir_probe < 0) {
        return CBM_NOT_FOUND;
    }
    if (commondir_probe > 0) {
        char *commondir = NULL;
        size_t commondir_len = 0;
        char commondir_sha[65];
        if (cbm_sha256_file_read_hex(commondir_path, CBM_SZ_4K, &commondir, &commondir_len,
                                     commondir_sha) != 0 ||
            memchr(commondir, '\0', commondir_len) != NULL ||
            !file_list_add_source(sources, commondir_path, commondir_sha,
                                  DISCOVERY_SOURCE_COMMONDIR)) {
            free(commondir);
            return CBM_NOT_FOUND;
        }
        char *newline = strchr(commondir, '\n');
        if (newline) {
            *newline = '\0';
        }
        char *value = trim_ws(commondir);
        bool resolved = value[0] != '\0' && (discover_path_is_absolute(value)
                                                 ? copy_path_checked(common_dir, cd_sz, value)
                                                 : path_join(common_dir, cd_sz, git_dir, value));
        free(commondir);
        if (!resolved) {
            return CBM_NOT_FOUND;
        }
        return 1;
    }

    return copy_path_checked(common_dir, cd_sz, git_dir) ? 1 : CBM_NOT_FOUND;
}

int cbm_discover(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                 int *count) {
    return cbm_discover_ex(repo_path, opts, out, count, NULL, NULL);
}

int cbm_discover_ex(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out) {
    return cbm_discover_ex2(repo_path, opts, out, count, excluded_out, excluded_count_out, NULL,
                            NULL, NULL);
}

int cbm_discover_ex2(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     cbm_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out) {
    return cbm_discover_ex3(repo_path, opts, out, count, excluded_out, excluded_count_out,
                            ignored_out, ignored_count_out, ignored_total_out, NULL);
}

int cbm_discover_ex3(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     cbm_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out, cbm_discovery_snapshot_t **snapshot_out) {
    if (excluded_out) {
        *excluded_out = NULL;
    }
    if (excluded_count_out) {
        *excluded_count_out = 0;
    }
    if (ignored_out) {
        *ignored_out = NULL;
    }
    if (ignored_count_out) {
        *ignored_count_out = 0;
    }
    if (ignored_total_out) {
        *ignored_total_out = 0;
    }
    if (snapshot_out) {
        *snapshot_out = NULL;
    }
    if (!repo_path || !out || !count) {
        return CBM_NOT_FOUND;
    }

    *out = NULL;
    *count = 0;

    /* Verify directory exists */
    struct stat st;
    if (wide_stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return CBM_NOT_FOUND;
    }
    file_list_t fl = {0};
    fl.collect_ignored = ignored_out != NULL || snapshot_out != NULL;
    fl.collect_all_ignored = snapshot_out != NULL;

    /* Keep ignore sources separate: Git precedence is per-directory/root
     * .gitignore > info/exclude > global excludes. Merging root and info in the
     * opposite order made a lower-precedence info rule override the root. */
    cbm_gitignore_t *root_gitignore = NULL;
    cbm_gitignore_t *info_exclude = NULL;
    cbm_gitignore_t *global_gi = NULL;
    cbm_gitignore_t *cbmignore = NULL;
    char gi_path[CBM_SZ_4K];
    struct stat gi_stat;
    /* Resolve the git common dir, transparently following a worktree gitlink so the
     * .git/info/exclude and core.excludesfile sources are honoured inside linked
     * worktrees too (where .git is a file pointing at the shared dir, not a directory). */
    char git_common_dir[CBM_SZ_4K];
    int git_repo_status =
        resolve_git_common_dir(repo_path, git_common_dir, sizeof(git_common_dir), &fl);
    if (git_repo_status < 0) {
        goto load_failed;
    }
    bool is_git_repo = git_repo_status > 0;
    bool has_git_config = false;
    /* Always honour the .gitignore at the indexed-directory root, even when the
     * directory is not a git repo root (e.g. indexing a sub-package directly).
     * Fixes issue #510: a root .gitignore was silently ignored without .git/. */
    char *root_ignore_path = NULL;
    if (!join_path_alloc(repo_path, ".gitignore", &root_ignore_path)) {
        return CBM_NOT_FOUND;
    }
    char source_sha256[65];
    cbm_gitignore_load_result_t load =
        cbm_gitignore_load_hashed(root_ignore_path, &root_gitignore, source_sha256);
    if (load == CBM_GITIGNORE_LOAD_OK && !file_list_add_source(&fl, root_ignore_path, source_sha256,
                                                               DISCOVERY_SOURCE_ROOT_GITIGNORE)) {
        free(root_ignore_path);
        goto load_failed;
    }
    free(root_ignore_path);
    if (load == CBM_GITIGNORE_LOAD_ERROR) {
        goto load_failed;
    }
    if (is_git_repo) {
        if (!path_join(gi_path, sizeof(gi_path), git_common_dir, "config")) {
            goto load_failed;
        }
        has_git_config = wide_stat(gi_path, &gi_stat) == 0 && S_ISREG(gi_stat.st_mode);

        char *exclude_path = NULL;
        if (!join_path_alloc(git_common_dir, "info/exclude", &exclude_path)) {
            goto load_failed;
        }
        load = cbm_gitignore_load_hashed(exclude_path, &info_exclude, source_sha256);
        if (load == CBM_GITIGNORE_LOAD_OK && !file_list_add_source(&fl, exclude_path, source_sha256,
                                                                   DISCOVERY_SOURCE_INFO_EXCLUDE)) {
            free(exclude_path);
            goto load_failed;
        }
        free(exclude_path);
        if (load == CBM_GITIGNORE_LOAD_ERROR) {
            goto load_failed;
        }
    }

    bool global_resolution_error = false;
    if (has_git_config &&
        resolve_global_excludes_path(gi_path, sizeof(gi_path), &global_resolution_error)) {
        load = cbm_gitignore_load_hashed(gi_path, &global_gi, source_sha256);
        if (load == CBM_GITIGNORE_LOAD_OK &&
            !file_list_add_source(&fl, gi_path, source_sha256, DISCOVERY_SOURCE_GLOBAL_EXCLUDE)) {
            goto load_failed;
        }
        if (load == CBM_GITIGNORE_LOAD_ERROR) {
            goto load_failed;
        }
    }
    if (global_resolution_error) {
        goto load_failed;
    }

    /* Load cbmignore if specified or exists at repo root */
    if (opts && opts->ignore_file) {
        load = cbm_gitignore_load_hashed(opts->ignore_file, &cbmignore, source_sha256);
        if (load == CBM_GITIGNORE_LOAD_OK &&
            !file_list_add_source(&fl, opts->ignore_file, source_sha256,
                                  DISCOVERY_SOURCE_CBMIGNORE)) {
            goto load_failed;
        }
        if (load != CBM_GITIGNORE_LOAD_OK) {
            goto load_failed;
        }
    } else {
        char *cbmignore_path = NULL;
        if (!join_path_alloc(repo_path, ".cbmignore", &cbmignore_path)) {
            goto load_failed;
        }
        load = cbm_gitignore_load_hashed(cbmignore_path, &cbmignore, source_sha256);
        if (load == CBM_GITIGNORE_LOAD_OK &&
            !file_list_add_source(&fl, cbmignore_path, source_sha256, DISCOVERY_SOURCE_CBMIGNORE)) {
            free(cbmignore_path);
            goto load_failed;
        }
        free(cbmignore_path);
        if (load == CBM_GITIGNORE_LOAD_ERROR) {
            goto load_failed;
        }
    }

    /* Walk */
    if (cbm_discover_after_ignore_load_hook_for_test) {
        cbm_discover_after_ignore_load_hook_for_test();
    }
    (void)walk_dir(repo_path, "", opts, root_gitignore, info_exclude, global_gi, cbmignore, &fl);

    /* Cleanup */
    cbm_gitignore_free(root_gitignore);
    cbm_gitignore_free(info_exclude);
    cbm_gitignore_free(global_gi);
    cbm_gitignore_free(cbmignore);
    if (cbm_discover_before_ignore_verify_hook_for_test) {
        cbm_discover_before_ignore_verify_hook_for_test();
    }

    if (fl.failed || fl.count > INT_MAX || fl.excluded_count > INT_MAX ||
        fl.ignored_count > INT_MAX || fl.ignored_total > INT_MAX ||
        !discovery_sources_still_selected(&fl)) {
        discovery_discard_results(&fl);
        return CBM_NOT_FOUND;
    }

    cbm_discovery_snapshot_t *selected_snapshot = NULL;
    if (snapshot_out) {
        cbm_index_mode_t requested_mode = opts ? opts->mode : CBM_MODE_FULL;
        if (requested_mode == CBM_MODE_FULL) {
            char fingerprint[CBM_SHA256_HEX_LEN + SKIP_ONE];
            if (!discovery_compute_fingerprint(&fl, fingerprint)) {
                discovery_discard_results(&fl);
                return CBM_NOT_FOUND;
            }
            selected_snapshot = discovery_snapshot_create(repo_path, opts, fingerprint);
            if (!selected_snapshot) {
                discovery_discard_results(&fl);
                return CBM_NOT_FOUND;
            }
        } else {
            cbm_discover_opts_t full_opts = *opts;
            full_opts.mode = CBM_MODE_FULL;
            cbm_file_info_t *full_files = NULL;
            char **full_excluded = NULL;
            cbm_ignored_file_t *full_ignored = NULL;
            int full_count = 0;
            int full_excluded_count = 0;
            int full_ignored_count = 0;
            int full_ignored_total = 0;
            int full_rc =
                cbm_discover_ex3(repo_path, &full_opts, &full_files, &full_count, &full_excluded,
                                 &full_excluded_count, &full_ignored, &full_ignored_count,
                                 &full_ignored_total, &selected_snapshot);
            cbm_discover_free(full_files, full_count);
            cbm_discover_free_excluded(full_excluded, full_excluded_count);
            cbm_discover_free_ignored(full_ignored, full_ignored_count);
            if (full_rc != 0 || !selected_snapshot) {
                cbm_discovery_snapshot_free(selected_snapshot);
                discovery_discard_results(&fl);
                return CBM_NOT_FOUND;
            }
        }
    }

    /* Public reporting remains bounded, but snapshot fingerprints above saw
     * every ignored path/reason so changes beyond the reporting cap cannot
     * collide merely because ignored_total stayed constant. */
    if (fl.ignored_count > CBM_DISCOVER_IGNORED_CAP) {
        for (size_t i = CBM_DISCOVER_IGNORED_CAP; i < fl.ignored_count; i++) {
            free(fl.ignored[i].rel_path);
            free(fl.ignored[i].reason);
        }
        fl.ignored_count = CBM_DISCOVER_IGNORED_CAP;
    }

    *out = fl.files;
    *count = (int)fl.count;

    /* Hand the excluded-dir list to the caller, or free it if not requested. */
    if (excluded_out) {
        *excluded_out = fl.excluded;
        if (excluded_count_out) {
            *excluded_count_out = (int)fl.excluded_count;
        }
    } else {
        cbm_discover_free_excluded(fl.excluded, (int)fl.excluded_count);
    }

    /* Same handoff for the ignored-file list (#963). */
    if (ignored_out) {
        *ignored_out = fl.ignored;
        if (ignored_count_out) {
            *ignored_count_out = (int)fl.ignored_count;
        }
        if (ignored_total_out) {
            *ignored_total_out = (int)fl.ignored_total;
        }
    } else {
        cbm_discover_free_ignored(fl.ignored, (int)fl.ignored_count);
    }
    discovery_free_sources(&fl);
    if (snapshot_out) {
        *snapshot_out = selected_snapshot;
    }
    return 0;

load_failed:
    cbm_gitignore_free(root_gitignore);
    cbm_gitignore_free(info_exclude);
    cbm_gitignore_free(global_gi);
    cbm_gitignore_free(cbmignore);
    discovery_free_sources(&fl);
    return CBM_NOT_FOUND;
}

void cbm_discover_free(cbm_file_info_t *files, int count) {
    if (!files) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].rel_path);
    }
    free(files);
}

void cbm_discover_free_excluded(char **excluded, int count) {
    if (!excluded) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(excluded[i]);
    }
    free(excluded);
}

void cbm_discover_free_ignored(cbm_ignored_file_t *ignored, int count) {
    if (!ignored) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(ignored[i].rel_path);
        free(ignored[i].reason);
    }
    free(ignored);
}

const char *cbm_discovery_snapshot_fingerprint(const cbm_discovery_snapshot_t *snapshot) {
    return snapshot ? snapshot->fingerprint : NULL;
}

bool cbm_discovery_snapshot_verify(const cbm_discovery_snapshot_t *snapshot) {
    if (!snapshot || !snapshot->repo_path || !snapshot->fingerprint[0]) {
        return false;
    }
    cbm_file_info_t *files = NULL;
    char **excluded = NULL;
    cbm_ignored_file_t *ignored = NULL;
    int file_count = 0;
    int excluded_count = 0;
    int ignored_count = 0;
    int ignored_total = 0;
    cbm_discovery_snapshot_t *current = NULL;
    int rc = cbm_discover_ex3(snapshot->repo_path, &snapshot->opts, &files, &file_count, &excluded,
                              &excluded_count, &ignored, &ignored_count, &ignored_total, &current);
    cbm_discover_free(files, file_count);
    cbm_discover_free_excluded(excluded, excluded_count);
    cbm_discover_free_ignored(ignored, ignored_count);
    bool matches = rc == 0 && current && strcmp(snapshot->fingerprint, current->fingerprint) == 0;
    cbm_discovery_snapshot_free(current);
    return matches;
}

void cbm_discovery_snapshot_free(cbm_discovery_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    free(snapshot->repo_path);
    free(snapshot->ignore_file);
    free(snapshot);
}
