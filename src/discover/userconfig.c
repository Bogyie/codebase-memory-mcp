/*
 * userconfig.c — User-defined extension→language mappings.
 *
 * Reads extra_extensions from:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 */
#include "discover/userconfig.h"
#include "discover/discover.h"
#include "cbm.h" /* CBMLanguage, CBM_LANG_* */
#include "foundation/constants.h"
#include "foundation/platform.h" /* cbm_safe_getenv */
#include "foundation/compat_fs.h"
#include "foundation/sha256.h"
#include "foundation/rooted_file.h"
#include "git/git_context.h"

enum {
    MAX_CONFIG_SIZE = 65536,
    /* design_load_config consumes the same project config with a 1 MiB ceiling.
     * Capture/hash to the largest consumer bound, while retaining the legacy
     * 64 KiB policy for extra_extensions parsing below. */
    MAX_PROJECT_CONFIG_SNAPSHOT_SIZE = 1024 * 1024,
};
#include "foundation/log.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CBM_VERSION
#define CBM_VERSION "dev"
#endif

typedef enum {
    USERCONFIG_SOURCE_ABSENT = 0,
    USERCONFIG_SOURCE_PRESENT = 1,
    USERCONFIG_SOURCE_UNREADABLE = 2,
} userconfig_source_state_t;

typedef struct {
    char *path;
    userconfig_source_state_t state;
    char sha256[CBM_SHA256_HEX_LEN + 1];
    size_t max_bytes;
    size_t content_len;
} userconfig_source_snapshot_t;

typedef struct {
    char *rel_path;
    userconfig_source_state_t state;
    char sha256[CBM_SHA256_HEX_LEN + 1];
    cbm_userconfig_aux_kind_t kind;
    bool consumed;
} auxiliary_source_snapshot_t;

struct cbm_userconfig_snapshot {
    char *repo_path;
    userconfig_source_snapshot_t global;
    userconfig_source_snapshot_t project;
    auxiliary_source_snapshot_t *auxiliary;
    int auxiliary_count;
    char *auxiliary_repo_path;
    char **auxiliary_excluded_dirs;
    int auxiliary_excluded_count;
    bool auxiliary_captured;
    bool auxiliary_loader_mismatch;
    bool git_head_tracked;
    char *git_head_state;
    bool git_history_tracked;
    char git_history_sha256[CBM_SHA256_HEX_LEN + 1];
    bool git_context_tracked;
    char git_context_sha256[CBM_SHA256_HEX_LEN + 1];
    bool discovery_tracked;
    char discovery_sha256[CBM_SHA256_HEX_LEN + 1];
    char config_fingerprint[CBM_SHA256_HEX_LEN + 1];
    char fingerprint[CBM_SHA256_HEX_LEN + 1];
};

/* ── Process-global user config pointer ──────────────────────────── */

static const cbm_userconfig_t *g_userconfig = NULL;

void cbm_set_user_lang_config(const cbm_userconfig_t *cfg) {
    g_userconfig = cfg;
}

const cbm_userconfig_t *cbm_get_user_lang_config(void) {
    return g_userconfig;
}

/* ── Language name → enum table ──────────────────────────────────── */

/*
 * Reverse-mapping from lowercase language name strings to CBMLanguage.
 * Covers all names exposed by cbm_language_name() plus common aliases.
 */
typedef struct {
    const char *name; /* lowercase */
    CBMLanguage lang;
} lang_name_entry_t;

static const lang_name_entry_t LANG_NAME_TABLE[] = {
    {"go", CBM_LANG_GO},
    {"python", CBM_LANG_PYTHON},
    {"javascript", CBM_LANG_JAVASCRIPT},
    {"typescript", CBM_LANG_TYPESCRIPT},
    {"tsx", CBM_LANG_TSX},
    {"rust", CBM_LANG_RUST},
    {"java", CBM_LANG_JAVA},
    {"c++", CBM_LANG_CPP},
    {"cpp", CBM_LANG_CPP},
    {"c#", CBM_LANG_CSHARP},
    {"csharp", CBM_LANG_CSHARP},
    {"php", CBM_LANG_PHP},
    {"lua", CBM_LANG_LUA},
    {"scala", CBM_LANG_SCALA},
    {"kotlin", CBM_LANG_KOTLIN},
    {"ruby", CBM_LANG_RUBY},
    {"c", CBM_LANG_C},
    {"bash", CBM_LANG_BASH},
    {"sh", CBM_LANG_BASH},
    {"zig", CBM_LANG_ZIG},
    {"elixir", CBM_LANG_ELIXIR},
    {"haskell", CBM_LANG_HASKELL},
    {"ocaml", CBM_LANG_OCAML},
    {"objective-c", CBM_LANG_OBJC},
    {"objc", CBM_LANG_OBJC},
    {"swift", CBM_LANG_SWIFT},
    {"dart", CBM_LANG_DART},
    {"perl", CBM_LANG_PERL},
    {"groovy", CBM_LANG_GROOVY},
    {"erlang", CBM_LANG_ERLANG},
    {"r", CBM_LANG_R},
    {"html", CBM_LANG_HTML},
    {"css", CBM_LANG_CSS},
    {"scss", CBM_LANG_SCSS},
    {"yaml", CBM_LANG_YAML},
    {"toml", CBM_LANG_TOML},
    {"hcl", CBM_LANG_HCL},
    {"terraform", CBM_LANG_HCL},
    {"sql", CBM_LANG_SQL},
    {"dockerfile", CBM_LANG_DOCKERFILE},
    {"clojure", CBM_LANG_CLOJURE},
    {"f#", CBM_LANG_FSHARP},
    {"fsharp", CBM_LANG_FSHARP},
    {"julia", CBM_LANG_JULIA},
    {"vimscript", CBM_LANG_VIMSCRIPT},
    {"nix", CBM_LANG_NIX},
    {"common lisp", CBM_LANG_COMMONLISP},
    {"commonlisp", CBM_LANG_COMMONLISP},
    {"lisp", CBM_LANG_COMMONLISP},
    {"elm", CBM_LANG_ELM},
    {"fortran", CBM_LANG_FORTRAN},
    {"cuda", CBM_LANG_CUDA},
    {"cobol", CBM_LANG_COBOL},
    {"verilog", CBM_LANG_VERILOG},
    {"emacs lisp", CBM_LANG_EMACSLISP},
    {"emacslisp", CBM_LANG_EMACSLISP},
    {"json", CBM_LANG_JSON},
    {"xml", CBM_LANG_XML},
    {"markdown", CBM_LANG_MARKDOWN},
    {"makefile", CBM_LANG_MAKEFILE},
    {"cmake", CBM_LANG_CMAKE},
    {"protobuf", CBM_LANG_PROTOBUF},
    {"graphql", CBM_LANG_GRAPHQL},
    {"vue", CBM_LANG_VUE},
    {"svelte", CBM_LANG_SVELTE},
    {"meson", CBM_LANG_MESON},
    {"glsl", CBM_LANG_GLSL},
    {"ini", CBM_LANG_INI},
    {"matlab", CBM_LANG_MATLAB},
    {"mojo", CBM_LANG_MOJO},
    {"lean", CBM_LANG_LEAN},
    {"form", CBM_LANG_FORM},
    {"magma", CBM_LANG_MAGMA},
    {"wolfram", CBM_LANG_WOLFRAM},
};

#define LANG_NAME_TABLE_SIZE (sizeof(LANG_NAME_TABLE) / sizeof(LANG_NAME_TABLE[0]))

/*
 * Parse a language string (case-insensitive) to a CBMLanguage enum.
 * Returns CBM_LANG_COUNT if the string is not recognized.
 */
static CBMLanguage lang_from_string(const char *s) {
    if (!s || !s[0]) {
        return CBM_LANG_COUNT;
    }

    /* Build a lowercase copy for comparison */
    char lower[CBM_SZ_64];
    size_t i;
    for (i = 0; i < sizeof(lower) - SKIP_ONE && s[i]; i++) {
        lower[i] = (char)tolower((unsigned char)s[i]);
    }
    lower[i] = '\0';

    for (size_t j = 0; j < LANG_NAME_TABLE_SIZE; j++) {
        if (strcmp(LANG_NAME_TABLE[j].name, lower) == 0) {
            return LANG_NAME_TABLE[j].lang;
        }
    }
    return CBM_LANG_COUNT;
}

/* ── Config directory helper ─────────────────────────────────────── */

/* cbm_app_config_dir() is now in platform.c (cross-platform). */

/* ── JSON parsing ────────────────────────────────────────────────── */

/*
 * Parse extra_extensions from a yyjson object root.
 * Appends valid entries to *entries / *count (growing via realloc).
 * Project-level entries (from_project=true) are appended after global
 * entries so that a later dedup pass can prefer project values.
 *
 * Returns 0 on success, -1 on alloc failure.
 */
static int parse_extra_extensions(yyjson_val *root, cbm_userext_t **entries, int *count,
                                  const char *source_label) {
    if (!yyjson_is_obj(root)) {
        cbm_log_warn("userconfig.bad_root", "file", source_label);
        return 0;
    }

    yyjson_val *extra = yyjson_obj_get(root, "extra_extensions");
    if (!extra) {
        return 0; /* key absent — fine */
    }
    if (!yyjson_is_obj(extra)) {
        cbm_log_warn("userconfig.bad_extra_extensions", "file", source_label);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(extra, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);

        const char *ext_str = yyjson_get_str(key);
        const char *lang_str = yyjson_get_str(val);

        if (!ext_str || !lang_str) {
            cbm_log_warn("userconfig.skip_non_string", "file", source_label);
            continue;
        }

        /* Extension must start with '.' */
        if (ext_str[0] != '.') {
            cbm_log_warn("userconfig.skip_bad_ext", "file", source_label, "ext", ext_str);
            continue;
        }

        CBMLanguage lang = lang_from_string(lang_str);
        if (lang == CBM_LANG_COUNT) {
            cbm_log_warn("userconfig.unknown_lang", "file", source_label, "lang", lang_str);
            continue; /* fail-open: skip unknown languages */
        }

        /* Grow the array */
        cbm_userext_t *tmp = realloc(*entries, (size_t)(*count + SKIP_ONE) * sizeof(cbm_userext_t));
        if (!tmp) {
            return CBM_NOT_FOUND;
        }
        *entries = tmp;

        char *ext_copy = strdup(ext_str);
        if (!ext_copy) {
            return CBM_NOT_FOUND;
        }

        (*entries)[*count].ext = ext_copy;
        (*entries)[*count].lang = lang;
        (*count)++;
    }
    return 0;
}

/* Capture one config source.  PRESENT means data/hash came from the same
 * stable regular-file descriptor.  A missing path is different from an
 * existing path that cannot safely be read (including an oversized file), so
 * both states participate in the combined fingerprint. */
static int capture_config_source(const char *path, const char *repo_root, const char *relative_path,
                                 size_t max_bytes, userconfig_source_state_t *state,
                                 char sha256[CBM_SHA256_HEX_LEN + 1], char **out_data,
                                 size_t *out_len) {
    if (!path || !state || !sha256 || !out_data || !out_len) {
        return CBM_NOT_FOUND;
    }
    *out_data = NULL;
    *out_len = 0;
    sha256[0] = '\0';
    int rc = CBM_SHA256_FILE_ERROR;
    if (repo_root && relative_path) {
        cbm_rooted_file_t rooted = {0};
        cbm_rooted_file_status_t rooted_rc =
            cbm_rooted_file_read(repo_root, relative_path, max_bytes, &rooted);
        if (rooted_rc == CBM_ROOTED_FILE_OK) {
            *out_data = rooted.data;
            rooted.data = NULL;
            *out_len = rooted.len;
            memcpy(sha256, rooted.sha256, CBM_SHA256_HEX_LEN + 1U);
            rc = CBM_SHA256_FILE_HASHED;
        } else if (rooted_rc == CBM_ROOTED_FILE_OUT_OF_MEMORY) {
            cbm_rooted_file_free(&rooted);
            return CBM_NOT_FOUND;
        }
        cbm_rooted_file_free(&rooted);
    } else {
        rc = cbm_sha256_file_read_hex(path, max_bytes, out_data, out_len, sha256);
    }
    if (rc == CBM_SHA256_FILE_HASHED) {
        *state = USERCONFIG_SOURCE_PRESENT;
        return 0;
    }
    if (rc == CBM_SHA256_FILE_OUT_OF_MEMORY) {
        return CBM_NOT_FOUND;
    }
    int probe = cbm_path_probe(path);
    *state = probe == 0 ? USERCONFIG_SOURCE_ABSENT : USERCONFIG_SOURCE_UNREADABLE;
    return 0;
}

/* Read a JSON file and parse the exact bytes hashed by capture_config_source.
 * Missing files are ignored, while their absence is retained in source for
 * the completed-index fingerprint. */
static int load_config_file(const char *path, const char *repo_root, const char *relative_path,
                            cbm_userext_t **entries, int *count, size_t snapshot_max_bytes,
                            userconfig_source_snapshot_t *source) {
    source->path = strdup(path);
    if (!source->path) {
        return CBM_NOT_FOUND;
    }
    char *buf = NULL;
    size_t len = 0;
    source->max_bytes = snapshot_max_bytes;
    if (capture_config_source(path, repo_root, relative_path, snapshot_max_bytes, &source->state,
                              source->sha256, &buf, &len) != 0) {
        return CBM_NOT_FOUND;
    }
    source->content_len = len;
    if (source->state == USERCONFIG_SOURCE_ABSENT) {
        return 0;
    }
    if (source->state == USERCONFIG_SOURCE_UNREADABLE) {
        cbm_log_warn("userconfig.read_failed", "path", path);
        return 0;
    }
    if (len > MAX_CONFIG_SIZE) {
        cbm_log_warn("userconfig.file_too_large", "path", path);
        free(buf);
        return 0;
    }
    if (len == 0) {
        free(buf);
        return 0;
    }

    yyjson_doc *doc = yyjson_read(buf, len, 0);
    free(buf);

    if (!doc) {
        cbm_log_warn("userconfig.corrupt_json", "path", path);
        return 0; /* corrupt JSON — silently ignore (fail-open) */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    int rc = parse_extra_extensions(root, entries, count, path);
    yyjson_doc_free(doc);
    return rc;
}

static char *join_config_path(const char *base, const char *suffix) {
    if (!base || !suffix) {
        return NULL;
    }
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    bool need_sep =
        base_len > 0 && base[base_len - SKIP_ONE] != '/' && base[base_len - SKIP_ONE] != '\\';
    if (base_len > SIZE_MAX - suffix_len - (need_sep ? PAIR_LEN : SKIP_ONE)) {
        return NULL;
    }
    size_t total = base_len + (need_sep ? SKIP_ONE : 0) + suffix_len + SKIP_ONE;
    char *path = (char *)malloc(total);
    if (!path) {
        return NULL;
    }
    memcpy(path, base, base_len);
    size_t pos = base_len;
    if (need_sep) {
        path[pos++] = '/';
    }
    memcpy(path + pos, suffix, suffix_len + SKIP_ONE);
    return path;
}

typedef struct {
    auxiliary_source_snapshot_t *items;
    int count;
    int cap;
} auxiliary_scan_t;

static void auxiliary_scan_free(auxiliary_scan_t *scan) {
    if (!scan) {
        return;
    }
    for (int i = 0; i < scan->count; i++) {
        free(scan->items[i].rel_path);
    }
    free(scan->items);
    memset(scan, 0, sizeof(*scan));
}

static bool auxiliary_relpath_excluded(const char *rel_path, char **excluded_dirs,
                                       int excluded_count) {
    if (!rel_path) {
        return false;
    }
    for (int i = 0; i < excluded_count; i++) {
        const char *excluded = excluded_dirs ? excluded_dirs[i] : NULL;
        if (!excluded || !excluded[0]) {
            continue;
        }
        size_t n = strlen(excluded);
        if (strncmp(rel_path, excluded, n) == 0 && (rel_path[n] == '\0' || rel_path[n] == '/')) {
            return true;
        }
    }
    return false;
}

static bool auxiliary_ends_with(const char *value, const char *suffix) {
    size_t value_len = value ? strlen(value) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    return suffix_len <= value_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool auxiliary_is_pkgmap_manifest(const char *name) {
    static const char *const names[] = {
        "package.json", "go.mod",  "Cargo.toml",   "pyproject.toml",   "composer.json",
        "pubspec.yaml", "pom.xml", "build.gradle", "build.gradle.kts", "mix.exs",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return auxiliary_ends_with(name, ".gemspec");
}

static bool auxiliary_alias_dir_skipped(const char *name) {
    return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "dist") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, ".next") == 0 ||
           strcmp(name, "coverage") == 0 || strcmp(name, "target") == 0;
}

static int auxiliary_scan_add(auxiliary_scan_t *scan, const char *root_path, const char *rel_path,
                              size_t max_bytes, cbm_userconfig_aux_kind_t kind) {
    if (!scan || !root_path || !rel_path) {
        return CBM_NOT_FOUND;
    }
    if (scan->count == scan->cap) {
        int next_cap = scan->cap > 0 ? scan->cap * PAIR_LEN : CBM_SZ_16;
        if (next_cap <= scan->cap || (size_t)next_cap > SIZE_MAX / sizeof(*scan->items)) {
            return CBM_NOT_FOUND;
        }
        auxiliary_source_snapshot_t *grown = (auxiliary_source_snapshot_t *)realloc(
            scan->items, (size_t)next_cap * sizeof(*scan->items));
        if (!grown) {
            return CBM_NOT_FOUND;
        }
        scan->items = grown;
        scan->cap = next_cap;
    }
    auxiliary_source_snapshot_t *item = &scan->items[scan->count];
    memset(item, 0, sizeof(*item));
    item->rel_path = strdup(rel_path);
    if (!item->rel_path) {
        return CBM_NOT_FOUND;
    }
    item->kind = kind;
    cbm_rooted_file_t source = {0};
    cbm_rooted_file_status_t read_rc =
        cbm_rooted_file_read(root_path, rel_path, max_bytes, &source);
    if (read_rc == CBM_ROOTED_FILE_OK) {
        item->state = USERCONFIG_SOURCE_PRESENT;
        memcpy(item->sha256, source.sha256, sizeof(item->sha256));
    } else if (read_rc == CBM_ROOTED_FILE_OUT_OF_MEMORY) {
        free(item->rel_path);
        item->rel_path = NULL;
        cbm_rooted_file_free(&source);
        return CBM_NOT_FOUND;
    } else {
        item->state = USERCONFIG_SOURCE_UNREADABLE;
        item->sha256[0] = '\0';
    }
    cbm_rooted_file_free(&source);
    scan->count++;
    return 0;
}

static int auxiliary_scan_dir(const char *root_path, const char *abs_dir, const char *rel_dir,
                              int depth, bool scan_pkgmap, bool scan_aliases, char **excluded_dirs,
                              int excluded_count, int *alias_count, int *pkgmap_count,
                              auxiliary_scan_t *scan) {
    enum {
        ALIAS_MAX_DEPTH = 32,
        ALIAS_MAX_FILES = 256,
        PKGMAP_MAX_DEPTH = 64,
        PKGMAP_MAX_FILES = 4096,
    };
    if (!scan_pkgmap && !scan_aliases) {
        return 0;
    }

    /* The alias loader chooses at most one config per directory, preferring
     * tsconfig.json over jsconfig.json. Mirror that selection exactly. */
    if (scan_aliases && depth <= ALIAS_MAX_DEPTH) {
        static const char *const alias_names[] = {"tsconfig.json", "jsconfig.json"};
        for (size_t i = 0; i < sizeof(alias_names) / sizeof(alias_names[0]); i++) {
            char abs_path[CBM_SZ_4K];
            char rel_path[CBM_SZ_4K];
            int abs_n = snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, alias_names[i]);
            int rel_n = rel_dir && rel_dir[0]
                            ? snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, alias_names[i])
                            : snprintf(rel_path, sizeof(rel_path), "%s", alias_names[i]);
            if (abs_n < 0 || abs_n >= (int)sizeof(abs_path) || rel_n < 0 ||
                rel_n >= (int)sizeof(rel_path)) {
                return CBM_NOT_FOUND;
            }
            cbm_rooted_file_t probe = {0};
            cbm_rooted_file_status_t probe_rc =
                cbm_rooted_file_read(root_path, rel_path, MAX_CONFIG_SIZE, &probe);
            cbm_rooted_file_free(&probe);
            if (probe_rc == CBM_ROOTED_FILE_OUT_OF_MEMORY) {
                return CBM_NOT_FOUND;
            }
            int path_state = cbm_path_probe(abs_path);
            if (probe_rc == CBM_ROOTED_FILE_NOT_FOUND && path_state == 0) {
                continue;
            }
            if (probe_rc != CBM_ROOTED_FILE_NOT_FOUND || path_state != 0) {
                if (auxiliary_scan_add(scan, root_path, rel_path, MAX_CONFIG_SIZE,
                                       CBM_USERCONFIG_AUX_PATH_ALIAS) != 0) {
                    return CBM_NOT_FOUND;
                }
                (*alias_count)++;
                if (*alias_count > ALIAS_MAX_FILES) {
                    cbm_log_warn("index_inputs.path_alias_cap_hit", "repo", abs_dir, "limit",
                                 "256");
                    return CBM_NOT_FOUND;
                }
                break;
            }
        }
    }

    cbm_dir_t *dir = cbm_opendir(abs_dir);
    if (!dir) {
        return CBM_NOT_FOUND;
    }
    int rc = 0;
    cbm_dirent_t *entry;
    while (rc == 0 && (entry = cbm_readdir(dir)) != NULL) {
        const char *name = entry->name;
        char abs_path[CBM_SZ_4K];
        char rel_path[CBM_SZ_4K];
        int abs_n = snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, name);
        int rel_n = rel_dir && rel_dir[0]
                        ? snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, name)
                        : snprintf(rel_path, sizeof(rel_path), "%s", name);
        if (abs_n < 0 || abs_n >= (int)sizeof(abs_path) || rel_n < 0 ||
            rel_n >= (int)sizeof(rel_path)) {
            rc = CBM_NOT_FOUND;
            continue;
        }
        if (entry->is_reparse) {
            continue;
        }
        if (entry->is_dir) {
            if (auxiliary_relpath_excluded(rel_path, excluded_dirs, excluded_count)) {
                continue;
            }
            bool child_pkgmap = scan_pkgmap && !cbm_should_skip_dir(name, CBM_MODE_FULL);
            bool child_aliases = scan_aliases && !auxiliary_alias_dir_skipped(name);
            if ((child_pkgmap && depth + SKIP_ONE >= PKGMAP_MAX_DEPTH) ||
                (child_aliases && depth >= ALIAS_MAX_DEPTH)) {
                rc = CBM_NOT_FOUND;
                continue;
            }
            rc = auxiliary_scan_dir(root_path, abs_path, rel_path, depth + SKIP_ONE, child_pkgmap,
                                    child_aliases, excluded_dirs, excluded_count, alias_count,
                                    pkgmap_count, scan);
            continue;
        }
        if (scan_pkgmap && auxiliary_is_pkgmap_manifest(name)) {
            if (*pkgmap_count >= PKGMAP_MAX_FILES) {
                cbm_log_warn("index_inputs.pkgmap_cap_hit", "repo", abs_dir, "kept", "4096");
                rc = CBM_NOT_FOUND;
                continue;
            }
#ifndef _WIN32
            struct stat st;
            if (lstat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
#endif
            rc = auxiliary_scan_add(scan, root_path, rel_path, 1024U * 1024U,
                                    CBM_USERCONFIG_AUX_PKGMAP);
            if (rc == 0) {
                (*pkgmap_count)++;
            }
        }
    }
    if (rc == 0 && cbm_dir_had_error(dir)) {
        rc = CBM_NOT_FOUND;
    }
    cbm_closedir(dir);
    return rc;
}

static int auxiliary_cmp(const void *a, const void *b) {
    const auxiliary_source_snapshot_t *left = (const auxiliary_source_snapshot_t *)a;
    const auxiliary_source_snapshot_t *right = (const auxiliary_source_snapshot_t *)b;
    return strcmp(left->rel_path, right->rel_path);
}

static int auxiliary_scan_repo(const char *repo_path, char **excluded_dirs, int excluded_count,
                               auxiliary_scan_t *scan) {
    if (!repo_path || !scan || excluded_count < 0) {
        return CBM_NOT_FOUND;
    }
    int alias_count = 0;
    int pkgmap_count = 0;
    int rc = auxiliary_scan_dir(repo_path, repo_path, "", 0, true, true, excluded_dirs,
                                excluded_count, &alias_count, &pkgmap_count, scan);
    if (rc == 0 && scan->count > 1) {
        qsort(scan->items, (size_t)scan->count, sizeof(*scan->items), auxiliary_cmp);
    }
    return rc;
}

static void sha256_update_u64_be(cbm_sha256_ctx *ctx, uint64_t value) {
    unsigned char encoded[8];
    for (int i = 7; i >= 0; i--) {
        encoded[i] = (unsigned char)(value & 0xffU);
        value >>= 8;
    }
    cbm_sha256_update(ctx, encoded, sizeof(encoded));
}

static void compute_snapshot_fingerprint(cbm_userconfig_snapshot_t *snapshot) {
    static const char domain[] = "cbm-index-inputs-v3";
    static const char config_domain[] = "cbm-base-index-inputs-v3";
    static const char semantics_domain[] = "cbm-indexer-semantics";
    const userconfig_source_snapshot_t *sources[] = {&snapshot->global, &snapshot->project};
    cbm_sha256_ctx config_ctx;
    cbm_sha256_init(&config_ctx);
    cbm_sha256_update(&config_ctx, config_domain, sizeof(config_domain));
    cbm_sha256_update(&config_ctx, semantics_domain, sizeof(semantics_domain));
    cbm_sha256_update(&config_ctx, CBM_INDEXER_SEMANTICS_VERSION,
                      sizeof(CBM_INDEXER_SEMANTICS_VERSION));
    cbm_sha256_update(&config_ctx, CBM_VERSION, sizeof(CBM_VERSION));
    for (unsigned char i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        unsigned char state[] = {i, (unsigned char)sources[i]->state};
        cbm_sha256_update(&config_ctx, state, sizeof(state));
        if (sources[i]->state == USERCONFIG_SOURCE_PRESENT) {
            cbm_sha256_update(&config_ctx, sources[i]->sha256, CBM_SHA256_HEX_LEN);
        }
    }
    unsigned char base_auxiliary_state = snapshot->auxiliary_captured ? 1U : 0U;
    cbm_sha256_update(&config_ctx, &base_auxiliary_state, sizeof(base_auxiliary_state));
    sha256_update_u64_be(&config_ctx, (uint64_t)snapshot->auxiliary_count);
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        size_t rel_len = strlen(item->rel_path);
        sha256_update_u64_be(&config_ctx, (uint64_t)rel_len);
        cbm_sha256_update(&config_ctx, item->rel_path, rel_len);
        unsigned char state = (unsigned char)item->state;
        unsigned char kind = (unsigned char)item->kind;
        cbm_sha256_update(&config_ctx, &state, sizeof(state));
        cbm_sha256_update(&config_ctx, &kind, sizeof(kind));
        if (item->state == USERCONFIG_SOURCE_PRESENT) {
            cbm_sha256_update(&config_ctx, item->sha256, CBM_SHA256_HEX_LEN);
        }
    }
    unsigned char base_context_state = snapshot->git_context_tracked ? 1U : 0U;
    cbm_sha256_update(&config_ctx, &base_context_state, sizeof(base_context_state));
    if (snapshot->git_context_tracked) {
        cbm_sha256_update(&config_ctx, snapshot->git_context_sha256, CBM_SHA256_HEX_LEN);
    }
    unsigned char base_discovery_state = snapshot->discovery_tracked ? 1U : 0U;
    cbm_sha256_update(&config_ctx, &base_discovery_state, sizeof(base_discovery_state));
    if (snapshot->discovery_tracked) {
        cbm_sha256_update(&config_ctx, snapshot->discovery_sha256, CBM_SHA256_HEX_LEN);
    }
    uint8_t config_digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&config_ctx, config_digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        snapshot->config_fingerprint[i * PAIR_LEN] = hex[config_digest[i] >> 4];
        snapshot->config_fingerprint[i * PAIR_LEN + SKIP_ONE] = hex[config_digest[i] & 0x0f];
    }
    snapshot->config_fingerprint[CBM_SHA256_HEX_LEN] = '\0';

    cbm_sha256_ctx ctx;
    cbm_sha256_init(&ctx);
    cbm_sha256_update(&ctx, domain, sizeof(domain));
    cbm_sha256_update(&ctx, semantics_domain, sizeof(semantics_domain));
    cbm_sha256_update(&ctx, CBM_INDEXER_SEMANTICS_VERSION, sizeof(CBM_INDEXER_SEMANTICS_VERSION));
    cbm_sha256_update(&ctx, CBM_VERSION, sizeof(CBM_VERSION));
    for (unsigned char i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        unsigned char state[] = {i, (unsigned char)sources[i]->state};
        cbm_sha256_update(&ctx, state, sizeof(state));
        if (sources[i]->state == USERCONFIG_SOURCE_PRESENT) {
            cbm_sha256_update(&ctx, sources[i]->sha256, CBM_SHA256_HEX_LEN);
        }
    }
    unsigned char auxiliary_state = snapshot->auxiliary_captured ? 1U : 0U;
    cbm_sha256_update(&ctx, &auxiliary_state, sizeof(auxiliary_state));
    sha256_update_u64_be(&ctx, (uint64_t)snapshot->auxiliary_count);
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        size_t rel_len = strlen(item->rel_path);
        sha256_update_u64_be(&ctx, (uint64_t)rel_len);
        cbm_sha256_update(&ctx, item->rel_path, rel_len);
        unsigned char state = (unsigned char)item->state;
        cbm_sha256_update(&ctx, &state, sizeof(state));
        unsigned char kind = (unsigned char)item->kind;
        cbm_sha256_update(&ctx, &kind, sizeof(kind));
        if (item->state == USERCONFIG_SOURCE_PRESENT) {
            cbm_sha256_update(&ctx, item->sha256, CBM_SHA256_HEX_LEN);
        }
    }
    unsigned char git_state = snapshot->git_head_tracked ? 1U : 0U;
    cbm_sha256_update(&ctx, &git_state, sizeof(git_state));
    if (snapshot->git_head_tracked && snapshot->git_head_state) {
        size_t n = strlen(snapshot->git_head_state);
        sha256_update_u64_be(&ctx, (uint64_t)n);
        cbm_sha256_update(&ctx, snapshot->git_head_state, n);
    }
    unsigned char history_state = snapshot->git_history_tracked ? 1U : 0U;
    cbm_sha256_update(&ctx, &history_state, sizeof(history_state));
    if (snapshot->git_history_tracked) {
        cbm_sha256_update(&ctx, snapshot->git_history_sha256, CBM_SHA256_HEX_LEN);
    }
    unsigned char context_state = snapshot->git_context_tracked ? 1U : 0U;
    cbm_sha256_update(&ctx, &context_state, sizeof(context_state));
    if (snapshot->git_context_tracked) {
        cbm_sha256_update(&ctx, snapshot->git_context_sha256, CBM_SHA256_HEX_LEN);
    }
    unsigned char discovery_state = snapshot->discovery_tracked ? 1U : 0U;
    cbm_sha256_update(&ctx, &discovery_state, sizeof(discovery_state));
    if (snapshot->discovery_tracked) {
        cbm_sha256_update(&ctx, snapshot->discovery_sha256, CBM_SHA256_HEX_LEN);
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&ctx, digest);
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        snapshot->fingerprint[i * PAIR_LEN] = hex[digest[i] >> 4];
        snapshot->fingerprint[i * PAIR_LEN + SKIP_ONE] = hex[digest[i] & 0x0f];
    }
    snapshot->fingerprint[CBM_SHA256_HEX_LEN] = '\0';
}

static void free_entries(cbm_userext_t *entries, int count) {
    for (int i = 0; i < count; i++) {
        free(entries[i].ext);
    }
    free(entries);
}

/* ── Public API ──────────────────────────────────────────────────── */

cbm_userconfig_t *cbm_userconfig_load_with_snapshot(const char *repo_path,
                                                    cbm_userconfig_snapshot_t **out_snapshot) {
    if (out_snapshot) {
        *out_snapshot = NULL;
    }
    cbm_userconfig_t *cfg = calloc(CBM_ALLOC_ONE, sizeof(cbm_userconfig_t));
    cbm_userconfig_snapshot_t *snapshot =
        (cbm_userconfig_snapshot_t *)calloc(CBM_ALLOC_ONE, sizeof(*snapshot));
    if (!cfg || !snapshot) {
        free(cfg);
        free(snapshot);
        return NULL;
    }
    snapshot->repo_path = strdup(repo_path ? repo_path : "");
    if (!snapshot->repo_path) {
        free(cfg);
        free(snapshot);
        return NULL;
    }

    cbm_userext_t *entries = NULL;
    int count = 0;

    /* ── Step 1: Load global config ── */
    const char *cfg_base = cbm_app_config_dir();
    if (cfg_base) {
        char *global_path = join_config_path(cfg_base, "codebase-memory-mcp/config.json");
        if (!global_path || load_config_file(global_path, NULL, NULL, &entries, &count,
                                             MAX_CONFIG_SIZE, &snapshot->global) != 0) {
            free(global_path);
            free_entries(entries, count);
            free(cfg);
            cbm_userconfig_snapshot_free(snapshot);
            return NULL;
        }
        free(global_path);
    }

    int global_count = count; /* entries[0..global_count) are from global */

    /* ── Step 2: Load project config ── */
    if (repo_path && repo_path[0]) {
        char *project_path = join_config_path(repo_path, ".codebase-memory.json");

        if (!project_path ||
            load_config_file(project_path, repo_path, ".codebase-memory.json", &entries, &count,
                             MAX_PROJECT_CONFIG_SNAPSHOT_SIZE, &snapshot->project) != 0) {
            free(project_path);
            free_entries(entries, count);
            free(cfg);
            cbm_userconfig_snapshot_free(snapshot);
            return NULL;
        }
        free(project_path);
    }

    /*
     * ── Step 3: Dedup — project entries win over global ──
     *
     * For any extension that appears in both global (indices 0..global_count)
     * and project (indices global_count..count), remove the global entry by
     * replacing it with the last global entry (order-insensitive dedup).
     */
    for (int p = global_count; p < count; p++) {
        for (int g = 0; g < global_count; g++) {
            if (entries[g].ext && strcmp(entries[g].ext, entries[p].ext) == 0) {
                /* Remove global entry: overwrite with last global entry */
                free(entries[g].ext);
                entries[g] = entries[global_count - SKIP_ONE];
                entries[global_count - SKIP_ONE].ext = NULL; /* mark as consumed */
                global_count--;
                break;
            }
        }
    }

    /*
     * Compact: remove any NULL-ext slots left by the dedup step.
     * (Those are the consumed "last global" entries.)
     */
    int write_idx = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].ext != NULL) {
            entries[write_idx++] = entries[i];
        }
    }
    count = write_idx;

    cfg->entries = entries;
    cfg->count = count;
    compute_snapshot_fingerprint(snapshot);
    if (out_snapshot) {
        *out_snapshot = snapshot;
    } else {
        cbm_userconfig_snapshot_free(snapshot);
    }
    return cfg;
}

cbm_userconfig_t *cbm_userconfig_load(const char *repo_path) {
    return cbm_userconfig_load_with_snapshot(repo_path, NULL);
}

const char *cbm_userconfig_snapshot_fingerprint(const cbm_userconfig_snapshot_t *snapshot) {
    return snapshot ? snapshot->fingerprint : NULL;
}

const char *cbm_userconfig_snapshot_config_fingerprint(const cbm_userconfig_snapshot_t *snapshot) {
    return snapshot ? snapshot->config_fingerprint : NULL;
}

const char *cbm_userconfig_snapshot_project_sha256(const cbm_userconfig_snapshot_t *snapshot) {
    return snapshot && snapshot->project.state == USERCONFIG_SOURCE_PRESENT
               ? snapshot->project.sha256
               : NULL;
}

int cbm_userconfig_snapshot_project_source(const cbm_userconfig_snapshot_t *snapshot,
                                           const char **out_sha256, size_t *out_len) {
    if (out_sha256) {
        *out_sha256 = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!snapshot || snapshot->project.state != USERCONFIG_SOURCE_PRESENT) {
        return 0;
    }
    if (out_sha256) {
        *out_sha256 = snapshot->project.sha256;
    }
    if (out_len) {
        *out_len = snapshot->project.content_len;
    }
    return 1;
}

int cbm_userconfig_snapshot_capture_auxiliary(cbm_userconfig_snapshot_t *snapshot,
                                              const char *repo_path, char **excluded_dirs,
                                              int excluded_count) {
    if (!snapshot || !repo_path || !snapshot->repo_path ||
        strcmp(repo_path, snapshot->repo_path) != 0 || excluded_count < 0) {
        return CBM_NOT_FOUND;
    }
    auxiliary_scan_t scan = {0};
    if (auxiliary_scan_repo(repo_path, excluded_dirs, excluded_count, &scan) != 0) {
        auxiliary_scan_free(&scan);
        return CBM_NOT_FOUND;
    }
    char *repo_copy = strdup(repo_path);
    char **excluded_copy = NULL;
    if (!repo_copy) {
        auxiliary_scan_free(&scan);
        return CBM_NOT_FOUND;
    }
    if (excluded_count > 0) {
        excluded_copy = (char **)calloc((size_t)excluded_count, sizeof(*excluded_copy));
        if (!excluded_copy) {
            free(repo_copy);
            auxiliary_scan_free(&scan);
            return CBM_NOT_FOUND;
        }
        for (int i = 0; i < excluded_count; i++) {
            excluded_copy[i] = strdup(excluded_dirs && excluded_dirs[i] ? excluded_dirs[i] : "");
            if (!excluded_copy[i]) {
                for (int j = 0; j < i; j++) {
                    free(excluded_copy[j]);
                }
                free(excluded_copy);
                free(repo_copy);
                auxiliary_scan_free(&scan);
                return CBM_NOT_FOUND;
            }
        }
    }

    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        free(snapshot->auxiliary[i].rel_path);
    }
    free(snapshot->auxiliary);
    for (int i = 0; i < snapshot->auxiliary_excluded_count; i++) {
        free(snapshot->auxiliary_excluded_dirs[i]);
    }
    free(snapshot->auxiliary_excluded_dirs);
    free(snapshot->auxiliary_repo_path);

    snapshot->auxiliary = scan.items;
    snapshot->auxiliary_count = scan.count;
    snapshot->auxiliary_repo_path = repo_copy;
    snapshot->auxiliary_excluded_dirs = excluded_copy;
    snapshot->auxiliary_excluded_count = excluded_count;
    snapshot->auxiliary_captured = true;
    compute_snapshot_fingerprint(snapshot);
    return 0;
}

static char *capture_git_head_state(const char *repo_path) {
    if (!repo_path || !repo_path[0]) {
        return NULL;
    }
    cbm_git_context_t git = {0};
    if (cbm_git_context_resolve(repo_path, &git) != 0) {
        cbm_git_context_free(&git);
        return NULL;
    }
    const char *state =
        !git.is_git ? "not-git" : (git.head_sha && git.head_sha[0] ? git.head_sha : "unborn");
    char *copy = strdup(state);
    cbm_git_context_free(&git);
    return copy;
}

int cbm_userconfig_snapshot_capture_git_head(cbm_userconfig_snapshot_t *snapshot,
                                             const char *repo_path, bool enabled) {
    if (!snapshot || !repo_path || !snapshot->repo_path ||
        strcmp(repo_path, snapshot->repo_path) != 0) {
        return CBM_NOT_FOUND;
    }
    char *state = enabled ? capture_git_head_state(repo_path) : NULL;
    if (enabled && !state) {
        return CBM_NOT_FOUND;
    }
    char history_sha256[CBM_SHA256_HEX_LEN + 1] = "";
    if (enabled && strcmp(state, "not-git") != 0 && strcmp(state, "unborn") != 0) {
        char *history = NULL;
        size_t history_len = 0;
        if (cbm_git_history_capture(repo_path, state, &history, &history_len, history_sha256) !=
            0) {
            free(history);
            free(state);
            return CBM_NOT_FOUND;
        }
        free(history);
    }
    free(snapshot->git_head_state);
    snapshot->git_head_state = state;
    snapshot->git_head_tracked = enabled;
    snapshot->git_history_tracked = enabled;
    memcpy(snapshot->git_history_sha256, history_sha256, sizeof(history_sha256));
    compute_snapshot_fingerprint(snapshot);
    return 0;
}

const char *cbm_userconfig_snapshot_git_head(const cbm_userconfig_snapshot_t *snapshot) {
    if (!snapshot || !snapshot->git_head_tracked || !snapshot->git_head_state ||
        strcmp(snapshot->git_head_state, "not-git") == 0 ||
        strcmp(snapshot->git_head_state, "unborn") == 0) {
        return NULL;
    }
    return snapshot->git_head_state;
}

const char *cbm_userconfig_snapshot_git_history_sha256(const cbm_userconfig_snapshot_t *snapshot) {
    return snapshot && snapshot->git_history_tracked ? snapshot->git_history_sha256 : NULL;
}

int cbm_userconfig_snapshot_set_git_context(cbm_userconfig_snapshot_t *snapshot,
                                            const char *context_sha256) {
    if (!snapshot || !context_sha256 || strlen(context_sha256) != CBM_SHA256_HEX_LEN) {
        return CBM_NOT_FOUND;
    }
    memcpy(snapshot->git_context_sha256, context_sha256, CBM_SHA256_HEX_LEN + 1);
    snapshot->git_context_tracked = true;
    compute_snapshot_fingerprint(snapshot);
    return 0;
}

int cbm_userconfig_snapshot_set_discovery(cbm_userconfig_snapshot_t *snapshot,
                                          const char *discovery_sha256) {
    if (!snapshot || !discovery_sha256 || strlen(discovery_sha256) != CBM_SHA256_HEX_LEN) {
        return CBM_NOT_FOUND;
    }
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)discovery_sha256[i])) {
            return CBM_NOT_FOUND;
        }
    }
    memcpy(snapshot->discovery_sha256, discovery_sha256, CBM_SHA256_HEX_LEN + 1U);
    snapshot->discovery_tracked = true;
    compute_snapshot_fingerprint(snapshot);
    return 0;
}

const char *cbm_userconfig_snapshot_git_context_sha256(const cbm_userconfig_snapshot_t *snapshot) {
    return snapshot && snapshot->git_context_tracked ? snapshot->git_context_sha256 : NULL;
}

cbm_userconfig_aux_source_state_t cbm_userconfig_snapshot_auxiliary_source_state(
    const cbm_userconfig_snapshot_t *snapshot, const char *rel_path) {
    if (!snapshot || !snapshot->auxiliary_captured || !rel_path || rel_path[0] == '\0') {
        return CBM_USERCONFIG_AUX_SOURCE_UNKNOWN;
    }
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        const auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        if (strcmp(item->rel_path, rel_path) != 0) {
            continue;
        }
        if (item->state == USERCONFIG_SOURCE_PRESENT) {
            return CBM_USERCONFIG_AUX_SOURCE_PRESENT;
        }
        if (item->state == USERCONFIG_SOURCE_UNREADABLE) {
            return CBM_USERCONFIG_AUX_SOURCE_UNREADABLE;
        }
        return CBM_USERCONFIG_AUX_SOURCE_ABSENT;
    }
    return CBM_USERCONFIG_AUX_SOURCE_ABSENT;
}

int cbm_userconfig_snapshot_verify_auxiliary_source(cbm_userconfig_snapshot_t *snapshot,
                                                    const char *rel_path,
                                                    const char *source_sha256) {
    if (!snapshot || !snapshot->auxiliary_captured || !rel_path || !source_sha256) {
        return CBM_NOT_FOUND;
    }
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        if (strcmp(item->rel_path, rel_path) == 0) {
            if (item->state == USERCONFIG_SOURCE_PRESENT &&
                strcmp(item->sha256, source_sha256) == 0) {
                item->consumed = true;
                return 0;
            }
            snapshot->auxiliary_loader_mismatch = true;
            return CBM_NOT_FOUND;
        }
    }
    snapshot->auxiliary_loader_mismatch = true;
    return CBM_NOT_FOUND;
}

void cbm_userconfig_snapshot_note_auxiliary_read_failure(cbm_userconfig_snapshot_t *snapshot,
                                                         const char *rel_path) {
    if (!snapshot || !snapshot->auxiliary_captured || !rel_path) {
        return;
    }
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        if (strcmp(item->rel_path, rel_path) == 0) {
            if (item->state == USERCONFIG_SOURCE_PRESENT) {
                snapshot->auxiliary_loader_mismatch = true;
            } else {
                item->consumed = true;
            }
            return;
        }
    }
    snapshot->auxiliary_loader_mismatch = true;
}

void cbm_userconfig_snapshot_note_auxiliary_consumer_failure(cbm_userconfig_snapshot_t *snapshot,
                                                             cbm_userconfig_aux_kind_t kind) {
    (void)kind;
    if (snapshot) {
        snapshot->auxiliary_loader_mismatch = true;
    }
}

void cbm_userconfig_snapshot_finish_auxiliary_consumer(cbm_userconfig_snapshot_t *snapshot,
                                                       cbm_userconfig_aux_kind_t kind) {
    if (!snapshot || !snapshot->auxiliary_captured) {
        return;
    }
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        const auxiliary_source_snapshot_t *item = &snapshot->auxiliary[i];
        if (item->kind == kind && !item->consumed) {
            snapshot->auxiliary_loader_mismatch = true;
        }
    }
}

int cbm_userconfig_snapshot_verify(const cbm_userconfig_snapshot_t *snapshot) {
    if (!snapshot || snapshot->auxiliary_loader_mismatch) {
        return CBM_NOT_FOUND;
    }
    const userconfig_source_snapshot_t *expected[] = {&snapshot->global, &snapshot->project};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (!expected[i]->path) {
            if (expected[i]->state != USERCONFIG_SOURCE_ABSENT) {
                return CBM_NOT_FOUND;
            }
            continue;
        }
        userconfig_source_state_t state = USERCONFIG_SOURCE_ABSENT;
        char sha256[CBM_SHA256_HEX_LEN + 1];
        char *data = NULL;
        size_t len = 0;
        const char *root = i == 1 ? snapshot->repo_path : NULL;
        const char *relative = i == 1 ? ".codebase-memory.json" : NULL;
        if (capture_config_source(expected[i]->path, root, relative, expected[i]->max_bytes, &state,
                                  sha256, &data, &len) != 0) {
            free(data);
            return CBM_NOT_FOUND;
        }
        free(data);
        if (state != expected[i]->state ||
            (state == USERCONFIG_SOURCE_PRESENT && strcmp(sha256, expected[i]->sha256) != 0)) {
            return CBM_NOT_FOUND;
        }
    }
    if (snapshot->auxiliary_captured) {
        auxiliary_scan_t current = {0};
        if (!snapshot->auxiliary_repo_path ||
            auxiliary_scan_repo(snapshot->auxiliary_repo_path, snapshot->auxiliary_excluded_dirs,
                                snapshot->auxiliary_excluded_count, &current) != 0) {
            auxiliary_scan_free(&current);
            return CBM_NOT_FOUND;
        }
        bool matches = current.count == snapshot->auxiliary_count;
        for (int i = 0; matches && i < current.count; i++) {
            const auxiliary_source_snapshot_t *left = &current.items[i];
            const auxiliary_source_snapshot_t *right = &snapshot->auxiliary[i];
            matches = strcmp(left->rel_path, right->rel_path) == 0 && left->state == right->state &&
                      (left->state != USERCONFIG_SOURCE_PRESENT ||
                       strcmp(left->sha256, right->sha256) == 0);
        }
        auxiliary_scan_free(&current);
        if (!matches) {
            return CBM_NOT_FOUND;
        }
    }
    if (snapshot->git_head_tracked) {
        char *live_head = capture_git_head_state(snapshot->repo_path);
        bool matches = live_head && snapshot->git_head_state &&
                       strcmp(live_head, snapshot->git_head_state) == 0;
        free(live_head);
        if (!matches) {
            return CBM_NOT_FOUND;
        }
        if (snapshot->git_history_tracked && snapshot->git_head_state &&
            strcmp(snapshot->git_head_state, "not-git") != 0 &&
            strcmp(snapshot->git_head_state, "unborn") != 0) {
            char *history = NULL;
            size_t history_len = 0;
            char history_sha256[CBM_SHA256_HEX_LEN + 1];
            int history_rc = cbm_git_history_capture(snapshot->repo_path, snapshot->git_head_state,
                                                     &history, &history_len, history_sha256);
            free(history);
            if (history_rc != 0 || strcmp(history_sha256, snapshot->git_history_sha256) != 0) {
                return CBM_NOT_FOUND;
            }
        }
    }
    return 0;
}

void cbm_userconfig_snapshot_free(cbm_userconfig_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    free(snapshot->repo_path);
    free(snapshot->global.path);
    free(snapshot->project.path);
    for (int i = 0; i < snapshot->auxiliary_count; i++) {
        free(snapshot->auxiliary[i].rel_path);
    }
    free(snapshot->auxiliary);
    free(snapshot->auxiliary_repo_path);
    for (int i = 0; i < snapshot->auxiliary_excluded_count; i++) {
        free(snapshot->auxiliary_excluded_dirs[i]);
    }
    free(snapshot->auxiliary_excluded_dirs);
    free(snapshot->git_head_state);
    free(snapshot);
}

CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext) {
    if (!cfg || !ext || !ext[0]) {
        return CBM_LANG_COUNT;
    }
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].ext && strcmp(cfg->entries[i].ext, ext) == 0) {
            return cfg->entries[i].lang;
        }
    }
    return CBM_LANG_COUNT;
}

void cbm_userconfig_free(cbm_userconfig_t *cfg) {
    if (!cfg) {
        return;
    }
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].ext);
    }
    free(cfg->entries);
    free(cfg);
}
