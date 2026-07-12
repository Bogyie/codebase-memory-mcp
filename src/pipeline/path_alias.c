/*
 * path_alias.c — Resolve build-tool path aliases.
 *
 * Builds a directory-scoped collection of alias maps from per-language
 * config files (currently tsconfig.json / jsconfig.json) so the import
 * resolver can turn "@/lib/auth"-style imports into repo-relative paths.
 *
 * Design notes:
 *   - Public types and functions are language-agnostic. Adding a Vite /
 *     Webpack / Python loader means writing a new load_*_file() helper
 *     and registering it in find_alias_files. The resolver, the
 *     collection, and the pipeline integration do not change.
 *   - Sorting uses qsort (n log n). The bubble-sorts that the original
 *     Layer 1b draft used were O(n^2); with up to 256 alias entries
 *     and 256 scoped maps per repo, qsort is the right ceiling.
 *   - The repo walk caps recursion depth and total file count and emits
 *     a warning when either cap fires, so silent truncation on
 *     pathological monorepos shows up in the index log.
 */

#include "pipeline/path_alias.h"

#include "pipeline/pipeline_internal.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "foundation/rooted_file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson/yyjson.h>

/* Resource ceilings. Chosen to comfortably cover real-world monorepos
 * (Next.js Skyline, large nx workspaces) while bounding worst-case
 * memory and walk time. Cap hits are logged. */
enum {
    CBM_PATH_ALIAS_MAX_ENTRIES = 256, /* per single config file       */
    CBM_PATH_ALIAS_MAX_FILES = 256,   /* config files per repo walk   */
    CBM_PATH_ALIAS_MAX_FILE_BYTES = 64 * 1024,
    CBM_PATH_ALIAS_MAX_DEPTH = 32, /* directory recursion depth    */
};

/* ── Helpers ───────────────────────────────────────────────────── */

/* Strip .ts/.tsx/.js/.jsx in place. Returns its argument. */
static char *strip_resolved_ext(char *path) {
    if (!path) {
        return path;
    }
    size_t len = strlen(path);
    if (len > 3 && path[len - 3] == '.' && (path[len - 2] == 't' || path[len - 2] == 'j') &&
        path[len - 1] == 's') {
        path[len - 3] = '\0';
        return path;
    }
    if (len > 4 && path[len - 4] == '.' && (path[len - 3] == 't' || path[len - 3] == 'j') &&
        path[len - 2] == 's' && path[len - 1] == 'x') {
        path[len - 4] = '\0';
    }
    return path;
}

/* Join dir_prefix with target, collapsing "." and ".." segments so aliases
 * that climb out of their tsconfig's directory (the common monorepo
 * pattern: a tsconfig at apps/web/tsconfig.json pointing an alias at a
 * wildcard target like "../../packages/shared/src/" + wildcard) resolve
 * to a real repo-relative path. Naive concatenation left literal ".."
 * components in the target, which never match a module's FQN since
 * cbm_pipeline_fqn_module tokenizes on '/' without collapsing them
 * (#730). A trailing '/' on target (the usual case right before a
 * wildcard) is preserved so the caller's later wildcard-substring
 * concat still lines up. Returns heap-allocated
 * repo-relative target. */
static char *resolve_target_relative(const char *dir_prefix, const char *target) {
    if (!target) {
        return NULL;
    }
    size_t dp_len = (dir_prefix && dir_prefix[0] != '\0') ? strlen(dir_prefix) : 0;
    size_t t_len = strlen(target);
    char *buf = malloc(dp_len + t_len + 2);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    if (dp_len > 0) {
        memcpy(buf, dir_prefix, dp_len);
        buf[dp_len] = '\0';
    }

    bool trailing_slash = t_len > 0 && target[t_len - 1] == '/';

    const char *p = target;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *seg_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 1 && seg_start[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            char *last = strrchr(buf, '/');
            if (last) {
                *last = '\0';
            } else {
                buf[0] = '\0';
            }
            continue;
        }
        size_t cur = strlen(buf);
        if (cur > 0) {
            buf[cur++] = '/';
        }
        memcpy(buf + cur, seg_start, seg_len);
        buf[cur + seg_len] = '\0';
    }

    if (trailing_slash) {
        size_t cur = strlen(buf);
        buf[cur] = '/';
        buf[cur + 1] = '\0';
    }
    return buf;
}

/* qsort comparator: alias entries by alias_prefix length, descending. */
static int cmp_alias_entry_by_specificity(const void *a, const void *b) {
    const cbm_path_alias_t *ea = a;
    const cbm_path_alias_t *eb = b;
    size_t la = strlen(ea->alias_prefix);
    size_t lb = strlen(eb->alias_prefix);
    if (lb > la) {
        return 1;
    }
    if (lb < la) {
        return -1;
    }
    return 0;
}

/* qsort comparator: scopes by dir_prefix length, descending. */
static int cmp_scope_by_specificity(const void *a, const void *b) {
    const cbm_path_alias_scope_t *sa = a;
    const cbm_path_alias_scope_t *sb = b;
    size_t la = strlen(sa->dir_prefix);
    size_t lb = strlen(sb->dir_prefix);
    if (lb > la) {
        return 1;
    }
    if (lb < la) {
        return -1;
    }
    return 0;
}

static void free_alias_map(cbm_path_alias_map_t *map) {
    if (!map) {
        return;
    }
    for (int i = 0; i < map->count; i++) {
        free(map->entries[i].alias_prefix);
        free(map->entries[i].alias_suffix);
        free(map->entries[i].target_prefix);
        free(map->entries[i].target_suffix);
    }
    free(map->entries);
    free(map->base_url);
    free(map);
}

/* ── tsconfig.json / jsconfig.json loader ──────────────────────── */

/* Parse compilerOptions.paths and compilerOptions.baseUrl into an alias map.
 * dir_prefix is the directory of the config file relative to the repo root
 * (e.g. "apps/manager", or "" for repo root). Returns NULL if the file is
 * missing, malformed, or has neither a usable paths block nor a baseUrl. */
static cbm_path_alias_map_t *load_tsconfig_file(const char *repo_path, const char *abs_path,
                                                const char *config_rel_path, const char *dir_prefix,
                                                cbm_userconfig_snapshot_t *input_snapshot) {
    cbm_rooted_file_t source = {0};
    if (cbm_rooted_file_read(repo_path, config_rel_path, CBM_PATH_ALIAS_MAX_FILE_BYTES, &source) !=
        CBM_ROOTED_FILE_OK) {
        cbm_userconfig_snapshot_note_auxiliary_read_failure(input_snapshot, config_rel_path);
        cbm_rooted_file_free(&source);
        return NULL;
    }
    if (input_snapshot && cbm_userconfig_snapshot_verify_auxiliary_source(
                              input_snapshot, config_rel_path, source.sha256) != 0) {
        cbm_rooted_file_free(&source);
        return NULL;
    }
    if (source.len == 0) {
        cbm_rooted_file_free(&source);
        return NULL;
    }

    yyjson_read_flag flg = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;
    yyjson_doc *doc = yyjson_read(source.data, source.len, flg);
    cbm_rooted_file_free(&source);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *compiler_opts = yyjson_obj_get(root, "compilerOptions");
    if (!compiler_opts) {
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_val *base_url_val = yyjson_obj_get(compiler_opts, "baseUrl");
    const char *base_url_str = base_url_val ? yyjson_get_str(base_url_val) : NULL;
    yyjson_val *paths_obj = yyjson_obj_get(compiler_opts, "paths");
    if (!paths_obj && !base_url_str) {
        yyjson_doc_free(doc);
        return NULL;
    }

    cbm_path_alias_map_t *map = calloc(1, sizeof(*map));
    if (!map) {
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        yyjson_doc_free(doc);
        return NULL;
    }

    if (base_url_str && base_url_str[0] != '\0' && strcmp(base_url_str, ".") != 0) {
        map->base_url = resolve_target_relative(dir_prefix, base_url_str);
    } else if (base_url_str && strcmp(base_url_str, ".") == 0 && dir_prefix &&
               dir_prefix[0] != '\0') {
        map->base_url = strdup(dir_prefix);
    }
    if (base_url_str && base_url_str[0] != '\0' &&
        !(strcmp(base_url_str, ".") == 0 && (!dir_prefix || dir_prefix[0] == '\0')) &&
        !map->base_url) {
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        free_alias_map(map);
        yyjson_doc_free(doc);
        return NULL;
    }

    if (paths_obj && yyjson_is_obj(paths_obj)) {
        size_t obj_size = yyjson_obj_size(paths_obj);
        bool capped = obj_size > CBM_PATH_ALIAS_MAX_ENTRIES;
        if (capped) {
            cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                    CBM_USERCONFIG_AUX_PATH_ALIAS);
            free_alias_map(map);
            yyjson_doc_free(doc);
            return NULL;
        }
        int capacity = (int)obj_size;
        if (capacity > 0) {
            map->entries = calloc((size_t)capacity, sizeof(cbm_path_alias_t));
            if (!map->entries) {
                cbm_userconfig_snapshot_note_auxiliary_consumer_failure(
                    input_snapshot, CBM_USERCONFIG_AUX_PATH_ALIAS);
                free_alias_map(map);
                yyjson_doc_free(doc);
                return NULL;
            }
            yyjson_val *key;
            yyjson_obj_iter iter = yyjson_obj_iter_with(paths_obj);
            while ((key = yyjson_obj_iter_next(&iter)) != NULL && map->count < capacity) {
                yyjson_val *val = yyjson_obj_iter_get_val(key);
                const char *alias_pattern = yyjson_get_str(key);
                if (!alias_pattern || !yyjson_is_arr(val) || yyjson_arr_size(val) == 0) {
                    continue;
                }
                const char *target_pattern = yyjson_get_str(yyjson_arr_get_first(val));
                if (!target_pattern) {
                    continue;
                }
                cbm_path_alias_t *entry = &map->entries[map->count];
                const char *star = strchr(alias_pattern, '*');
                if (star) {
                    entry->has_wildcard = true;
                    entry->alias_prefix =
                        cbm_strndup(alias_pattern, (size_t)(star - alias_pattern));
                    entry->alias_suffix = strdup(star + 1);
                } else {
                    entry->has_wildcard = false;
                    entry->alias_prefix = strdup(alias_pattern);
                    entry->alias_suffix = strdup("");
                }
                const char *tstar = strchr(target_pattern, '*');
                if (tstar) {
                    char *pre = cbm_strndup(target_pattern, (size_t)(tstar - target_pattern));
                    if (!pre) {
                        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(
                            input_snapshot, CBM_USERCONFIG_AUX_PATH_ALIAS);
                        free_alias_map(map);
                        yyjson_doc_free(doc);
                        return NULL;
                    }
                    entry->target_prefix = resolve_target_relative(dir_prefix, pre);
                    free(pre);
                    entry->target_suffix = strdup(tstar + 1);
                } else {
                    entry->target_prefix = resolve_target_relative(dir_prefix, target_pattern);
                    entry->target_suffix = strdup("");
                }
                if (!entry->alias_prefix || !entry->alias_suffix || !entry->target_prefix ||
                    !entry->target_suffix) {
                    cbm_userconfig_snapshot_note_auxiliary_consumer_failure(
                        input_snapshot, CBM_USERCONFIG_AUX_PATH_ALIAS);
                    /* Include this partially populated slot in cleanup. */
                    map->count++;
                    free_alias_map(map);
                    yyjson_doc_free(doc);
                    return NULL;
                }
                map->count++;
            }
            qsort(map->entries, (size_t)map->count, sizeof(cbm_path_alias_t),
                  cmp_alias_entry_by_specificity);
        }
    }

    yyjson_doc_free(doc);
    return map;
}

/* ── Public API ────────────────────────────────────────────────── */

void cbm_path_alias_collection_free(cbm_path_alias_collection_t *coll) {
    if (!coll) {
        return;
    }
    for (int i = 0; i < coll->count; i++) {
        free(coll->scopes[i].dir_prefix);
        free_alias_map(coll->scopes[i].map);
    }
    free(coll->scopes);
    free(coll);
}

char *cbm_path_alias_resolve(const cbm_path_alias_map_t *map, const char *module_path) {
    if (!map || !module_path) {
        return NULL;
    }
    size_t mod_len = strlen(module_path);

    for (int i = 0; i < map->count; i++) {
        const cbm_path_alias_t *e = &map->entries[i];

        if (e->has_wildcard) {
            size_t prefix_len = strlen(e->alias_prefix);
            size_t suffix_len = strlen(e->alias_suffix);
            if (mod_len < prefix_len + suffix_len) {
                continue;
            }
            if (strncmp(module_path, e->alias_prefix, prefix_len) != 0) {
                continue;
            }
            if (suffix_len > 0 &&
                strcmp(module_path + mod_len - suffix_len, e->alias_suffix) != 0) {
                continue;
            }
            size_t wild_len = mod_len - prefix_len - suffix_len;
            const char *wild_start = module_path + prefix_len;
            size_t tp_len = strlen(e->target_prefix);
            size_t ts_len = strlen(e->target_suffix);
            char *result = malloc(tp_len + wild_len + ts_len + 1);
            if (!result) {
                return NULL;
            }
            memcpy(result, e->target_prefix, tp_len);
            memcpy(result + tp_len, wild_start, wild_len);
            memcpy(result + tp_len + wild_len, e->target_suffix, ts_len);
            result[tp_len + wild_len + ts_len] = '\0';
            return strip_resolved_ext(result);
        }

        if (strcmp(module_path, e->alias_prefix) == 0) {
            return strip_resolved_ext(strdup(e->target_prefix));
        }
    }

    /* baseUrl fallback. Apply only to non-relative imports that look
     * sub-path-ish (contain '/' but don't start with '.' or '@'); skips
     * obvious package names like "react" or "lodash". */
    if (map->base_url && module_path[0] != '.' && module_path[0] != '@' &&
        strchr(module_path, '/') != NULL) {
        size_t bu_len = strlen(map->base_url);
        size_t need = bu_len + 1 + mod_len + 1;
        char *result = malloc(need);
        if (!result) {
            return NULL;
        }
        snprintf(result, need, "%s/%s", map->base_url, module_path);
        return strip_resolved_ext(result);
    }
    return NULL;
}

/* ── Repo walk ─────────────────────────────────────────────────── */

typedef struct {
    char abs[CBM_SZ_4K];
    char rel[CBM_SZ_4K];
    char config_rel[CBM_SZ_4K];
} alias_config_hit_t;

static const char *const TS_CONFIG_NAMES[] = {"tsconfig.json", "jsconfig.json"};
enum { TS_CONFIG_NAMES_COUNT = 2 };

static int find_alias_files(const char *repo_path, const char *abs_dir, const char *rel_dir,
                            alias_config_hit_t *out, int *count, int max_count, int depth,
                            char **excluded_dirs, int excluded_count) {
    if (depth > CBM_PATH_ALIAS_MAX_DEPTH) {
        return CBM_NOT_FOUND;
    }
    cbm_dir_t *d = cbm_opendir(abs_dir);
    if (!d) {
        return CBM_NOT_FOUND;
    }

    /* One config file per directory: prefer tsconfig.json over jsconfig.json. */
    for (int i = 0; i < TS_CONFIG_NAMES_COUNT; i++) {
        char check[CBM_SZ_4K];
        int check_n = snprintf(check, sizeof(check), "%s/%s", abs_dir, TS_CONFIG_NAMES[i]);
        if (check_n < 0 || check_n >= (int)sizeof(check)) {
            cbm_closedir(d);
            return CBM_NOT_FOUND;
        }
        char candidate_rel[CBM_SZ_4K];
        int candidate_n =
            rel_dir[0] ? snprintf(candidate_rel, sizeof(candidate_rel), "%s/%s", rel_dir,
                                  TS_CONFIG_NAMES[i])
                       : snprintf(candidate_rel, sizeof(candidate_rel), "%s", TS_CONFIG_NAMES[i]);
        if (candidate_n < 0 || candidate_n >= (int)sizeof(candidate_rel)) {
            cbm_closedir(d);
            return CBM_NOT_FOUND;
        }
        cbm_rooted_file_t probe = {0};
        cbm_rooted_file_status_t probe_rc =
            cbm_rooted_file_read(repo_path, candidate_rel, CBM_PATH_ALIAS_MAX_FILE_BYTES, &probe);
        cbm_rooted_file_free(&probe);
        if (probe_rc == CBM_ROOTED_FILE_OK) {
            if (*count >= max_count) {
                cbm_closedir(d);
                return CBM_NOT_FOUND;
            }
            int abs_n = snprintf(out[*count].abs, sizeof(out[*count].abs), "%s", check);
            int rel_n = snprintf(out[*count].rel, sizeof(out[*count].rel), "%s", rel_dir);
            int config_n = snprintf(out[*count].config_rel, sizeof(out[*count].config_rel), "%s",
                                    candidate_rel);
            if (abs_n < 0 || abs_n >= (int)sizeof(out[*count].abs) || rel_n < 0 ||
                rel_n >= (int)sizeof(out[*count].rel) || config_n < 0 ||
                config_n >= (int)sizeof(out[*count].config_rel)) {
                cbm_closedir(d);
                return CBM_NOT_FOUND;
            }
            (*count)++;
            break;
        }
        if (probe_rc != CBM_ROOTED_FILE_NOT_FOUND || cbm_path_probe(check) != 0) {
            cbm_closedir(d);
            return CBM_NOT_FOUND;
        }
    }

    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!ent->is_dir || ent->is_reparse) {
            continue;
        }
        const char *name = ent->name;
        if (name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "dist") == 0 ||
            strcmp(name, "build") == 0 || strcmp(name, ".next") == 0 ||
            strcmp(name, "coverage") == 0 || strcmp(name, "target") == 0 /* Rust */) {
            continue;
        }
        char child_abs[CBM_SZ_4K];
        char child_rel[CBM_SZ_4K];
        int child_abs_n = snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_dir, name);
        int child_rel_n;
        if (rel_dir[0] == '\0') {
            child_rel_n = snprintf(child_rel, sizeof(child_rel), "%s", name);
        } else {
            child_rel_n = snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_dir, name);
        }
        if (child_abs_n < 0 || child_abs_n >= (int)sizeof(child_abs) || child_rel_n < 0 ||
            child_rel_n >= (int)sizeof(child_rel)) {
            cbm_closedir(d);
            return CBM_NOT_FOUND;
        }
        if (cbm_pipeline_relpath_is_excluded(child_rel, excluded_dirs, excluded_count)) {
            continue;
        }
        if (find_alias_files(repo_path, child_abs, child_rel, out, count, max_count, depth + 1,
                             excluded_dirs, excluded_count) != 0) {
            cbm_closedir(d);
            return CBM_NOT_FOUND;
        }
    }
    if (cbm_dir_had_error(d)) {
        cbm_closedir(d);
        return CBM_NOT_FOUND;
    }
    cbm_closedir(d);
    return 0;
}

cbm_path_alias_collection_t *cbm_load_path_aliases_excluded_snapshot(
    const char *repo_path, char **excluded_dirs, int excluded_count,
    cbm_userconfig_snapshot_t *input_snapshot) {
    if (!repo_path) {
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }
    alias_config_hit_t *hits = calloc(CBM_PATH_ALIAS_MAX_FILES, sizeof(*hits));
    if (!hits) {
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }
    int count = 0;
    if (find_alias_files(repo_path, repo_path, "", hits, &count, CBM_PATH_ALIAS_MAX_FILES, 0,
                         excluded_dirs, excluded_count) != 0) {
        free(hits);
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }
    if (count == 0) {
        free(hits);
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }

    cbm_path_alias_collection_t *coll = calloc(1, sizeof(*coll));
    if (!coll) {
        free(hits);
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }
    coll->scopes = calloc((size_t)count, sizeof(cbm_path_alias_scope_t));
    if (!coll->scopes) {
        free(coll);
        free(hits);
        cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                CBM_USERCONFIG_AUX_PATH_ALIAS);
        cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                          CBM_USERCONFIG_AUX_PATH_ALIAS);
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        cbm_path_alias_map_t *map = load_tsconfig_file(repo_path, hits[i].abs, hits[i].config_rel,
                                                       hits[i].rel, input_snapshot);
        if (!map) {
            continue;
        }
        char *dir_prefix = strdup(hits[i].rel);
        if (!dir_prefix) {
            free_alias_map(map);
            cbm_userconfig_snapshot_note_auxiliary_consumer_failure(input_snapshot,
                                                                    CBM_USERCONFIG_AUX_PATH_ALIAS);
            free(hits);
            cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                              CBM_USERCONFIG_AUX_PATH_ALIAS);
            cbm_path_alias_collection_free(coll);
            return NULL;
        }
        coll->scopes[coll->count].dir_prefix = dir_prefix;
        coll->scopes[coll->count].map = map;
        coll->count++;
    }
    free(hits);
    cbm_userconfig_snapshot_finish_auxiliary_consumer(input_snapshot,
                                                      CBM_USERCONFIG_AUX_PATH_ALIAS);

    if (coll->count == 0) {
        free(coll->scopes);
        free(coll);
        return NULL;
    }

    qsort(coll->scopes, (size_t)coll->count, sizeof(cbm_path_alias_scope_t),
          cmp_scope_by_specificity);
    return coll;
}

cbm_path_alias_collection_t *cbm_load_path_aliases_excluded(const char *repo_path,
                                                            char **excluded_dirs,
                                                            int excluded_count) {
    return cbm_load_path_aliases_excluded_snapshot(repo_path, excluded_dirs, excluded_count, NULL);
}

cbm_path_alias_collection_t *cbm_load_path_aliases(const char *repo_path) {
    return cbm_load_path_aliases_excluded(repo_path, NULL, 0);
}

const cbm_path_alias_map_t *cbm_path_alias_find_for_file(const cbm_path_alias_collection_t *coll,
                                                         const char *rel_path) {
    if (!coll || !rel_path) {
        return NULL;
    }
    for (int i = 0; i < coll->count; i++) {
        const char *prefix = coll->scopes[i].dir_prefix;
        size_t plen = strlen(prefix);
        if (plen == 0) {
            return coll->scopes[i].map;
        }
        if (strncmp(rel_path, prefix, plen) == 0 &&
            (rel_path[plen] == '/' || rel_path[plen] == '\0')) {
            return coll->scopes[i].map;
        }
    }
    return NULL;
}
