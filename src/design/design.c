/*
 * design.c — Portable, read-only Design Context graph builder.
 *
 * Supported inputs:
 *   - DTCG 2025.10 JSON (`*.tokens.json`, `$value`, `$type`, aliases)
 *   - DTCG 2025.10 resolver metadata (`*.resolver.json`, local references only)
 *   - Google-style DESIGN.md YAML frontmatter plus Markdown guidance
 *   - CSS/SCSS custom-property definitions and var() usages
 *
 * The pass never invokes an external command and never writes repository
 * artifacts. This keeps the static-binary/runtime-dependency contract intact.
 */
#include "design/design.h"

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "pipeline/pipeline.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DESIGN_MAX_FILE_SIZE = 8 * 1024 * 1024,
    DESIGN_PATH_CAP = 1024,
    DESIGN_QN_CAP = 2048,
};

typedef struct {
    char **items;
    int count;
} design_patterns_t;

typedef struct {
    design_patterns_t documents;
    design_patterns_t token_sources;
    design_patterns_t resolvers;
    design_patterns_t authoritative;
    design_patterns_t generated;
} design_config_t;

typedef struct {
    int64_t source_id;
    char *scope;
    char *target_path;
} design_alias_t;

typedef struct {
    design_alias_t *items;
    int count;
    int cap;
} design_aliases_t;

typedef struct {
    char *path;
    char *scope;
    int64_t system_id;
} design_document_t;

typedef struct {
    design_document_t *items;
    int count;
    int cap;
} design_documents_t;

typedef struct {
    const cbm_design_index_opts_t *opts;
    design_config_t config;
    design_aliases_t aliases;
    design_documents_t documents;
    int token_count;
    int component_count;
    int mode_count;
    int document_count;
    int css_usage_count;
} design_ctx_t;

static char *design_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *copy = (char *)malloc(n + 1);
    if (copy) {
        memcpy(copy, s, n + 1);
    }
    return copy;
}

static bool design_has_suffix(const char *s, const char *suffix) {
    if (!s || !suffix) {
        return false;
    }
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static const char *design_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *back = path ? strrchr(path, '\\') : NULL;
    const char *last = slash > back ? slash : back;
    return last ? last + 1 : (path ? path : "");
}

static void design_dirname(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!path) {
        return;
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static void design_normalize_segment(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    size_t j = 0;
    bool last_dot = false;
    for (size_t i = 0; in && in[i] && j + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '_' || c == '-') {
            out[j++] = (char)c;
            last_dot = false;
        } else if (!last_dot && j > 0) {
            out[j++] = '.';
            last_dot = true;
        }
    }
    while (j > 0 && out[j - 1] == '.') {
        j--;
    }
    out[j] = '\0';
}

static void design_scope_from_dir(const char *dir, char *out, size_t out_size) {
    if (!dir || dir[0] == '\0') {
        snprintf(out, out_size, "root");
        return;
    }
    design_normalize_segment(dir, out, out_size);
    if (out[0] == '\0') {
        snprintf(out, out_size, "root");
    }
}

static void design_token_qn(const design_ctx_t *ctx, const char *scope, const char *token_path,
                            char *out, size_t out_size) {
    char normalized[DESIGN_PATH_CAP];
    design_normalize_segment(token_path, normalized, sizeof(normalized));
    snprintf(out, out_size, "%s.design.token.%s.%s", ctx->opts->project_name, scope, normalized);
}

static void design_system_qn(const design_ctx_t *ctx, const char *scope, char *out,
                             size_t out_size) {
    snprintf(out, out_size, "%s.design.system.%s", ctx->opts->project_name, scope);
}

static void design_component_qn(const design_ctx_t *ctx, const char *scope, const char *name,
                                char *out, size_t out_size) {
    char normalized[DESIGN_PATH_CAP];
    design_normalize_segment(name, normalized, sizeof(normalized));
    snprintf(out, out_size, "%s.design.component.%s.%s", ctx->opts->project_name, scope,
             normalized);
}

static void design_mode_qn(const design_ctx_t *ctx, const char *scope, const char *modifier,
                           const char *context, char *out, size_t out_size) {
    char normalized_modifier[DESIGN_PATH_CAP];
    char normalized_context[DESIGN_PATH_CAP];
    design_normalize_segment(modifier, normalized_modifier, sizeof(normalized_modifier));
    design_normalize_segment(context, normalized_context, sizeof(normalized_context));
    snprintf(out, out_size, "%s.design.mode.%s.%s.%s", ctx->opts->project_name, scope,
             normalized_modifier, normalized_context);
}

/* Small glob matcher for repository-relative configuration patterns. Supports
 * `*`, `**`, and `?`; path separators are normalized by discovery already. */
static bool design_glob_match(const char *pattern, const char *text) {
    if (!pattern || !text) {
        return false;
    }
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '*') {
        bool double_star = pattern[1] == '*';
        const char *next = pattern + (double_star ? 2 : 1);
        if (double_star && *next == '/') {
            next++;
        }
        if (design_glob_match(next, text)) {
            return true;
        }
        for (const char *p = text; *p; p++) {
            if (!double_star && *p == '/') {
                break;
            }
            if (design_glob_match(pattern, p + 1)) {
                return true;
            }
        }
        return false;
    }
    if (*pattern == '?') {
        return *text != '\0' && *text != '/' && design_glob_match(pattern + 1, text + 1);
    }
    return *pattern == *text && design_glob_match(pattern + 1, text + 1);
}

static bool design_patterns_match(const design_patterns_t *patterns, const char *path) {
    if (!patterns || patterns->count == 0) {
        return false;
    }
    for (int i = 0; i < patterns->count; i++) {
        if (design_glob_match(patterns->items[i], path)) {
            return true;
        }
    }
    return false;
}

static void design_patterns_free(design_patterns_t *patterns) {
    if (!patterns) {
        return;
    }
    for (int i = 0; i < patterns->count; i++) {
        free(patterns->items[i]);
    }
    free(patterns->items);
    patterns->items = NULL;
    patterns->count = 0;
}

static int design_patterns_load(yyjson_val *obj, const char *key, design_patterns_t *out) {
    yyjson_val *array = yyjson_obj_get(obj, key);
    if (!array || !yyjson_is_arr(array)) {
        return 0;
    }
    size_t idx, max;
    yyjson_val *value;
    yyjson_arr_foreach(array, idx, max, value) {
        const char *s = yyjson_get_str(value);
        if (!s || !s[0]) {
            continue;
        }
        char **grown = (char **)realloc(out->items, (size_t)(out->count + 1) * sizeof(char *));
        if (!grown) {
            return -1;
        }
        out->items = grown;
        out->items[out->count] = design_strdup(s);
        if (!out->items[out->count]) {
            return -1;
        }
        out->count++;
    }
    return 0;
}

static char *design_read_file(const cbm_file_info_t *file, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!file || !file->path || file->size <= 0 || file->size > DESIGN_MAX_FILE_SIZE) {
        return NULL;
    }
    FILE *f = cbm_fopen(file->path, "rb");
    if (!f) {
        return NULL;
    }
    size_t cap = (size_t)file->size;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, cap, f);
    (void)fclose(f);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static int design_load_config(design_ctx_t *ctx) {
    char path[DESIGN_PATH_CAP];
    snprintf(path, sizeof(path), "%s/.codebase-memory.json", ctx->opts->repo_path);
    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return 0;
    }
    long len = ftell(f);
    if (len <= 0 || len > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return 0;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        (void)fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    (void)fclose(f);
    buf[n] = '\0';
    yyjson_doc *doc = yyjson_read(buf, n, 0);
    free(buf);
    if (!doc) {
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *design = yyjson_is_obj(root) ? yyjson_obj_get(root, "design") : NULL;
    int rc = 0;
    if (yyjson_is_obj(design)) {
        if (design_patterns_load(design, "documents", &ctx->config.documents) != 0 ||
            design_patterns_load(design, "token_sources", &ctx->config.token_sources) != 0 ||
            design_patterns_load(design, "resolvers", &ctx->config.resolvers) != 0 ||
            design_patterns_load(design, "authoritative", &ctx->config.authoritative) != 0 ||
            design_patterns_load(design, "generated", &ctx->config.generated) != 0) {
            rc = -1;
        }
    }
    yyjson_doc_free(doc);
    return rc;
}

static const char *design_provenance(const design_ctx_t *ctx, const char *path,
                                     const char *default_value) {
    if (design_patterns_match(&ctx->config.generated, path)) {
        return "generated";
    }
    if (design_patterns_match(&ctx->config.authoritative, path)) {
        return "authoritative";
    }
    return default_value;
}

static char *design_properties(const char *format, const char *source_path, const char *scope,
                               const char *token_path, const char *type, const char *value,
                               const char *description, const char *provenance) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "source_format", format ? format : "unknown");
    yyjson_mut_obj_add_str(doc, root, "source_path", source_path ? source_path : "");
    yyjson_mut_obj_add_str(doc, root, "scope", scope ? scope : "root");
    yyjson_mut_obj_add_str(doc, root, "provenance", provenance ? provenance : "observed");
    if (token_path) {
        yyjson_mut_obj_add_str(doc, root, "token_path", token_path);
    }
    if (type && type[0]) {
        yyjson_mut_obj_add_str(doc, root, "token_type", type);
    }
    if (value) {
        yyjson_mut_obj_add_str(doc, root, "value", value);
    }
    if (description && description[0]) {
        yyjson_mut_obj_add_str(doc, root, "description", description);
    }
    yyjson_mut_obj_add_bool(doc, root, "authoritative",
                            provenance && strcmp(provenance, "authoritative") == 0);
    yyjson_mut_obj_add_bool(doc, root, "generated",
                            provenance && strcmp(provenance, "generated") == 0);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int64_t design_ensure_system(design_ctx_t *ctx, const char *scope, const char *source_path,
                                    const char *name) {
    char qn[DESIGN_QN_CAP];
    design_system_qn(ctx, scope, qn, sizeof(qn));
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (existing) {
        return existing->id;
    }
    char *props = design_properties("design-context", source_path, scope, NULL, NULL, NULL, NULL,
                                    "authoritative");
    int64_t id =
        cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignSystem", name && name[0] ? name : scope, qn,
                             source_path ? source_path : "", 1, 1, props ? props : "{}");
    free(props);
    return id;
}

static int design_alias_add(design_ctx_t *ctx, int64_t source_id, const char *scope,
                            const char *target_path) {
    if (!target_path || !target_path[0]) {
        return 0;
    }
    if (ctx->aliases.count == ctx->aliases.cap) {
        int cap = ctx->aliases.cap ? ctx->aliases.cap * 2 : 32;
        design_alias_t *grown =
            (design_alias_t *)realloc(ctx->aliases.items, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        ctx->aliases.items = grown;
        ctx->aliases.cap = cap;
    }
    design_alias_t *alias = &ctx->aliases.items[ctx->aliases.count++];
    alias->source_id = source_id;
    alias->scope = design_strdup(scope);
    alias->target_path = design_strdup(target_path);
    return alias->scope && alias->target_path ? 0 : -1;
}

static int64_t design_upsert_component(design_ctx_t *ctx, const char *scope, const char *name,
                                       const char *file_path, const char *provenance,
                                       int64_t system_id) {
    char qn[DESIGN_QN_CAP];
    design_component_qn(ctx, scope, name, qn, sizeof(qn));
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (existing) {
        return existing->id;
    }
    char *props =
        design_properties("design-component", file_path, scope, NULL, NULL, NULL, NULL, provenance);
    int64_t id = cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignComponent", name, qn, file_path, 1, 1,
                                      props ? props : "{}");
    free(props);
    if (id > 0 && system_id > 0) {
        cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, id, "PROVIDES", "{}");
    }
    if (id > 0) {
        ctx->component_count++;
    }
    return id;
}

static int64_t design_upsert_token(design_ctx_t *ctx, const char *scope, const char *token_path,
                                   const char *type, const char *value, const char *description,
                                   const char *file_path, const char *format,
                                   const char *provenance, int start_line, int64_t system_id) {
    char qn[DESIGN_QN_CAP];
    design_token_qn(ctx, scope, token_path, qn, sizeof(qn));
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    bool incoming_generated = provenance && strcmp(provenance, "generated") == 0;
    bool incoming_css = format && strcmp(format, "css") == 0;
    if (existing && (incoming_generated || incoming_css)) {
        if (incoming_generated) {
            char *file_qn =
                cbm_pipeline_fqn_compute(ctx->opts->project_name, file_path, "__file__");
            const cbm_gbuf_node_t *file_node =
                file_qn ? cbm_gbuf_find_by_qn(ctx->opts->gbuf, file_qn) : NULL;
            if (file_node) {
                cbm_gbuf_insert_edge(ctx->opts->gbuf, existing->id, file_node->id, "GENERATED_AS",
                                     "{}");
            }
            free(file_qn);
        }
        return existing->id;
    }
    char *props = design_properties(format, file_path, scope, token_path, type, value, description,
                                    provenance);
    const char *name = strrchr(token_path, '.');
    name = name ? name + 1 : token_path;
    int64_t id = cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignToken", name, qn, file_path,
                                      start_line, start_line, props ? props : "{}");
    free(props);
    if (id > 0 && system_id > 0) {
        cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, id, "PROVIDES", "{}");
    }
    if (!existing && id > 0) {
        ctx->token_count++;
    }
    if (value && value[0] == '{') {
        size_t n = strlen(value);
        if (n > 2 && value[n - 1] == '}') {
            char *target = (char *)malloc(n - 1);
            if (target) {
                memcpy(target, value + 1, n - 2);
                target[n - 2] = '\0';
                (void)design_alias_add(ctx, id, scope, target);
                free(target);
            }
        }
    }
    return id;
}

static char *design_json_value_string(yyjson_val *value) {
    if (!value) {
        return design_strdup("");
    }
    if (yyjson_is_str(value)) {
        return design_strdup(yyjson_get_str(value));
    }
    if (yyjson_is_null(value)) {
        return design_strdup("null");
    }
    return yyjson_val_write(value, 0, NULL);
}

static void design_join_path(const char *prefix, const char *name, char *out, size_t out_size) {
    if (!prefix || !prefix[0]) {
        snprintf(out, out_size, "%s", name ? name : "");
    } else if (!name || !name[0]) {
        snprintf(out, out_size, "%s", prefix);
    } else {
        snprintf(out, out_size, "%s.%s", prefix, name);
    }
}

/* Recursively visit a DTCG document. Group `$type` is inherited. `$root` is
 * represented at the group's path, matching DTCG's group-root semantics. */
static int design_parse_dtcg_object(design_ctx_t *ctx, yyjson_val *object, const char *prefix,
                                    const char *inherited_type, const char *scope,
                                    const char *file_path, const char *provenance,
                                    int64_t system_id) {
    if (!yyjson_is_obj(object)) {
        return 0;
    }
    const char *group_type = inherited_type;
    yyjson_val *type_value = yyjson_obj_get(object, "$type");
    if (yyjson_is_str(type_value)) {
        group_type = yyjson_get_str(type_value);
    }

    yyjson_val *value = yyjson_obj_get(object, "$value");
    if (value && prefix && prefix[0]) {
        const char *token_type = group_type;
        const char *description = NULL;
        yyjson_val *description_value = yyjson_obj_get(object, "$description");
        if (yyjson_is_str(description_value)) {
            description = yyjson_get_str(description_value);
        }
        char *serialized = design_json_value_string(value);
        if (!serialized) {
            return -1;
        }
        (void)design_upsert_token(ctx, scope, prefix, token_type, serialized, description,
                                  file_path, "dtcg", provenance, 1, system_id);
        free(serialized);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(object, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *name = yyjson_get_str(key);
        yyjson_val *child = yyjson_obj_iter_get_val(key);
        if (!name || (name[0] == '$' && strcmp(name, "$root") != 0)) {
            continue;
        }
        char path[DESIGN_PATH_CAP];
        if (strcmp(name, "$root") == 0) {
            snprintf(path, sizeof(path), "%s", prefix);
        } else {
            design_join_path(prefix, name, path, sizeof(path));
        }
        if (yyjson_is_obj(child)) {
            if (design_parse_dtcg_object(ctx, child, path, group_type, scope, file_path, provenance,
                                         system_id) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int design_parse_dtcg(design_ctx_t *ctx, const cbm_file_info_t *file, const char *scope,
                             int64_t system_id) {
    size_t len = 0;
    char *source = design_read_file(file, &len);
    if (!source) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read(source, len, 0);
    free(source);
    if (!doc) {
        cbm_log_warn("design.parse_skip", "format", "dtcg", "path", file->rel_path);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *provenance = design_provenance(ctx, file->rel_path, "authoritative");
    int rc =
        design_parse_dtcg_object(ctx, root, "", NULL, scope, file->rel_path, provenance, system_id);
    yyjson_doc_free(doc);
    return rc;
}

typedef struct {
    int indent;
    char key[256];
} design_yaml_level_t;

static char *design_trim(char *s) {
    if (!s) {
        return s;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

static const char *design_google_type(const char *root_key) {
    if (!root_key) {
        return NULL;
    }
    if (strcmp(root_key, "colors") == 0) {
        return "color";
    }
    if (strcmp(root_key, "typography") == 0) {
        return "typography";
    }
    if (strcmp(root_key, "spacing") == 0 || strcmp(root_key, "rounded") == 0) {
        return "dimension";
    }
    return NULL;
}

static int design_parse_frontmatter(design_ctx_t *ctx, char *source, const cbm_file_info_t *file,
                                    const char *scope, int64_t system_id, char *system_name,
                                    size_t system_name_size) {
    if (!source || strncmp(source, "---", 3) != 0 ||
        (source[3] != '\n' && !(source[3] == '\r' && source[4] == '\n'))) {
        return 0;
    }
    char *cursor = strchr(source, '\n');
    if (!cursor) {
        return 0;
    }
    cursor++;
    design_yaml_level_t levels[32];
    int level_count = 0;
    const char *provenance = design_provenance(ctx, file->rel_path, "authoritative");
    int line_no = 2;

    while (*cursor) {
        char *line_end = strchr(cursor, '\n');
        if (line_end) {
            *line_end = '\0';
        }
        char *line = cursor;
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }
        if (strcmp(line, "---") == 0) {
            break;
        }
        int indent = 0;
        while (line[indent] == ' ') {
            indent++;
        }
        char *content = line + indent;
        if (*content && *content != '#') {
            char *colon = strchr(content, ':');
            if (colon) {
                *colon = '\0';
                char *key = design_trim(content);
                char *value = design_trim(colon + 1);
                while (level_count > 0 && levels[level_count - 1].indent >= indent) {
                    level_count--;
                }
                if (value[0] == '\0') {
                    if (level_count < (int)(sizeof(levels) / sizeof(levels[0]))) {
                        levels[level_count].indent = indent;
                        snprintf(levels[level_count].key, sizeof(levels[level_count].key), "%s",
                                 key);
                        level_count++;
                    }
                } else if (level_count == 0) {
                    if (strcmp(key, "name") == 0) {
                        snprintf(system_name, system_name_size, "%s", value);
                    }
                } else {
                    char token_path[DESIGN_PATH_CAP] = {0};
                    for (int i = 0; i < level_count; i++) {
                        char joined[DESIGN_PATH_CAP];
                        design_join_path(token_path, levels[i].key, joined, sizeof(joined));
                        snprintf(token_path, sizeof(token_path), "%s", joined);
                    }
                    char joined[DESIGN_PATH_CAP];
                    design_join_path(token_path, key, joined, sizeof(joined));
                    snprintf(token_path, sizeof(token_path), "%s", joined);
                    const char *root_key = levels[0].key;
                    if (strcmp(root_key, "components") == 0 && level_count >= 2) {
                        int64_t component_id = design_upsert_component(
                            ctx, scope, levels[1].key, file->rel_path, provenance, system_id);
                        int64_t token_id = design_upsert_token(ctx, scope, token_path, NULL, value,
                                                               NULL, file->rel_path, "design-md",
                                                               provenance, line_no, system_id);
                        if (component_id > 0 && token_id > 0) {
                            cbm_gbuf_insert_edge(ctx->opts->gbuf, component_id, token_id,
                                                 "PROVIDES", "{}");
                        }
                    } else if (strcmp(root_key, "colors") == 0 ||
                               strcmp(root_key, "typography") == 0 ||
                               strcmp(root_key, "spacing") == 0 ||
                               strcmp(root_key, "rounded") == 0) {
                        (void)design_upsert_token(
                            ctx, scope, token_path, design_google_type(root_key), value, NULL,
                            file->rel_path, "design-md", provenance, line_no, system_id);
                    }
                }
            }
        }
        if (!line_end) {
            break;
        }
        cursor = line_end + 1;
        line_no++;
    }
    return 0;
}

static int design_documents_add(design_ctx_t *ctx, const char *path, const char *scope,
                                int64_t system_id) {
    if (ctx->documents.count == ctx->documents.cap) {
        int cap = ctx->documents.cap ? ctx->documents.cap * 2 : 8;
        design_document_t *grown =
            (design_document_t *)realloc(ctx->documents.items, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        ctx->documents.items = grown;
        ctx->documents.cap = cap;
    }
    design_document_t *doc = &ctx->documents.items[ctx->documents.count++];
    doc->path = design_strdup(path);
    doc->scope = design_strdup(scope);
    doc->system_id = system_id;
    return doc->path && doc->scope ? 0 : -1;
}

static void design_frontmatter_name(const char *source, char *out, size_t out_size) {
    if (!source || strncmp(source, "---", 3) != 0) {
        return;
    }
    const char *cursor = strchr(source, '\n');
    if (!cursor) {
        return;
    }
    cursor++;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t n = end ? (size_t)(end - cursor) : strlen(cursor);
        if (n == 3 && strncmp(cursor, "---", 3) == 0) {
            return;
        }
        if (n > 5 && strncmp(cursor, "name:", 5) == 0) {
            const char *value = cursor + 5;
            while ((size_t)(value - cursor) < n && isspace((unsigned char)*value)) {
                value++;
            }
            size_t value_len = n - (size_t)(value - cursor);
            while (value_len > 0 && isspace((unsigned char)value[value_len - 1])) {
                value_len--;
            }
            if (value_len >= 2 && ((value[0] == '"' && value[value_len - 1] == '"') ||
                                   (value[0] == '\'' && value[value_len - 1] == '\''))) {
                value++;
                value_len -= 2;
            }
            if (value_len >= out_size) {
                value_len = out_size - 1;
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return;
        }
        if (!end) {
            return;
        }
        cursor = end + 1;
    }
}

static int design_parse_document(design_ctx_t *ctx, const cbm_file_info_t *file) {
    char dir[DESIGN_PATH_CAP];
    char scope[DESIGN_PATH_CAP];
    design_dirname(file->rel_path, dir, sizeof(dir));
    design_scope_from_dir(dir, scope, sizeof(scope));
    char name[256];
    snprintf(name, sizeof(name), "%s", strcmp(scope, "root") == 0 ? "Repository Design" : scope);
    size_t len = 0;
    char *source = design_read_file(file, &len);
    design_frontmatter_name(source, name, sizeof(name));
    int64_t system_id = design_ensure_system(ctx, scope, file->rel_path, name);
    if (system_id <= 0) {
        free(source);
        return -1;
    }
    char *file_qn = cbm_pipeline_fqn_compute(ctx->opts->project_name, file->rel_path, "__file__");
    const cbm_gbuf_node_t *file_node =
        file_qn ? cbm_gbuf_find_by_qn(ctx->opts->gbuf, file_qn) : NULL;
    if (file_node) {
        cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, file_node->id, "DOCUMENTED_BY", "{}");
    }
    free(file_qn);
    if (source) {
        (void)design_parse_frontmatter(ctx, source, file, scope, system_id, name, sizeof(name));
        free(source);
    }
    if (design_documents_add(ctx, file->rel_path, scope, system_id) != 0) {
        return -1;
    }
    ctx->document_count++;
    return 0;
}

static bool design_path_in_scope(const char *path, const char *scope_dir) {
    if (!scope_dir || !scope_dir[0]) {
        return true;
    }
    size_t n = strlen(scope_dir);
    return strncmp(path, scope_dir, n) == 0 && path[n] == '/';
}

/* Nearest DESIGN.md ancestor controls prose scope. Token overrides themselves
 * remain explicit through aliases; path nesting does not silently rewrite them. */
static const design_document_t *design_nearest_document(const design_ctx_t *ctx, const char *path) {
    const design_document_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < ctx->documents.count; i++) {
        char dir[DESIGN_PATH_CAP];
        design_dirname(ctx->documents.items[i].path, dir, sizeof(dir));
        if (design_path_in_scope(path, dir) && strlen(dir) >= best_len) {
            best = &ctx->documents.items[i];
            best_len = strlen(dir);
        }
    }
    return best;
}

static void design_scope_for_file(design_ctx_t *ctx, const char *path, char *scope,
                                  size_t scope_size, int64_t *system_id) {
    const design_document_t *document = design_nearest_document(ctx, path);
    if (document) {
        snprintf(scope, scope_size, "%s", document->scope);
        *system_id = document->system_id;
        return;
    }
    snprintf(scope, scope_size, "root");
    *system_id = design_ensure_system(ctx, scope, path, "Repository Design");
}

typedef struct {
    design_ctx_t *ctx;
    int64_t mode_id;
    const char *source_path;
    int order;
} design_mode_link_ctx_t;

static void design_link_mode_token_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    design_mode_link_ctx_t *link = (design_mode_link_ctx_t *)userdata;
    if (!node || !node->label || strcmp(node->label, "DesignToken") != 0 || !node->file_path ||
        strcmp(node->file_path, link->source_path) != 0) {
        return;
    }
    char props[64];
    snprintf(props, sizeof(props), "{\"source_order\":%d}", link->order);
    cbm_gbuf_insert_edge(link->ctx->opts->gbuf, link->mode_id, node->id, "OVERRIDES", props);
}

/* Resolve only repository-local references. Resolver documents may legally
 * mention remote sources, but indexing must never turn repository content into
 * an implicit network request. Parent traversal is rejected as well. */
static bool design_resolver_local_path(const char *resolver_path, const char *ref, char *out,
                                       size_t out_size) {
    if (!resolver_path || !ref || !ref[0] || ref[0] == '#' || ref[0] == '/' || ref[0] == '\\' ||
        strchr(ref, ':') || strchr(ref, '\\')) {
        return false;
    }
    size_t ref_len = strcspn(ref, "#?");
    if (ref_len == 0 || ref_len >= DESIGN_PATH_CAP) {
        return false;
    }
    char relative[DESIGN_PATH_CAP];
    memcpy(relative, ref, ref_len);
    relative[ref_len] = '\0';

    char dir[DESIGN_PATH_CAP];
    design_dirname(resolver_path, dir, sizeof(dir));
    char combined[DESIGN_PATH_CAP];
    if (dir[0]) {
        snprintf(combined, sizeof(combined), "%s/%s", dir, relative);
    } else {
        snprintf(combined, sizeof(combined), "%s", relative);
    }

    char normalized[DESIGN_PATH_CAP] = {0};
    size_t used = 0;
    char *cursor = combined;
    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        char *end = cursor;
        while (*end && *end != '/') {
            end++;
        }
        size_t n = (size_t)(end - cursor);
        if (n == 0) {
            break;
        }
        if ((n == 1 && cursor[0] == '.') || (n == 2 && cursor[0] == '.' && cursor[1] == '.')) {
            if (n == 2) {
                return false;
            }
        } else {
            if (used && used + 1 < sizeof(normalized)) {
                normalized[used++] = '/';
            }
            if (used + n >= sizeof(normalized)) {
                return false;
            }
            memcpy(normalized + used, cursor, n);
            used += n;
            normalized[used] = '\0';
        }
        cursor = end;
    }
    if (!normalized[0] || strlen(normalized) >= out_size) {
        return false;
    }
    snprintf(out, out_size, "%s", normalized);
    return true;
}

static char *design_mode_properties(yyjson_val *modifier, yyjson_val *sources,
                                    const char *resolver_path, const char *scope,
                                    const char *modifier_name, const char *context_name) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "source_format", "dtcg-resolver");
    yyjson_mut_obj_add_str(doc, root, "source_path", resolver_path);
    yyjson_mut_obj_add_str(doc, root, "scope", scope);
    yyjson_mut_obj_add_str(doc, root, "provenance", "authoritative");
    yyjson_mut_obj_add_str(doc, root, "modifier", modifier_name);
    yyjson_mut_obj_add_str(doc, root, "context", context_name);
    yyjson_mut_obj_add_str(doc, root, "resolver_version", "2025.10");
    yyjson_val *description = yyjson_obj_get(modifier, "description");
    if (yyjson_is_str(description)) {
        yyjson_mut_obj_add_str(doc, root, "description", yyjson_get_str(description));
    }
    yyjson_val *default_value = yyjson_obj_get(modifier, "default");
    if (yyjson_is_str(default_value)) {
        yyjson_mut_obj_add_bool(doc, root, "default",
                                strcmp(yyjson_get_str(default_value), context_name) == 0);
    }
    yyjson_mut_val *refs = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "sources", refs);
    if (yyjson_is_arr(sources)) {
        size_t idx, max;
        yyjson_val *source;
        yyjson_arr_foreach(sources, idx, max, source) {
            yyjson_val *ref = yyjson_is_obj(source) ? yyjson_obj_get(source, "$ref") : NULL;
            if (yyjson_is_str(ref)) {
                yyjson_mut_arr_add_str(doc, refs, yyjson_get_str(ref));
            }
        }
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int design_parse_resolver(design_ctx_t *ctx, const cbm_file_info_t *file) {
    size_t len = 0;
    char *source = design_read_file(file, &len);
    if (!source) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read(source, len, 0);
    free(source);
    if (!doc) {
        cbm_log_warn("design.parse_skip", "format", "dtcg-resolver", "path", file->rel_path);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *version = yyjson_is_obj(root) ? yyjson_obj_get(root, "version") : NULL;
    yyjson_val *modifiers = yyjson_is_obj(root) ? yyjson_obj_get(root, "modifiers") : NULL;
    if (!yyjson_is_str(version) || strcmp(yyjson_get_str(version), "2025.10") != 0 ||
        !yyjson_is_obj(modifiers)) {
        yyjson_doc_free(doc);
        return 0;
    }
    char scope[DESIGN_PATH_CAP];
    int64_t system_id = 0;
    design_scope_for_file(ctx, file->rel_path, scope, sizeof(scope), &system_id);

    yyjson_obj_iter modifier_iter;
    yyjson_obj_iter_init(modifiers, &modifier_iter);
    yyjson_val *modifier_key;
    while ((modifier_key = yyjson_obj_iter_next(&modifier_iter)) != NULL) {
        const char *modifier_name = yyjson_get_str(modifier_key);
        yyjson_val *modifier = yyjson_obj_iter_get_val(modifier_key);
        yyjson_val *contexts =
            yyjson_is_obj(modifier) ? yyjson_obj_get(modifier, "contexts") : NULL;
        if (!modifier_name || !yyjson_is_obj(contexts)) {
            continue;
        }
        yyjson_obj_iter context_iter;
        yyjson_obj_iter_init(contexts, &context_iter);
        yyjson_val *context_key;
        while ((context_key = yyjson_obj_iter_next(&context_iter)) != NULL) {
            const char *context_name = yyjson_get_str(context_key);
            yyjson_val *sources = yyjson_obj_iter_get_val(context_key);
            if (!context_name || !yyjson_is_arr(sources)) {
                continue;
            }
            char qn[DESIGN_QN_CAP];
            design_mode_qn(ctx, scope, modifier_name, context_name, qn, sizeof(qn));
            char display_name[512];
            snprintf(display_name, sizeof(display_name), "%s: %s", modifier_name, context_name);
            char *props = design_mode_properties(modifier, sources, file->rel_path, scope,
                                                 modifier_name, context_name);
            int64_t mode_id = cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignMode", display_name, qn,
                                                   file->rel_path, 1, 1, props ? props : "{}");
            free(props);
            if (mode_id <= 0) {
                continue;
            }
            cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, mode_id, "PROVIDES", "{}");
            ctx->mode_count++;
            size_t idx, max;
            yyjson_val *source_value;
            yyjson_arr_foreach(sources, idx, max, source_value) {
                yyjson_val *ref =
                    yyjson_is_obj(source_value) ? yyjson_obj_get(source_value, "$ref") : NULL;
                const char *ref_text = yyjson_is_str(ref) ? yyjson_get_str(ref) : NULL;
                char source_path[DESIGN_PATH_CAP];
                if (!design_resolver_local_path(file->rel_path, ref_text, source_path,
                                                sizeof(source_path))) {
                    continue;
                }
                design_mode_link_ctx_t link = {
                    .ctx = ctx, .mode_id = mode_id, .source_path = source_path, .order = (int)idx};
                cbm_gbuf_foreach_node(ctx->opts->gbuf, design_link_mode_token_visitor, &link);
            }
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

static const char *design_infer_css_type(const char *name, const char *value) {
    if ((name && strstr(name, "color")) ||
        (value && (value[0] == '#' || strstr(value, "rgb(") || strstr(value, "hsl(") ||
                   strstr(value, "oklch(")))) {
        return "color";
    }
    if (name &&
        (strstr(name, "font") || strstr(name, "line-height") || strstr(name, "letter-spacing"))) {
        return "typography";
    }
    if (value && (strstr(value, "px") || strstr(value, "rem") || strstr(value, "em"))) {
        return "dimension";
    }
    if (name && (strstr(name, "duration") || strstr(name, "ease"))) {
        return "duration";
    }
    return NULL;
}

static void design_css_name_to_path(const char *name, char *out, size_t out_size) {
    while (name && *name == '-') {
        name++;
    }
    size_t j = 0;
    bool last_dot = false;
    for (size_t i = 0; name && name[i] && j + 1 < out_size; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            out[j++] = (char)c;
            last_dot = false;
        } else if (!last_dot && j > 0) {
            out[j++] = '.';
            last_dot = true;
        }
    }
    while (j > 0 && out[j - 1] == '.') {
        j--;
    }
    out[j] = '\0';
}

static int design_line_number(const char *start, const char *position) {
    int line = 1;
    for (const char *p = start; p && p < position; p++) {
        if (*p == '\n') {
            line++;
        }
    }
    return line;
}

static int64_t design_find_css_token(design_ctx_t *ctx, const char *scope, const char *css_name) {
    char path[DESIGN_PATH_CAP];
    char qn[DESIGN_QN_CAP];
    design_css_name_to_path(css_name, path, sizeof(path));
    design_token_qn(ctx, scope, path, qn, sizeof(qn));
    const cbm_gbuf_node_t *node = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (!node && strcmp(scope, "root") != 0) {
        design_token_qn(ctx, "root", path, qn, sizeof(qn));
        node = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    }
    return node ? node->id : 0;
}

static int design_parse_css(design_ctx_t *ctx, const cbm_file_info_t *file, bool definitions,
                            bool usages) {
    size_t len = 0;
    char *source = design_read_file(file, &len);
    if (!source) {
        return 0;
    }
    char scope[DESIGN_PATH_CAP];
    int64_t system_id = 0;
    design_scope_for_file(ctx, file->rel_path, scope, sizeof(scope), &system_id);
    const char *provenance = design_provenance(ctx, file->rel_path, "observed");

    char *file_qn = cbm_pipeline_fqn_compute(ctx->opts->project_name, file->rel_path, "__file__");
    const cbm_gbuf_node_t *file_node =
        file_qn ? cbm_gbuf_find_by_qn(ctx->opts->gbuf, file_qn) : NULL;

    for (char *p = source; *p; p++) {
        if (p[0] == '/' && p[1] == '*') {
            char *end = strstr(p + 2, "*/");
            if (!end) {
                break;
            }
            p = end + 1;
            continue;
        }
        if (definitions && p[0] == '-' && p[1] == '-' &&
            (p == source || !(isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_'))) {
            char *name_end = p + 2;
            while (isalnum((unsigned char)*name_end) || *name_end == '-' || *name_end == '_') {
                name_end++;
            }
            char *scan = name_end;
            while (*scan && isspace((unsigned char)*scan)) {
                scan++;
            }
            if (*scan == ':') {
                char name[256];
                size_t name_len = (size_t)(name_end - p);
                if (name_len >= sizeof(name)) {
                    name_len = sizeof(name) - 1;
                }
                memcpy(name, p, name_len);
                name[name_len] = '\0';
                char *value_start = scan + 1;
                while (*value_start && isspace((unsigned char)*value_start)) {
                    value_start++;
                }
                char *value_end = value_start;
                int paren_depth = 0;
                while (*value_end) {
                    if (*value_end == '(') {
                        paren_depth++;
                    } else if (*value_end == ')' && paren_depth > 0) {
                        paren_depth--;
                    } else if (*value_end == ';' && paren_depth == 0) {
                        break;
                    }
                    value_end++;
                }
                char value[1024];
                size_t value_len = (size_t)(value_end - value_start);
                if (value_len >= sizeof(value)) {
                    value_len = sizeof(value) - 1;
                }
                memcpy(value, value_start, value_len);
                value[value_len] = '\0';
                char *trimmed = design_trim(value);
                char token_path[DESIGN_PATH_CAP];
                design_css_name_to_path(name, token_path, sizeof(token_path));
                (void)design_upsert_token(
                    ctx, scope, token_path, design_infer_css_type(name, trimmed), trimmed, NULL,
                    file->rel_path, "css", provenance, design_line_number(source, p), system_id);
                p = value_end;
                continue;
            }
        }
        if (usages && strncmp(p, "var(", 4) == 0) {
            char *name = p + 4;
            while (*name && isspace((unsigned char)*name)) {
                name++;
            }
            if (name[0] == '-' && name[1] == '-') {
                char *end = name + 2;
                while (isalnum((unsigned char)*end) || *end == '-' || *end == '_') {
                    end++;
                }
                char css_name[256];
                size_t n = (size_t)(end - name);
                if (n >= sizeof(css_name)) {
                    n = sizeof(css_name) - 1;
                }
                memcpy(css_name, name, n);
                css_name[n] = '\0';
                int64_t token_id = design_find_css_token(ctx, scope, css_name);
                if (file_node && token_id > 0) {
                    char props[128];
                    snprintf(props, sizeof(props), "{\"line\":%d}", design_line_number(source, p));
                    cbm_gbuf_insert_edge(ctx->opts->gbuf, file_node->id, token_id, "USES_TOKEN",
                                         props);
                    ctx->css_usage_count++;
                }
                p = end - 1;
            }
        }
    }
    free(file_qn);
    free(source);
    return 0;
}

typedef struct {
    design_ctx_t *ctx;
} design_section_link_ctx_t;

static void design_link_section_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    design_section_link_ctx_t *link = (design_section_link_ctx_t *)userdata;
    if (!node || !node->label || strcmp(node->label, "Section") != 0 || !node->file_path) {
        return;
    }
    for (int i = 0; i < link->ctx->documents.count; i++) {
        if (strcmp(node->file_path, link->ctx->documents.items[i].path) == 0) {
            cbm_gbuf_insert_edge(link->ctx->opts->gbuf, link->ctx->documents.items[i].system_id,
                                 node->id, "GUIDED_BY", "{}");
        }
    }
}

static void design_resolve_aliases(design_ctx_t *ctx) {
    for (int i = 0; i < ctx->aliases.count; i++) {
        design_alias_t *alias = &ctx->aliases.items[i];
        char qn[DESIGN_QN_CAP];
        design_token_qn(ctx, alias->scope, alias->target_path, qn, sizeof(qn));
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
        if (!target && strcmp(alias->scope, "root") != 0) {
            design_token_qn(ctx, "root", alias->target_path, qn, sizeof(qn));
            target = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
        }
        if (target) {
            cbm_gbuf_insert_edge(ctx->opts->gbuf, alias->source_id, target->id, "ALIASES_TO", "{}");
        }
    }
}

static bool design_is_document(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.documents.count > 0) {
        return design_patterns_match(&ctx->config.documents, path);
    }
    return strcmp(design_basename(path), "DESIGN.md") == 0;
}

static bool design_is_token_source(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.token_sources.count > 0) {
        return design_patterns_match(&ctx->config.token_sources, path);
    }
    return design_has_suffix(path, ".tokens.json");
}

static bool design_is_resolver(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.resolvers.count > 0) {
        return design_patterns_match(&ctx->config.resolvers, path);
    }
    return design_has_suffix(path, ".resolver.json");
}

static void design_ctx_free(design_ctx_t *ctx) {
    design_patterns_free(&ctx->config.documents);
    design_patterns_free(&ctx->config.token_sources);
    design_patterns_free(&ctx->config.resolvers);
    design_patterns_free(&ctx->config.authoritative);
    design_patterns_free(&ctx->config.generated);
    for (int i = 0; i < ctx->aliases.count; i++) {
        free(ctx->aliases.items[i].scope);
        free(ctx->aliases.items[i].target_path);
    }
    free(ctx->aliases.items);
    for (int i = 0; i < ctx->documents.count; i++) {
        free(ctx->documents.items[i].path);
        free(ctx->documents.items[i].scope);
    }
    free(ctx->documents.items);
}

int cbm_design_index(const cbm_design_index_opts_t *opts) {
    if (!opts || !opts->project_name || !opts->repo_path || !opts->gbuf ||
        (!opts->files && opts->file_count > 0) || opts->file_count < 0) {
        return -1;
    }
    design_ctx_t ctx = {.opts = opts};
    if (design_load_config(&ctx) != 0) {
        design_ctx_free(&ctx);
        return -1;
    }

    /* Documents first: nested scope inheritance for every later artifact. */
    for (int i = 0; i < opts->file_count; i++) {
        if (design_is_document(&ctx, opts->files[i].rel_path) &&
            design_parse_document(&ctx, &opts->files[i]) != 0) {
            design_ctx_free(&ctx);
            return -1;
        }
    }

    /* Machine-readable token sources are authoritative by default. */
    for (int i = 0; i < opts->file_count; i++) {
        const cbm_file_info_t *file = &opts->files[i];
        if (!design_is_token_source(&ctx, file->rel_path)) {
            continue;
        }
        char scope[DESIGN_PATH_CAP];
        int64_t system_id = 0;
        design_scope_for_file(&ctx, file->rel_path, scope, sizeof(scope), &system_id);
        if (design_parse_dtcg(&ctx, file, scope, system_id) != 0) {
            design_ctx_free(&ctx);
            return -1;
        }
    }

    /* CSS definitions precede usages across the whole repository so file
     * ordering never affects cross-file var() resolution. */
    for (int i = 0; i < opts->file_count; i++) {
        const char *path = opts->files[i].rel_path;
        if (design_has_suffix(path, ".css") || design_has_suffix(path, ".scss")) {
            (void)design_parse_css(&ctx, &opts->files[i], true, false);
        }
    }
    for (int i = 0; i < opts->file_count; i++) {
        const char *path = opts->files[i].rel_path;
        if (design_has_suffix(path, ".css") || design_has_suffix(path, ".scss")) {
            (void)design_parse_css(&ctx, &opts->files[i], false, true);
        }
    }
    /* Resolver metadata is indexed after token sources so mode-to-token
     * override edges can be connected without executing the resolver. */
    for (int i = 0; i < opts->file_count; i++) {
        if (design_is_resolver(&ctx, opts->files[i].rel_path)) {
            (void)design_parse_resolver(&ctx, &opts->files[i]);
        }
    }
    design_resolve_aliases(&ctx);
    design_section_link_ctx_t link = {.ctx = &ctx};
    cbm_gbuf_foreach_node(opts->gbuf, design_link_section_visitor, &link);

    if (ctx.document_count > 0 || ctx.token_count > 0) {
        char documents[32];
        char tokens[32];
        char components[32];
        char modes[32];
        char usages[32];
        snprintf(documents, sizeof(documents), "%d", ctx.document_count);
        snprintf(tokens, sizeof(tokens), "%d", ctx.token_count);
        snprintf(components, sizeof(components), "%d", ctx.component_count);
        snprintf(modes, sizeof(modes), "%d", ctx.mode_count);
        snprintf(usages, sizeof(usages), "%d", ctx.css_usage_count);
        cbm_log_info("pass.done", "pass", "design_context", "documents", documents, "tokens",
                     tokens, "components", components, "modes", modes, "usages", usages);
    }
    design_ctx_free(&ctx);
    return 0;
}
