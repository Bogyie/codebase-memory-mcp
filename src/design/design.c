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
#include "design/design_io.h"

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/sha256.h"
#include "pipeline/pipeline.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DESIGN_PATH_CAP = 1024,
    DESIGN_QN_CAP = 2048,
    DESIGN_IDENTITY_SLUG_CAP = 256,
};

typedef struct {
    cbm_design_patterns_t documents;
    cbm_design_patterns_t token_sources;
    cbm_design_patterns_t resolvers;
    cbm_design_patterns_t authoritative;
    cbm_design_patterns_t generated;
} design_config_t;

typedef struct {
    int64_t source_id;
    char *scope;
    char *scope_qn;
    char *target_path;
    char *target_identity;
    bool target_identity_requires_digest;
} design_alias_t;

typedef struct {
    design_alias_t *items;
    int count;
    int cap;
} design_aliases_t;

typedef struct {
    char *path;
    char *dir;
    char *scope;
    char *scope_qn;
    int64_t system_id;
    const cbm_file_info_t *file;
    char *source;
} design_document_t;

typedef struct {
    design_document_t *items;
    int count;
    int cap;
} design_documents_t;

typedef struct {
    int64_t token_id;
    char *source_path;
    char *token_path;
    char *type;
    char *value;
    char *description;
    char *format;
    char *provenance;
    int start_line;
} design_token_definition_t;

typedef struct {
    char *path;
    design_token_definition_t *items;
    int count;
    int cap;
} design_token_source_t;

typedef struct {
    design_token_source_t *items;
    int count;
    int cap;
} design_token_sources_t;

typedef struct {
    const cbm_design_index_opts_t *opts;
    design_config_t config;
    design_aliases_t aliases;
    design_documents_t documents;
    design_token_sources_t token_sources;
    int token_count;
    int component_count;
    int mode_count;
    int document_count;
    int css_usage_count;
} design_ctx_t;

static int design_scope_for_file(design_ctx_t *ctx, const char *path, char *scope,
                                 size_t scope_size, char *scope_qn, size_t scope_qn_size,
                                 int64_t *system_id);
static char *design_join_path_dup(const char *prefix, const char *name);
static char *design_join_path_segment_dup(const char *prefix, const char *segment,
                                          size_t segment_len);

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

static char *design_strndup(const char *s, size_t len) {
    if (!s || len == SIZE_MAX) {
        return NULL;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
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

static char *design_dirname_dup(const char *path) {
    if (!path) {
        return design_strdup("");
    }
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
    size_t len = slash ? (size_t)(slash - path) : 0;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static bool design_normalize_segment_checked(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    size_t j = 0;
    bool last_dot = false;
    size_t i = 0;
    for (; in && in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            if (j + 1 >= out_size) {
                break;
            }
            out[j++] = (char)c;
            last_dot = false;
        } else if (c == '$') {
            if (j + 7 >= out_size) {
                break;
            }
            memcpy(out + j, "dollar-", 7);
            j += 7;
            last_dot = false;
        } else if (!last_dot && j > 0) {
            if (j + 1 >= out_size) {
                break;
            }
            out[j++] = '.';
            last_dot = true;
        }
    }
    while (j > 0 && out[j - 1] == '.') {
        j--;
    }
    out[j] = '\0';
    return !in || in[i] == '\0';
}

static bool design_identity_segment_safe(const char *segment, size_t len) {
    if (!segment || len == 0) {
        return false;
    }
    if (len == strlen("$root") && memcmp(segment, "$root", len) == 0) {
        return true; /* Preserve the established DTCG `$root` readable QN. */
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)segment[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    /* `$root` is rendered as `dollar-root`; force the literal spelling onto a
     * digested identity so those two raw segments can never alias. */
    static const char dollar_slug[] = "dollar-";
    for (size_t i = 0; i + sizeof(dollar_slug) - 1 <= len; i++) {
        if (memcmp(segment + i, dollar_slug, sizeof(dollar_slug) - 1) == 0) {
            return false;
        }
    }
    return true;
}

static char *design_identity_pointer_append(const char *prefix, const char *segment,
                                            size_t segment_len) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    if (segment_len > (SIZE_MAX - 19U) / 2U) {
        return NULL;
    }
    size_t encoded_len = segment_len * 2U;
    if (prefix_len > SIZE_MAX - encoded_len - 19U) {
        return NULL;
    }
    char *out = (char *)malloc(prefix_len + encoded_len + 19U);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    if (prefix_len > 0) {
        memcpy(out, prefix, prefix_len);
        pos = prefix_len;
    }
    int length_written = snprintf(out + pos, 19U, "/%016llx:",
                                  (unsigned long long)segment_len);
    if (length_written != 18) {
        free(out);
        return NULL;
    }
    pos += 18U;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < segment_len; i++) {
        unsigned char byte = (unsigned char)segment[i];
        out[pos++] = hex[byte >> 4];
        out[pos++] = hex[byte & 0x0f];
    }
    out[pos] = '\0';
    return out;
}

static char *design_identity_pointer_from_delimited(const char *value, char separator,
                                                    bool *requires_digest) {
    char *pointer = design_strdup("");
    if (!pointer) {
        return NULL;
    }
    if (!value || !value[0]) {
        return pointer;
    }
    const char *cursor = value;
    while (true) {
        const char *end = strchr(cursor, separator);
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (!design_identity_segment_safe(cursor, len) && requires_digest) {
            *requires_digest = true;
        }
        char *next = design_identity_pointer_append(pointer, cursor, len);
        free(pointer);
        if (!next) {
            return NULL;
        }
        pointer = next;
        if (!end) {
            return pointer;
        }
        cursor = end + 1;
    }
}

static void design_identity_hash(const char *domain, const char *canonical,
                                 char out[CBM_SHA256_HEX_LEN + 1]) {
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    size_t domain_len = strlen(domain);
    size_t canonical_len = canonical ? strlen(canonical) : 0;
    uint64_t lengths[2] = {(uint64_t)domain_len, (uint64_t)canonical_len};
    for (size_t field = 0; field < 2; field++) {
        unsigned char encoded[8];
        uint64_t value = lengths[field];
        for (int i = 7; i >= 0; i--) {
            encoded[i] = (unsigned char)(value & 0xffU);
            value >>= 8;
        }
        cbm_sha256_update(&hash, encoded, sizeof(encoded));
        cbm_sha256_update(&hash, field == 0 ? domain : canonical,
                          field == 0 ? domain_len : canonical_len);
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

static int design_qn_slug(const char *raw, const char *fallback, char *out, size_t out_size,
                          bool *truncated) {
    bool complete = design_normalize_segment_checked(raw, out, out_size);
    if (out[0] == '\0') {
        int written = snprintf(out, out_size, "%s", fallback);
        if (written < 0 || (size_t)written >= out_size) {
            return -1;
        }
    }
    if (!complete && truncated) {
        *truncated = true;
    }
    return 0;
}

static int design_scope_keys_from_dir(const char *dir, char *scope, size_t scope_size,
                                      char *scope_qn, size_t scope_qn_size) {
    const char *raw = dir && dir[0] ? dir : "root";
    bool requires_digest = false;
    char *identity = NULL;
    if (dir && dir[0]) {
        identity = design_identity_pointer_from_delimited(dir, '/', &requires_digest);
        if (strcmp(dir, "root") == 0) {
            requires_digest = true; /* `root` is reserved for repository scope. */
        }
    } else {
        identity = design_identity_pointer_append("", "root", strlen("root"));
    }
    if (!identity) {
        return -1;
    }

    bool scope_complete = design_normalize_segment_checked(raw, scope, scope_size);
    if (scope[0] == '\0') {
        int written = snprintf(scope, scope_size, "root");
        if (written < 0 || (size_t)written >= scope_size) {
            free(identity);
            return -1;
        }
    }
    if (!scope_complete) {
        requires_digest = true;
        cbm_log_warn("design.identity_slug_truncated", "kind", "scope", "path", raw);
    }

    char slug[DESIGN_IDENTITY_SLUG_CAP];
    bool slug_truncated = false;
    if (design_qn_slug(raw, "root", slug, sizeof(slug), &slug_truncated) != 0) {
        free(identity);
        return -1;
    }
    requires_digest = requires_digest || slug_truncated;
    char digest[CBM_SHA256_HEX_LEN + 1] = "";
    if (requires_digest) {
        design_identity_hash("scope", identity, digest);
    }
    int written = requires_digest ? snprintf(scope_qn, scope_qn_size, "%s.id-%s", slug, digest)
                                  : snprintf(scope_qn, scope_qn_size, "%s", slug);
    free(identity);
    if (written < 0 || (size_t)written >= scope_qn_size) {
        cbm_log_warn("design.identity_qn_overflow", "kind", "scope", "path", raw);
        return -1;
    }
    return 0;
}

static int design_entity_qn(const design_ctx_t *ctx, const char *kind, const char *scope_qn,
                            const char *readable, const char *canonical_identity,
                            bool identity_requires_digest, char *out, size_t out_size) {
    char slug[DESIGN_IDENTITY_SLUG_CAP];
    bool slug_truncated = false;
    if (design_qn_slug(readable, kind, slug, sizeof(slug), &slug_truncated) != 0) {
        return -1;
    }
    bool add_digest = identity_requires_digest || slug_truncated;
    if (slug_truncated) {
        cbm_log_warn("design.identity_slug_truncated", "kind", kind, "path",
                     readable ? readable : "");
    }
    char digest[CBM_SHA256_HEX_LEN + 1] = "";
    if (add_digest) {
        design_identity_hash(kind, canonical_identity ? canonical_identity : readable, digest);
    }
    int written = add_digest
                      ? snprintf(out, out_size, "%s.design.%s.%s.%s.id-%s",
                                 ctx->opts->project_name, kind, scope_qn, slug, digest)
                      : snprintf(out, out_size, "%s.design.%s.%s.%s", ctx->opts->project_name,
                                 kind, scope_qn, slug);
    return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static int design_token_qn(const design_ctx_t *ctx, const char *scope_qn, const char *token_path,
                           const char *canonical_identity, bool identity_requires_digest, char *out,
                           size_t out_size) {
    return design_entity_qn(ctx, "token", scope_qn, token_path, canonical_identity,
                            identity_requires_digest, out, out_size);
}

static int design_system_qn(const design_ctx_t *ctx, const char *scope_qn, char *out,
                            size_t out_size) {
    int written = snprintf(out, out_size, "%s.design.system.%s", ctx->opts->project_name, scope_qn);
    return written >= 0 && (size_t)written < out_size ? 0 : -1;
}

static int design_component_qn(const design_ctx_t *ctx, const char *scope_qn, const char *name,
                               char *out, size_t out_size) {
    bool requires_digest = !design_identity_segment_safe(name, name ? strlen(name) : 0);
    char *identity = design_identity_pointer_append("", name ? name : "", name ? strlen(name) : 0);
    int rc = identity ? design_entity_qn(ctx, "component", scope_qn, name, identity,
                                         requires_digest, out, out_size)
                      : -1;
    free(identity);
    return rc;
}

static int design_mode_qn(const design_ctx_t *ctx, const char *scope_qn, const char *modifier,
                          size_t modifier_len, const char *context, size_t context_len, char *out,
                          size_t out_size) {
    bool requires_digest = !design_identity_segment_safe(modifier, modifier_len) ||
                           !design_identity_segment_safe(context, context_len);
    char *identity = design_identity_pointer_append("", modifier ? modifier : "", modifier_len);
    char *full_identity =
        identity ? design_identity_pointer_append(identity, context ? context : "", context_len)
                 : NULL;
    free(identity);
    char *modifier_readable =
        design_join_path_segment_dup("", modifier ? modifier : "", modifier_len);
    char *readable = modifier_readable
                         ? design_join_path_segment_dup(modifier_readable, context ? context : "",
                                                        context_len)
                         : NULL;
    free(modifier_readable);
    if (!full_identity || !readable) {
        free(full_identity);
        free(readable);
        return -1;
    }
    int rc = design_entity_qn(ctx, "mode", scope_qn, readable, full_identity, requires_digest, out,
                              out_size);
    free(full_identity);
    free(readable);
    return rc;
}

static int design_load_validated_source_limited(design_ctx_t *ctx, const cbm_file_info_t *file,
                                                const char *source_kind, size_t max_bytes,
                                                char **out_source, size_t *out_len);

static int design_load_validated_source(design_ctx_t *ctx, const cbm_file_info_t *file,
                                        const char *source_kind, char **out_source,
                                        size_t *out_len) {
    return design_load_validated_source_limited(ctx, file, source_kind, 0, out_source, out_len);
}

static int design_load_validated_source_limited(design_ctx_t *ctx, const cbm_file_info_t *file,
                                                const char *source_kind, size_t max_bytes,
                                                char **out_source, size_t *out_len) {
    char source_hash[CBM_SHA256_HEX_LEN + 1];
    int read_rc = cbm_design_load_source_limited(file, source_kind, max_bytes, out_source, out_len,
                                                 source_hash);
    if (read_rc == 1 && ctx->opts->verify_source &&
        ctx->opts->verify_source(ctx->opts->verify_source_userdata, file->rel_path, source_hash) !=
            0) {
        free(*out_source);
        *out_source = NULL;
        if (out_len) {
            *out_len = 0;
        }
        cbm_log_warn("design.source_skip", "source_kind", source_kind ? source_kind : "unknown",
                     "path", file->rel_path ? file->rel_path : "", "reason", "snapshot_mismatch");
        return -1;
    }
    return read_rc;
}

static const cbm_file_info_t *design_find_file(const design_ctx_t *ctx, const char *rel_path) {
    for (int i = 0; i < ctx->opts->file_count; i++) {
        if (ctx->opts->files[i].rel_path && strcmp(ctx->opts->files[i].rel_path, rel_path) == 0) {
            return &ctx->opts->files[i];
        }
    }
    return NULL;
}

static int design_load_config(design_ctx_t *ctx) {
    static const char config_rel_path[] = ".codebase-memory.json";
    const cbm_file_info_t *file = design_find_file(ctx, config_rel_path);
    cbm_file_info_t synthetic = {0};
    char path[DESIGN_PATH_CAP];
    if (!file) {
        if (snprintf(path, sizeof(path), "%s/%s", ctx->opts->repo_path, config_rel_path) >=
            (int)sizeof(path)) {
            return 0;
        }
        synthetic.path = path;
        synthetic.rel_path = (char *)config_rel_path;
        /* The UTF-8-safe stable reader determines the actual size. A negative
         * sentinel distinguishes this optional, undiscovered config from a
         * selected empty source (size zero). */
        synthetic.size = -1;
        file = &synthetic;
    }
    char *buf = NULL;
    size_t n = 0;
    int read_rc =
        design_load_validated_source_limited(ctx, file, "design-config", 1024U * 1024U, &buf, &n);
    if (read_rc <= 0) {
        return read_rc < 0 ? -1 : 0;
    }
    if (n > 1024 * 1024) {
        free(buf);
        cbm_log_warn("design.source_skip", "source_kind", "design-config", "path", config_rel_path,
                     "reason", "oversized");
        return 0;
    }
    yyjson_doc *doc = yyjson_read(buf, n, 0);
    free(buf);
    if (!doc) {
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *design = yyjson_is_obj(root) ? yyjson_obj_get(root, "design") : NULL;
    int rc = 0;
    if (yyjson_is_obj(design)) {
        if (cbm_design_patterns_load(design, "documents", &ctx->config.documents) != 0 ||
            cbm_design_patterns_load(design, "token_sources", &ctx->config.token_sources) != 0 ||
            cbm_design_patterns_load(design, "resolvers", &ctx->config.resolvers) != 0 ||
            cbm_design_patterns_load(design, "authoritative", &ctx->config.authoritative) != 0 ||
            cbm_design_patterns_load(design, "generated", &ctx->config.generated) != 0) {
            rc = -1;
        }
    }
    yyjson_doc_free(doc);
    return rc;
}

static const char *design_provenance(const design_ctx_t *ctx, const char *path,
                                     const char *default_value) {
    if (cbm_design_patterns_match(&ctx->config.generated, path)) {
        return "generated";
    }
    if (cbm_design_patterns_match(&ctx->config.authoritative, path)) {
        return "authoritative";
    }
    return default_value;
}

static char *design_properties(const char *format, const char *source_path, const char *scope,
                               const char *token_path, const char *type, const char *value,
                               const char *description, const char *provenance,
                               const char *reference, const char *extends, bool composite) {
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
    if (reference && reference[0]) {
        yyjson_mut_obj_add_str(doc, root, "reference", reference);
    }
    if (extends && extends[0]) {
        yyjson_mut_obj_add_str(doc, root, "extends", extends);
    }
    if (composite) {
        yyjson_mut_obj_add_bool(doc, root, "composite", true);
    }
    yyjson_mut_obj_add_bool(doc, root, "authoritative",
                            provenance && strcmp(provenance, "authoritative") == 0);
    yyjson_mut_obj_add_bool(doc, root, "generated",
                            provenance && strcmp(provenance, "generated") == 0);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int64_t design_ensure_system(design_ctx_t *ctx, const char *scope, const char *scope_qn,
                                    const char *source_path, const char *name) {
    char qn[DESIGN_QN_CAP];
    if (design_system_qn(ctx, scope_qn, qn, sizeof(qn)) != 0) {
        cbm_log_warn("design.identity_qn_overflow", "kind", "system", "path",
                     source_path ? source_path : "");
        return 0;
    }
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (existing) {
        return existing->id;
    }
    char *props = design_properties("design-context", source_path, scope, NULL, NULL, NULL, NULL,
                                    "authoritative", NULL, NULL, false);
    if (!props) {
        return 0;
    }
    int64_t id =
        cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignSystem", name && name[0] ? name : scope, qn,
                             source_path ? source_path : "", 1, 1, props);
    free(props);
    return id;
}

static int design_alias_add(design_ctx_t *ctx, int64_t source_id, const char *scope,
                            const char *scope_qn, const char *target_path) {
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
    bool target_requires_digest = false;
    char *target_identity =
        design_identity_pointer_from_delimited(target_path, '.', &target_requires_digest);
    design_alias_t *alias = &ctx->aliases.items[ctx->aliases.count];
    memset(alias, 0, sizeof(*alias));
    alias->source_id = source_id;
    alias->scope = design_strdup(scope);
    alias->scope_qn = design_strdup(scope_qn);
    alias->target_path = design_strdup(target_path);
    alias->target_identity = target_identity;
    alias->target_identity_requires_digest = target_requires_digest;
    if (!alias->scope || !alias->scope_qn || !alias->target_path || !alias->target_identity) {
        free(alias->scope);
        free(alias->scope_qn);
        free(alias->target_path);
        free(alias->target_identity);
        memset(alias, 0, sizeof(*alias));
        return -1;
    }
    ctx->aliases.count++;
    return 0;
}

static int64_t design_upsert_component(design_ctx_t *ctx, const char *scope, const char *scope_qn,
                                       const char *name, const char *file_path,
                                       const char *provenance, int64_t system_id) {
    char qn[DESIGN_QN_CAP];
    if (design_component_qn(ctx, scope_qn, name, qn, sizeof(qn)) != 0) {
        cbm_log_warn("design.identity_qn_overflow", "kind", "component", "path",
                     file_path ? file_path : "");
        return 0;
    }
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (existing) {
        return existing->id;
    }
    char *props = design_properties("design-component", file_path, scope, NULL, NULL, NULL, NULL,
                                    provenance, NULL, NULL, false);
    if (!props) {
        return 0;
    }
    int64_t id =
        cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignComponent", name, qn, file_path, 1, 1, props);
    free(props);
    if (id > 0 && system_id > 0) {
        cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, id, "PROVIDES", "{}");
    }
    if (id > 0) {
        ctx->component_count++;
    }
    return id;
}

static design_token_source_t *design_token_source_get(design_ctx_t *ctx, const char *path) {
    for (int i = 0; i < ctx->token_sources.count; i++) {
        if (strcmp(ctx->token_sources.items[i].path, path) == 0) {
            return &ctx->token_sources.items[i];
        }
    }
    if (ctx->token_sources.count == ctx->token_sources.cap) {
        int cap = ctx->token_sources.cap ? ctx->token_sources.cap * 2 : 16;
        design_token_source_t *grown = (design_token_source_t *)realloc(
            ctx->token_sources.items, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        ctx->token_sources.items = grown;
        ctx->token_sources.cap = cap;
    }
    design_token_source_t *source = &ctx->token_sources.items[ctx->token_sources.count++];
    memset(source, 0, sizeof(*source));
    source->path = design_strdup(path);
    return source->path ? source : NULL;
}

static int design_token_definition_add(design_ctx_t *ctx, const char *file_path, int64_t token_id,
                                       const char *token_path, const char *type, const char *value,
                                       const char *description, const char *format,
                                       const char *provenance, int start_line) {
    design_token_source_t *source = design_token_source_get(ctx, file_path);
    if (!source) {
        return -1;
    }
    if (source->count == source->cap) {
        int cap = source->cap ? source->cap * 2 : 16;
        design_token_definition_t *grown =
            (design_token_definition_t *)realloc(source->items, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        source->items = grown;
        source->cap = cap;
    }
    design_token_definition_t *definition = &source->items[source->count++];
    memset(definition, 0, sizeof(*definition));
    definition->token_id = token_id;
    definition->source_path = design_strdup(file_path);
    definition->token_path = design_strdup(token_path);
    definition->type = design_strdup(type);
    definition->value = design_strdup(value);
    definition->description = design_strdup(description);
    definition->format = design_strdup(format);
    definition->provenance = design_strdup(provenance);
    definition->start_line = start_line;
    return definition->source_path && definition->token_path && (!value || definition->value) ? 0
                                                                                              : -1;
}

/* Sources are parsed in descending priority, so the first definition for a
 * qualified token is its canonical definition. Ties are broken by source path
 * and line through the ordered file walk below. Every later definition is
 * retained as provenance metadata rather than silently overwriting it. */
static int design_source_priority(const char *format, const char *provenance) {
    int provenance_rank = 100;
    if (provenance && strcmp(provenance, "authoritative") == 0) {
        provenance_rank = 200;
    } else if (provenance && strcmp(provenance, "generated") == 0) {
        provenance_rank = 0;
    }
    int format_rank = 0;
    if (format && strcmp(format, "dtcg") == 0) {
        format_rank = 30;
    } else if (format && strcmp(format, "design-md") == 0) {
        format_rank = 20;
    } else if (format && strcmp(format, "css") == 0) {
        format_rank = 10;
    }
    return provenance_rank + format_rank;
}

static int64_t design_upsert_token_identity(
    design_ctx_t *ctx, const char *scope, const char *scope_qn, const char *token_path,
    const char *canonical_identity, bool identity_requires_digest, const char *type,
    const char *value, const char *description, const char *file_path, const char *format,
    const char *provenance, const char *reference, const char *extends, bool composite,
    int start_line, int64_t system_id) {
    char qn[DESIGN_QN_CAP];
    if (design_token_qn(ctx, scope_qn, token_path, canonical_identity, identity_requires_digest, qn,
                        sizeof(qn)) != 0) {
        cbm_log_warn("design.identity_qn_overflow", "kind", "token", "path",
                     file_path ? file_path : "");
        return 0;
    }
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    bool incoming_generated = provenance && strcmp(provenance, "generated") == 0;
    int64_t id = existing ? existing->id : 0;
    if (!existing) {
        char *props = design_properties(format, file_path, scope, token_path, type, value,
                                        description, provenance, reference, extends, composite);
        if (!props) {
            return 0;
        }
        const char *name = strrchr(token_path, '.');
        name = name ? name + 1 : token_path;
        id = cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignToken", name, qn, file_path, start_line,
                                  start_line, props);
        free(props);
    }
    if (id > 0 && system_id > 0) {
        cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, id, "PROVIDES", "{}");
    }
    if (!existing && id > 0) {
        ctx->token_count++;
    }
    if (id > 0 && design_token_definition_add(ctx, file_path, id, token_path, type, value,
                                              description, format, provenance, start_line) != 0) {
        return 0;
    }
    if (id > 0 && incoming_generated) {
        char *file_qn = cbm_pipeline_fqn_compute(ctx->opts->project_name, file_path, "__file__");
        const cbm_gbuf_node_t *file_node =
            file_qn ? cbm_gbuf_find_by_qn(ctx->opts->gbuf, file_qn) : NULL;
        if (file_node) {
            cbm_gbuf_insert_edge(ctx->opts->gbuf, id, file_node->id, "GENERATED_AS", "{}");
        }
        free(file_qn);
    }
    if (value && value[0] == '{') {
        size_t n = strlen(value);
        if (n > 2 && value[n - 1] == '}') {
            char *target = (char *)malloc(n - 1);
            if (target) {
                memcpy(target, value + 1, n - 2);
                target[n - 2] = '\0';
                if (design_alias_add(ctx, id, scope, scope_qn, target) != 0) {
                    free(target);
                    return 0;
                }
                free(target);
            }
        }
    }
    return id;
}

static int design_definition_compare(const void *a, const void *b) {
    const design_token_definition_t *left = *(const design_token_definition_t *const *)a;
    const design_token_definition_t *right = *(const design_token_definition_t *const *)b;
    if (left->token_id < right->token_id) {
        return -1;
    }
    if (left->token_id > right->token_id) {
        return 1;
    }
    int path_order = strcmp(left->source_path, right->source_path);
    if (path_order != 0) {
        return path_order;
    }
    if (left->start_line != right->start_line) {
        return left->start_line < right->start_line ? -1 : 1;
    }
    return strcmp(left->value ? left->value : "", right->value ? right->value : "");
}

static bool design_definition_is_canonical(const cbm_gbuf_node_t *token,
                                           const design_token_definition_t *definition) {
    return token && token->file_path && strcmp(token->file_path, definition->source_path) == 0 &&
           token->start_line == definition->start_line;
}

static void design_definition_add_json(yyjson_mut_doc *doc, yyjson_mut_val *object,
                                       const design_token_definition_t *definition, bool canonical,
                                       bool ambiguous) {
    yyjson_mut_obj_add_str(doc, object, "source_path", definition->source_path);
    yyjson_mut_obj_add_int(doc, object, "line", definition->start_line);
    yyjson_mut_obj_add_str(doc, object, "token_path", definition->token_path);
    if (definition->type) {
        yyjson_mut_obj_add_str(doc, object, "token_type", definition->type);
    }
    if (definition->value) {
        yyjson_mut_obj_add_str(doc, object, "value", definition->value);
    }
    if (definition->description) {
        yyjson_mut_obj_add_str(doc, object, "description", definition->description);
    }
    if (definition->format) {
        yyjson_mut_obj_add_str(doc, object, "source_format", definition->format);
    }
    if (definition->provenance) {
        yyjson_mut_obj_add_str(doc, object, "provenance", definition->provenance);
    }
    yyjson_mut_obj_add_bool(doc, object, "canonical", canonical);
    yyjson_mut_obj_add_bool(doc, object, "ambiguous", ambiguous);
}

static char *design_definition_edge_properties(const design_token_definition_t *definition,
                                               bool canonical, bool ambiguous) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    design_definition_add_json(doc, root, definition, canonical, ambiguous);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

/* Attach the complete source set to each canonical DesignToken and add one
 * File-[:DEFINES_TOKEN]->DesignToken edge per source definition. This keeps
 * duplicate DTCG/CSS values queryable while the token itself remains the
 * stable identity used by aliases, modes, and usages. */
static int design_materialize_token_definitions(design_ctx_t *ctx) {
    int definition_count = 0;
    for (int i = 0; i < ctx->token_sources.count; i++) {
        definition_count += ctx->token_sources.items[i].count;
    }
    if (definition_count == 0) {
        return 0;
    }
    design_token_definition_t **definitions =
        (design_token_definition_t **)malloc((size_t)definition_count * sizeof(*definitions));
    if (!definitions) {
        return -1;
    }
    int cursor = 0;
    for (int i = 0; i < ctx->token_sources.count; i++) {
        for (int j = 0; j < ctx->token_sources.items[i].count; j++) {
            definitions[cursor++] = &ctx->token_sources.items[i].items[j];
        }
    }
    qsort(definitions, (size_t)definition_count, sizeof(*definitions), design_definition_compare);

    int rc = 0;
    for (int first = 0; first < definition_count;) {
        int end = first + 1;
        while (end < definition_count &&
               definitions[end]->token_id == definitions[first]->token_id) {
            end++;
        }
        const cbm_gbuf_node_t *token =
            cbm_gbuf_find_by_id(ctx->opts->gbuf, definitions[first]->token_id);
        yyjson_doc *properties_doc =
            token && token->properties_json
                ? yyjson_read(token->properties_json, strlen(token->properties_json), 0)
                : NULL;
        if (!token || !properties_doc || !yyjson_is_obj(yyjson_doc_get_root(properties_doc))) {
            yyjson_doc_free(properties_doc);
            rc = -1;
            break;
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root =
            doc ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(properties_doc)) : NULL;
        yyjson_doc_free(properties_doc);
        if (!doc || !root) {
            yyjson_mut_doc_free(doc);
            rc = -1;
            break;
        }
        yyjson_mut_doc_set_root(doc, root);
        bool ambiguous = end - first > 1;
        yyjson_mut_obj_add_int(doc, root, "definition_count", end - first);
        yyjson_mut_obj_add_bool(doc, root, "ambiguous", ambiguous);
        yyjson_mut_obj_add_str(
            doc, root, "canonical_selection",
            "provenance then source format; ties use source path and first definition");
        yyjson_mut_val *array = yyjson_mut_arr(doc);
        for (int i = first; i < end; i++) {
            bool canonical = design_definition_is_canonical(token, definitions[i]);
            yyjson_mut_val *entry = yyjson_mut_obj(doc);
            design_definition_add_json(doc, entry, definitions[i], canonical, ambiguous);
            yyjson_mut_arr_add_val(array, entry);

            char *file_qn = cbm_pipeline_fqn_compute(ctx->opts->project_name,
                                                     definitions[i]->source_path, "__file__");
            const cbm_gbuf_node_t *file_node =
                file_qn ? cbm_gbuf_find_by_qn(ctx->opts->gbuf, file_qn) : NULL;
            char *edge_properties =
                design_definition_edge_properties(definitions[i], canonical, ambiguous);
            if (!edge_properties ||
                (file_node && cbm_gbuf_insert_edge(ctx->opts->gbuf, file_node->id, token->id,
                                                   "DEFINES_TOKEN", edge_properties) <= 0)) {
                free(file_qn);
                free(edge_properties);
                yyjson_mut_doc_free(doc);
                rc = -1;
                goto done;
            }
            free(file_qn);
            free(edge_properties);
        }
        yyjson_mut_obj_add_val(doc, root, "definitions", array);
        char *properties = yyjson_mut_write(doc, 0, NULL);
        yyjson_mut_doc_free(doc);
        if (!properties ||
            cbm_gbuf_upsert_node(ctx->opts->gbuf, token->label, token->name, token->qualified_name,
                                 token->file_path, token->start_line, token->end_line,
                                 properties) <= 0) {
            free(properties);
            rc = -1;
            break;
        }
        free(properties);
        first = end;
    }

done:
    free(definitions);
    return rc;
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

static char *design_join_path_dup(const char *prefix, const char *name) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t name_len = name ? strlen(name) : 0;
    bool separator = prefix_len > 0 && name_len > 0;
    if (prefix_len > SIZE_MAX - name_len - (separator ? 2U : 1U)) {
        return NULL;
    }
    char *out = (char *)malloc(prefix_len + name_len + (separator ? 2U : 1U));
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    if (prefix_len > 0) {
        memcpy(out + pos, prefix, prefix_len);
        pos += prefix_len;
    }
    if (separator) {
        out[pos++] = '.';
    }
    if (name_len > 0) {
        memcpy(out + pos, name, name_len);
        pos += name_len;
    }
    out[pos] = '\0';
    return out;
}

static char *design_join_path_segment_dup(const char *prefix, const char *segment,
                                          size_t segment_len) {
    static const char empty_segment[] = "empty";
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t readable_len = segment_len == 0 ? sizeof(empty_segment) - 1 : 0;
    for (size_t i = 0; i < segment_len; i++) {
        unsigned char c = (unsigned char)segment[i];
        size_t add = (c == 0 || c < 0x20 || c == 0x7f) ? 4U : 1U;
        if (readable_len > SIZE_MAX - add) {
            return NULL;
        }
        readable_len += add;
    }
    bool separator = prefix_len > 0;
    if (prefix_len > SIZE_MAX - readable_len - (separator ? 2U : 1U)) {
        return NULL;
    }
    char *out = (char *)malloc(prefix_len + readable_len + (separator ? 2U : 1U));
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    if (prefix_len > 0) {
        memcpy(out, prefix, prefix_len);
        pos = prefix_len;
    }
    if (separator) {
        out[pos++] = '.';
    }
    if (segment_len == 0) {
        memcpy(out + pos, empty_segment, sizeof(empty_segment) - 1);
        pos += sizeof(empty_segment) - 1;
    } else {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < segment_len; i++) {
            unsigned char c = (unsigned char)segment[i];
            if (c == 0 || c < 0x20 || c == 0x7f) {
                out[pos++] = '\\';
                out[pos++] = 'x';
                out[pos++] = hex[c >> 4];
                out[pos++] = hex[c & 0x0f];
            } else {
                out[pos++] = (char)c;
            }
        }
    }
    out[pos] = '\0';
    return out;
}

/* Recursively visit a DTCG document. Group `$type` is inherited. Structural
 * `$ref`/`$extends` metadata is preserved but deliberately not evaluated. */
static int design_parse_dtcg_object(design_ctx_t *ctx, yyjson_val *object, const char *prefix,
                                    const char *identity, bool identity_requires_digest,
                                    const char *inherited_type, const char *inherited_extends,
                                    const char *scope, const char *scope_qn, const char *file_path,
                                    const char *provenance, int64_t system_id) {
    if (!yyjson_is_obj(object)) {
        return 0;
    }
    const char *group_type = inherited_type;
    yyjson_val *type_value = yyjson_obj_get(object, "$type");
    if (yyjson_is_str(type_value)) {
        group_type = yyjson_get_str(type_value);
    }
    const char *group_extends = inherited_extends;
    yyjson_val *extends_value = yyjson_obj_get(object, "$extends");
    if (yyjson_is_str(extends_value)) {
        group_extends = yyjson_get_str(extends_value);
    }

    yyjson_val *value = yyjson_obj_get(object, "$value");
    yyjson_val *reference_value = yyjson_obj_get(object, "$ref");
    const char *reference = yyjson_is_str(reference_value) ? yyjson_get_str(reference_value) : NULL;
    if ((value || reference) && prefix && prefix[0]) {
        const char *token_type = group_type;
        const char *description = NULL;
        yyjson_val *description_value = yyjson_obj_get(object, "$description");
        if (yyjson_is_str(description_value)) {
            description = yyjson_get_str(description_value);
        }
        char *serialized = value ? design_json_value_string(value) : design_strdup(reference);
        if (!serialized) {
            return -1;
        }
        int64_t token_id = design_upsert_token_identity(
            ctx, scope, scope_qn, prefix, identity, identity_requires_digest, token_type, serialized,
            description, file_path, "dtcg", provenance, reference, group_extends,
            yyjson_is_obj(value), 1, system_id);
        free(serialized);
        return token_id > 0 ? 0 : -1;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(object, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        const char *name = yyjson_get_str(key);
        size_t name_len = yyjson_get_len(key);
        yyjson_val *child = yyjson_obj_iter_get_val(key);
        bool root_token = name && name_len == strlen("$root") &&
                          memcmp(name, "$root", name_len) == 0;
        if (!name || (name_len > 0 && name[0] == '$' && !root_token)) {
            continue;
        }
        if (yyjson_is_obj(child)) {
            char *path = design_join_path_segment_dup(prefix, name, name_len);
            char *child_identity = design_identity_pointer_append(identity, name, name_len);
            bool child_requires_digest =
                identity_requires_digest || !design_identity_segment_safe(name, name_len);
            if (!path || !child_identity) {
                free(path);
                free(child_identity);
                return -1;
            }
            int rc = design_parse_dtcg_object(
                ctx, child, path, child_identity, child_requires_digest, group_type, group_extends,
                scope, scope_qn, file_path, provenance, system_id);
            free(path);
            free(child_identity);
            if (rc != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int design_parse_dtcg(design_ctx_t *ctx, const cbm_file_info_t *file) {
    size_t len = 0;
    char *source = NULL;
    int read_rc = design_load_validated_source(ctx, file, "dtcg", &source, &len);
    if (read_rc <= 0) {
        return read_rc;
    }
    yyjson_doc *doc = yyjson_read(source, len, 0);
    free(source);
    if (!doc) {
        cbm_log_warn("design.parse_skip", "format", "dtcg", "path", file->rel_path);
        return 0;
    }
    char scope[DESIGN_PATH_CAP];
    char scope_qn[DESIGN_PATH_CAP];
    int64_t system_id = 0;
    if (design_scope_for_file(ctx, file->rel_path, scope, sizeof(scope), scope_qn,
                              sizeof(scope_qn), &system_id) != 0) {
        yyjson_doc_free(doc);
        return -1;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *provenance = design_provenance(ctx, file->rel_path, "authoritative");
    int rc = design_parse_dtcg_object(ctx, root, "", "", false, NULL, NULL, scope, scope_qn,
                                      file->rel_path, provenance, system_id);
    yyjson_doc_free(doc);
    return rc;
}

typedef struct {
    int indent;
    char *key;
} design_yaml_level_t;

typedef struct {
    char *token_path;
    char *identity;
    bool identity_requires_digest;
    yyjson_mut_doc *doc;
    yyjson_mut_val *value;
    int start_line;
} design_google_composite_t;

typedef struct {
    design_google_composite_t *items;
    int count;
    int cap;
} design_google_composites_t;

static void design_google_composites_free(design_google_composites_t *composites) {
    for (int i = 0; i < composites->count; i++) {
        free(composites->items[i].token_path);
        free(composites->items[i].identity);
        yyjson_mut_doc_free(composites->items[i].doc);
    }
    free(composites->items);
}

static design_google_composite_t *design_google_composite_get(
    design_google_composites_t *composites, const char *token_path, const char *identity,
    bool identity_requires_digest, int start_line) {
    for (int i = 0; i < composites->count; i++) {
        if (strcmp(composites->items[i].identity, identity) == 0) {
            return &composites->items[i];
        }
    }
    if (composites->count == composites->cap) {
        int cap = composites->cap ? composites->cap * 2 : 8;
        design_google_composite_t *grown =
            (design_google_composite_t *)realloc(composites->items, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        composites->items = grown;
        composites->cap = cap;
    }
    design_google_composite_t *composite = &composites->items[composites->count];
    memset(composite, 0, sizeof(*composite));
    composite->token_path = design_strdup(token_path);
    composite->identity = design_strdup(identity);
    composite->identity_requires_digest = identity_requires_digest;
    composite->start_line = start_line;
    composite->doc = yyjson_mut_doc_new(NULL);
    if (!composite->token_path || !composite->identity || !composite->doc) {
        free(composite->token_path);
        free(composite->identity);
        yyjson_mut_doc_free(composite->doc);
        memset(composite, 0, sizeof(*composite));
        return NULL;
    }
    composite->value = yyjson_mut_obj(composite->doc);
    if (!composite->value) {
        free(composite->token_path);
        free(composite->identity);
        yyjson_mut_doc_free(composite->doc);
        memset(composite, 0, sizeof(*composite));
        return NULL;
    }
    yyjson_mut_doc_set_root(composite->doc, composite->value);
    composites->count++;
    return composite;
}

static int design_google_composite_add(design_google_composites_t *composites,
                                       const char *token_path, const char *identity,
                                       bool identity_requires_digest, const char *field,
                                       const char *value, int start_line) {
    design_google_composite_t *composite =
        design_google_composite_get(composites, token_path, identity,
                                    identity_requires_digest, start_line);
    if (!composite || !composite->value) {
        return -1;
    }
    yyjson_mut_val *field_copy = yyjson_mut_strcpy(composite->doc, field);
    yyjson_mut_val *value_copy = yyjson_mut_strcpy(composite->doc, value);
    return field_copy && value_copy && yyjson_mut_obj_add(composite->value, field_copy, value_copy)
               ? 0
               : -1;
}

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

static int design_yaml_path_identity(const design_yaml_level_t *levels, int first, int count,
                                     const char *leaf, char **out_path, char **out_identity,
                                     bool *out_requires_digest) {
    char *path = design_strdup("");
    char *identity = design_strdup("");
    bool requires_digest = false;
    if (!path || !identity) {
        free(path);
        free(identity);
        return -1;
    }
    int total = count + (leaf ? 1 : 0);
    for (int i = 0; i < total; i++) {
        const char *segment = i < count ? levels[first + i].key : leaf;
        if (!segment) {
            free(path);
            free(identity);
            return -1;
        }
        char *next_path = design_join_path_dup(path, segment);
        char *next_identity = design_identity_pointer_append(identity, segment, strlen(segment));
        if (!design_identity_segment_safe(segment, strlen(segment))) {
            requires_digest = true;
        }
        free(path);
        free(identity);
        path = next_path;
        identity = next_identity;
        if (!path || !identity) {
            free(path);
            free(identity);
            return -1;
        }
    }
    *out_path = path;
    *out_identity = identity;
    *out_requires_digest = requires_digest;
    return 0;
}

static int design_parse_frontmatter(design_ctx_t *ctx, char *source, const cbm_file_info_t *file,
                                    const char *scope, const char *scope_qn, int64_t system_id,
                                    char *system_name, size_t system_name_size) {
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
    design_google_composites_t composites = {0};
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
                        levels[level_count].key = key;
                        level_count++;
                    } else {
                        cbm_log_warn("design.frontmatter_skip", "reason", "nesting_too_deep",
                                     "path", file->rel_path);
                        design_google_composites_free(&composites);
                        return -1;
                    }
                } else if (level_count == 0) {
                    if (strcmp(key, "name") == 0) {
                        int name_written = snprintf(system_name, system_name_size, "%s", value);
                        if (name_written < 0 || (size_t)name_written >= system_name_size) {
                            cbm_log_warn("design.display_name_truncated", "kind", "system",
                                         "path", file->rel_path);
                        }
                    }
                } else {
                    char *token_path = NULL;
                    char *token_identity = NULL;
                    bool token_requires_digest = false;
                    if (design_yaml_path_identity(levels, 0, level_count, key, &token_path,
                                                  &token_identity,
                                                  &token_requires_digest) != 0) {
                        design_google_composites_free(&composites);
                        return -1;
                    }
                    const char *root_key = levels[0].key;
                    int scalar_rc = 0;
                    if (strcmp(root_key, "components") == 0 && level_count >= 2) {
                        int64_t component_id = design_upsert_component(
                            ctx, scope, scope_qn, levels[1].key, file->rel_path, provenance,
                            system_id);
                        int64_t token_id = design_upsert_token_identity(
                            ctx, scope, scope_qn, token_path, token_identity,
                            token_requires_digest, NULL, value, NULL, file->rel_path, "design-md",
                            provenance, NULL, NULL, false, line_no, system_id);
                        if (component_id <= 0 || token_id <= 0) {
                            scalar_rc = -1;
                        }
                        if (scalar_rc == 0 && component_id > 0 && token_id > 0) {
                            cbm_gbuf_insert_edge(ctx->opts->gbuf, component_id, token_id,
                                                 "PROVIDES", "{}");
                        }
                    } else if (strcmp(root_key, "typography") == 0 && level_count >= 2) {
                        char *composite_path = NULL;
                        char *composite_identity = NULL;
                        bool composite_requires_digest = false;
                        char *field_path = NULL;
                        char *field_identity = NULL;
                        bool field_requires_digest = false;
                        if (design_yaml_path_identity(levels, 0, 2, NULL, &composite_path,
                                                      &composite_identity,
                                                      &composite_requires_digest) != 0 ||
                            design_yaml_path_identity(levels, 2, level_count - 2, key, &field_path,
                                                      &field_identity,
                                                      &field_requires_digest) != 0 ||
                            design_google_composite_add(
                                &composites, composite_path, composite_identity,
                                composite_requires_digest, field_path, value, line_no) != 0) {
                            scalar_rc = -1;
                        }
                        free(composite_path);
                        free(composite_identity);
                        free(field_path);
                        free(field_identity);
                    } else if (strcmp(root_key, "colors") == 0 ||
                               strcmp(root_key, "typography") == 0 ||
                               strcmp(root_key, "spacing") == 0 ||
                               strcmp(root_key, "rounded") == 0) {
                        if (design_upsert_token_identity(
                                ctx, scope, scope_qn, token_path, token_identity,
                                token_requires_digest, design_google_type(root_key), value, NULL,
                                file->rel_path, "design-md", provenance, NULL, NULL, false, line_no,
                                system_id) <= 0) {
                            scalar_rc = -1;
                        }
                    }
                    free(token_path);
                    free(token_identity);
                    if (scalar_rc != 0) {
                        design_google_composites_free(&composites);
                        return -1;
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
    for (int i = 0; i < composites.count; i++) {
        char *serialized = yyjson_mut_write(composites.items[i].doc, 0, NULL);
        if (!serialized ||
            design_upsert_token_identity(
                ctx, scope, scope_qn, composites.items[i].token_path,
                composites.items[i].identity, composites.items[i].identity_requires_digest,
                "typography", serialized, NULL, file->rel_path, "design-md", provenance, NULL,
                NULL, true, composites.items[i].start_line, system_id) <= 0) {
            free(serialized);
            design_google_composites_free(&composites);
            return -1;
        }
        free(serialized);
    }
    design_google_composites_free(&composites);
    return 0;
}

static int design_documents_add(design_ctx_t *ctx, const cbm_file_info_t *file, const char *dir,
                                const char *scope, const char *scope_qn, int64_t system_id,
                                char *source) {
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
    char *path = design_strdup(file->rel_path);
    char *dir_copy = design_strdup(dir);
    char *scope_copy = design_strdup(scope);
    char *scope_qn_copy = design_strdup(scope_qn);
    if (!path || !dir_copy || !scope_copy || !scope_qn_copy || !source) {
        free(path);
        free(dir_copy);
        free(scope_copy);
        free(scope_qn_copy);
        return -1;
    }
    design_document_t *doc = &ctx->documents.items[ctx->documents.count++];
    memset(doc, 0, sizeof(*doc));
    doc->path = path;
    doc->dir = dir_copy;
    doc->scope = scope_copy;
    doc->scope_qn = scope_qn_copy;
    doc->system_id = system_id;
    doc->file = file;
    doc->source = source;
    return 0;
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
    char scope[DESIGN_PATH_CAP];
    char scope_qn[DESIGN_PATH_CAP];
    char *dir = design_dirname_dup(file->rel_path);
    if (!dir || design_scope_keys_from_dir(dir, scope, sizeof(scope), scope_qn,
                                           sizeof(scope_qn)) != 0) {
        free(dir);
        return -1;
    }
    char name[256];
    int name_written =
        snprintf(name, sizeof(name), "%s", strcmp(scope, "root") == 0 ? "Repository Design" : scope);
    if (name_written < 0 || (size_t)name_written >= sizeof(name)) {
        cbm_log_warn("design.display_name_truncated", "kind", "system", "path", file->rel_path);
    }
    size_t len = 0;
    char *source = NULL;
    int read_rc = design_load_validated_source(ctx, file, "design-md", &source, &len);
    if (read_rc <= 0) {
        free(dir);
        return read_rc;
    }
    design_frontmatter_name(source, name, sizeof(name));
    int64_t system_id = design_ensure_system(ctx, scope, scope_qn, file->rel_path, name);
    if (system_id <= 0) {
        free(dir);
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
    if (design_documents_add(ctx, file, dir, scope, scope_qn, system_id, source) != 0) {
        free(dir);
        free(source);
        return -1;
    }
    free(dir);
    ctx->document_count++;
    return 0;
}

static int design_parse_registered_document(design_ctx_t *ctx, design_document_t *document) {
    char name[256];
    snprintf(name, sizeof(name), "%s",
             strcmp(document->scope, "root") == 0 ? "Repository Design" : document->scope);
    design_frontmatter_name(document->source, name, sizeof(name));
    return design_parse_frontmatter(ctx, document->source, document->file, document->scope,
                                    document->scope_qn, document->system_id, name, sizeof(name));
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
        const char *dir = ctx->documents.items[i].dir;
        if (design_path_in_scope(path, dir) && strlen(dir) >= best_len) {
            best = &ctx->documents.items[i];
            best_len = strlen(dir);
        }
    }
    return best;
}

static int design_scope_for_file(design_ctx_t *ctx, const char *path, char *scope,
                                 size_t scope_size, char *scope_qn, size_t scope_qn_size,
                                 int64_t *system_id) {
    const design_document_t *document = design_nearest_document(ctx, path);
    if (document) {
        int scope_written = snprintf(scope, scope_size, "%s", document->scope);
        int qn_written = snprintf(scope_qn, scope_qn_size, "%s", document->scope_qn);
        if (scope_written < 0 || (size_t)scope_written >= scope_size || qn_written < 0 ||
            (size_t)qn_written >= scope_qn_size) {
            return -1;
        }
        *system_id = document->system_id;
        return 0;
    }
    if (design_scope_keys_from_dir("", scope, scope_size, scope_qn, scope_qn_size) != 0) {
        return -1;
    }
    *system_id = design_ensure_system(ctx, scope, scope_qn, path, "Repository Design");
    return *system_id > 0 ? 0 : -1;
}

static design_token_source_t *design_token_source_find(design_ctx_t *ctx, const char *path) {
    for (int i = 0; i < ctx->token_sources.count; i++) {
        if (strcmp(ctx->token_sources.items[i].path, path) == 0) {
            return &ctx->token_sources.items[i];
        }
    }
    return NULL;
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
                                    const char *modifier_name, size_t modifier_name_len,
                                    const char *context_name, size_t context_name_len,
                                    int resolution_order) {
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
    yyjson_mut_obj_add_strn(doc, root, "modifier", modifier_name, modifier_name_len);
    yyjson_mut_obj_add_strn(doc, root, "context", context_name, context_name_len);
    yyjson_mut_obj_add_str(doc, root, "resolver_version", "2025.10");
    if (resolution_order >= 0) {
        yyjson_mut_obj_add_int(doc, root, "resolution_order", resolution_order);
    }
    yyjson_val *description = yyjson_obj_get(modifier, "description");
    if (yyjson_is_str(description)) {
        yyjson_mut_obj_add_str(doc, root, "description", yyjson_get_str(description));
    }
    yyjson_val *default_value = yyjson_obj_get(modifier, "default");
    if (yyjson_is_str(default_value)) {
        yyjson_mut_obj_add_bool(
            doc, root, "default",
            yyjson_get_len(default_value) == context_name_len &&
                memcmp(yyjson_get_str(default_value), context_name, context_name_len) == 0);
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

static int design_resolver_modifier_order(yyjson_val *root, const char *modifier_name,
                                          size_t modifier_name_len) {
    yyjson_val *order = yyjson_is_obj(root) ? yyjson_obj_get(root, "resolutionOrder") : NULL;
    if (!yyjson_is_arr(order)) {
        return -1;
    }
    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(order, idx, max, entry) {
        yyjson_val *ref = yyjson_is_obj(entry) ? yyjson_obj_get(entry, "$ref") : NULL;
        const char *text = yyjson_is_str(ref) ? yyjson_get_str(ref) : NULL;
        size_t text_len = yyjson_is_str(ref) ? yyjson_get_len(ref) : 0;
        size_t name_start = 0;
        for (size_t i = 0; text && i < text_len; i++) {
            if (text[i] == '/') {
                name_start = i + 1;
            }
        }
        if (text && text_len - name_start == modifier_name_len &&
            memcmp(text + name_start, modifier_name, modifier_name_len) == 0) {
            return (int)idx;
        }
    }
    return -1;
}

static char *design_mode_override_properties(const design_token_definition_t *definition,
                                             const char *source_path, int source_order,
                                             int modifier_order, bool default_context) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "source_order", source_order);
    if (modifier_order >= 0) {
        yyjson_mut_obj_add_int(doc, root, "modifier_order", modifier_order);
    }
    yyjson_mut_obj_add_str(doc, root, "source_path", source_path);
    yyjson_mut_obj_add_str(doc, root, "token_path", definition->token_path);
    yyjson_mut_obj_add_int(doc, root, "line", definition->start_line);
    yyjson_mut_obj_add_bool(doc, root, "default", default_context);
    if (definition->type) {
        yyjson_mut_obj_add_str(doc, root, "token_type", definition->type);
    }
    if (definition->value) {
        yyjson_mut_obj_add_str(doc, root, "value", definition->value);
    }
    if (definition->description) {
        yyjson_mut_obj_add_str(doc, root, "description", definition->description);
    }
    if (definition->format) {
        yyjson_mut_obj_add_str(doc, root, "source_format", definition->format);
    }
    if (definition->provenance) {
        yyjson_mut_obj_add_str(doc, root, "provenance", definition->provenance);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int design_parse_resolver(design_ctx_t *ctx, const cbm_file_info_t *file) {
    size_t len = 0;
    char *source = NULL;
    int read_rc = design_load_validated_source(ctx, file, "dtcg-resolver", &source, &len);
    if (read_rc <= 0) {
        return read_rc;
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
    char scope_qn[DESIGN_PATH_CAP];
    int64_t system_id = 0;
    if (design_scope_for_file(ctx, file->rel_path, scope, sizeof(scope), scope_qn,
                              sizeof(scope_qn), &system_id) != 0) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_obj_iter modifier_iter;
    yyjson_obj_iter_init(modifiers, &modifier_iter);
    yyjson_val *modifier_key;
    while ((modifier_key = yyjson_obj_iter_next(&modifier_iter)) != NULL) {
        const char *modifier_name = yyjson_get_str(modifier_key);
        size_t modifier_name_len = yyjson_get_len(modifier_key);
        yyjson_val *modifier = yyjson_obj_iter_get_val(modifier_key);
        yyjson_val *contexts =
            yyjson_is_obj(modifier) ? yyjson_obj_get(modifier, "contexts") : NULL;
        if (!modifier_name || !yyjson_is_obj(contexts)) {
            continue;
        }
        int modifier_order =
            design_resolver_modifier_order(root, modifier_name, modifier_name_len);
        yyjson_val *default_value = yyjson_obj_get(modifier, "default");
        const char *default_context =
            yyjson_is_str(default_value) ? yyjson_get_str(default_value) : NULL;
        size_t default_context_len = yyjson_is_str(default_value) ? yyjson_get_len(default_value) : 0;
        yyjson_obj_iter context_iter;
        yyjson_obj_iter_init(contexts, &context_iter);
        yyjson_val *context_key;
        while ((context_key = yyjson_obj_iter_next(&context_iter)) != NULL) {
            const char *context_name = yyjson_get_str(context_key);
            size_t context_name_len = yyjson_get_len(context_key);
            yyjson_val *sources = yyjson_obj_iter_get_val(context_key);
            if (!context_name || !yyjson_is_arr(sources)) {
                continue;
            }
            char qn[DESIGN_QN_CAP];
            if (design_mode_qn(ctx, scope_qn, modifier_name, modifier_name_len, context_name,
                               context_name_len, qn, sizeof(qn)) != 0) {
                cbm_log_warn("design.identity_qn_overflow", "kind", "mode", "path",
                             file->rel_path);
                yyjson_doc_free(doc);
                return -1;
            }
            char *modifier_readable =
                design_join_path_segment_dup("", modifier_name, modifier_name_len);
            char *context_readable =
                design_join_path_segment_dup("", context_name, context_name_len);
            int display_len = modifier_readable && context_readable
                                  ? snprintf(NULL, 0, "%s: %s", modifier_readable, context_readable)
                                  : -1;
            char *display_name =
                display_len >= 0 ? (char *)malloc((size_t)display_len + 1U) : NULL;
            if (!display_name ||
                snprintf(display_name, (size_t)display_len + 1U, "%s: %s", modifier_readable,
                         context_readable) != display_len) {
                free(modifier_readable);
                free(context_readable);
                free(display_name);
                yyjson_doc_free(doc);
                return -1;
            }
            free(modifier_readable);
            free(context_readable);
            char *props = design_mode_properties(modifier, sources, file->rel_path, scope,
                                                 modifier_name, modifier_name_len, context_name,
                                                 context_name_len, modifier_order);
            if (!props) {
                free(display_name);
                yyjson_doc_free(doc);
                return -1;
            }
            int64_t mode_id = cbm_gbuf_upsert_node(ctx->opts->gbuf, "DesignMode", display_name, qn,
                                                   file->rel_path, 1, 1, props);
            free(props);
            free(display_name);
            if (mode_id <= 0) {
                yyjson_doc_free(doc);
                return -1;
            }
            if (cbm_gbuf_insert_edge(ctx->opts->gbuf, system_id, mode_id, "PROVIDES", "{}") <= 0) {
                yyjson_doc_free(doc);
                return -1;
            }
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
                design_token_source_t *token_source = design_token_source_find(ctx, source_path);
                if (!token_source) {
                    continue;
                }
                for (int token_idx = 0; token_idx < token_source->count; token_idx++) {
                    design_token_definition_t *definition = &token_source->items[token_idx];
                    char *override_props = design_mode_override_properties(
                        definition, source_path, (int)idx, modifier_order,
                        default_context && default_context_len == context_name_len &&
                            memcmp(default_context, context_name, context_name_len) == 0);
                    if (!override_props) {
                        yyjson_doc_free(doc);
                        return -1;
                    }
                    if (cbm_gbuf_insert_edge(ctx->opts->gbuf, mode_id, definition->token_id,
                                             "OVERRIDES", override_props) <= 0) {
                        free(override_props);
                        yyjson_doc_free(doc);
                        return -1;
                    }
                    free(override_props);
                }
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

static char *design_css_name_to_path(const char *name) {
    while (name && *name == '-') {
        name++;
    }
    size_t len = name ? strlen(name) : 0;
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    bool last_dot = false;
    for (size_t i = 0; name && name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
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
    return out;
}

/* Preserve the historical `--color-brand` <-> `color.brand` bridge when the
 * CSS spelling is injective. Other valid spellings (repeated separators,
 * underscores, Unicode escapes, etc.) carry their complete raw name in the
 * digest identity instead of collapsing onto that readable path. */
static char *design_css_identity(const char *name, bool *requires_digest) {
    const char *full = name ? name : "";
    const char *raw = strncmp(full, "--", 2) == 0 ? full + 2 : full;
    bool safe = strncmp(full, "--", 2) == 0 && raw[0] != '\0';
    bool at_segment_start = true;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (*p == '-') {
            if (at_segment_start) {
                safe = false;
            }
            at_segment_start = true;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9')) {
            at_segment_start = false;
        } else {
            safe = false;
            at_segment_start = false;
        }
    }
    if (at_segment_start) {
        safe = false;
    }
    if (safe) {
        return design_identity_pointer_from_delimited(raw, '-', requires_digest);
    }
    if (requires_digest) {
        *requires_digest = true;
    }
    return design_identity_pointer_append("", name ? name : "", name ? strlen(name) : 0);
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

static int64_t design_find_css_token(design_ctx_t *ctx, const char *scope, const char *scope_qn,
                                     const char *css_name) {
    char qn[DESIGN_QN_CAP];
    char *path = design_css_name_to_path(css_name);
    bool requires_digest = false;
    char *identity = design_css_identity(css_name, &requires_digest);
    if (!path || !identity ||
        design_token_qn(ctx, scope_qn, path, identity, requires_digest, qn, sizeof(qn)) != 0) {
        free(path);
        free(identity);
        return 0;
    }
    const cbm_gbuf_node_t *node = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
    if (!node && strcmp(scope, "root") != 0) {
        if (design_token_qn(ctx, "root", path, identity, requires_digest, qn, sizeof(qn)) == 0) {
            node = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
        }
    }
    free(path);
    free(identity);
    return node ? node->id : 0;
}

static int design_parse_css(design_ctx_t *ctx, const cbm_file_info_t *file, bool definitions,
                            bool usages) {
    size_t len = 0;
    char *source = NULL;
    const char *source_kind = design_has_suffix(file->rel_path, ".scss") ? "scss" : "css";
    int read_rc = design_load_validated_source(ctx, file, source_kind, &source, &len);
    if (read_rc <= 0) {
        return read_rc;
    }
    char scope[DESIGN_PATH_CAP];
    char scope_qn[DESIGN_PATH_CAP];
    int64_t system_id = 0;
    if (design_scope_for_file(ctx, file->rel_path, scope, sizeof(scope), scope_qn,
                              sizeof(scope_qn), &system_id) != 0) {
        free(source);
        return -1;
    }
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
                size_t name_len = (size_t)(name_end - p);
                char *name = design_strndup(p, name_len);
                if (!name) {
                    free(file_qn);
                    free(source);
                    return -1;
                }
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
                    } else if ((*value_end == ';' || *value_end == '}') && paren_depth == 0) {
                        break;
                    }
                    value_end++;
                }
                size_t value_len = (size_t)(value_end - value_start);
                char *value = design_strndup(value_start, value_len);
                if (!value) {
                    free(name);
                    free(file_qn);
                    free(source);
                    return -1;
                }
                char *trimmed = design_trim(value);
                char *token_path = design_css_name_to_path(name);
                bool identity_requires_digest = false;
                char *identity = design_css_identity(name, &identity_requires_digest);
                int64_t token_id =
                    token_path && identity
                        ? design_upsert_token_identity(
                              ctx, scope, scope_qn, token_path, identity,
                              identity_requires_digest, design_infer_css_type(name, trimmed),
                              trimmed, NULL, file->rel_path, "css", provenance, NULL, NULL, false,
                              design_line_number(source, p), system_id)
                        : 0;
                free(identity);
                free(token_path);
                free(value);
                free(name);
                if (token_id <= 0) {
                    free(file_qn);
                    free(source);
                    return -1;
                }
                /* A final custom-property declaration may legally omit its
                 * semicolon before `}`. Bare/partial CSS can also end at EOF.
                 * Do not let the for-loop increment a pointer already at the
                 * terminating NUL: that would dereference one-past the stable
                 * source allocation on the next condition check. */
                if (*value_end == '\0') {
                    break;
                }
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
                size_t n = (size_t)(end - name);
                char *css_name = design_strndup(name, n);
                if (!css_name) {
                    free(file_qn);
                    free(source);
                    return -1;
                }
                int64_t token_id = design_find_css_token(ctx, scope, scope_qn, css_name);
                free(css_name);
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
        const cbm_gbuf_node_t *target = NULL;
        if (design_token_qn(ctx, alias->scope_qn, alias->target_path, alias->target_identity,
                            alias->target_identity_requires_digest, qn, sizeof(qn)) == 0) {
            target = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
        }
        if (!target && strcmp(alias->scope, "root") != 0) {
            if (design_token_qn(ctx, "root", alias->target_path, alias->target_identity,
                                alias->target_identity_requires_digest, qn, sizeof(qn)) == 0) {
                target = cbm_gbuf_find_by_qn(ctx->opts->gbuf, qn);
            }
        }
        if (target) {
            cbm_gbuf_insert_edge(ctx->opts->gbuf, alias->source_id, target->id, "ALIASES_TO", "{}");
        }
    }
}

static bool design_is_document(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.documents.count > 0) {
        return cbm_design_patterns_match(&ctx->config.documents, path);
    }
    return strcmp(design_basename(path), "DESIGN.md") == 0;
}

static bool design_is_token_source(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.token_sources.count > 0) {
        return cbm_design_patterns_match(&ctx->config.token_sources, path);
    }
    return design_has_suffix(path, ".tokens.json");
}

static bool design_is_resolver(const design_ctx_t *ctx, const char *path) {
    if (ctx->config.resolvers.count > 0) {
        return cbm_design_patterns_match(&ctx->config.resolvers, path);
    }
    return design_has_suffix(path, ".resolver.json");
}

static void design_ctx_free(design_ctx_t *ctx) {
    cbm_design_patterns_free(&ctx->config.documents);
    cbm_design_patterns_free(&ctx->config.token_sources);
    cbm_design_patterns_free(&ctx->config.resolvers);
    cbm_design_patterns_free(&ctx->config.authoritative);
    cbm_design_patterns_free(&ctx->config.generated);
    for (int i = 0; i < ctx->aliases.count; i++) {
        free(ctx->aliases.items[i].scope);
        free(ctx->aliases.items[i].scope_qn);
        free(ctx->aliases.items[i].target_path);
        free(ctx->aliases.items[i].target_identity);
    }
    free(ctx->aliases.items);
    for (int i = 0; i < ctx->documents.count; i++) {
        free(ctx->documents.items[i].path);
        free(ctx->documents.items[i].dir);
        free(ctx->documents.items[i].scope);
        free(ctx->documents.items[i].scope_qn);
        free(ctx->documents.items[i].source);
    }
    free(ctx->documents.items);
    for (int i = 0; i < ctx->token_sources.count; i++) {
        design_token_source_t *source = &ctx->token_sources.items[i];
        free(source->path);
        for (int j = 0; j < source->count; j++) {
            free(source->items[j].source_path);
            free(source->items[j].token_path);
            free(source->items[j].type);
            free(source->items[j].value);
            free(source->items[j].description);
            free(source->items[j].format);
            free(source->items[j].provenance);
        }
        free(source->items);
    }
    free(ctx->token_sources.items);
}

static int design_file_compare(const void *a, const void *b) {
    const cbm_file_info_t *left = *(const cbm_file_info_t *const *)a;
    const cbm_file_info_t *right = *(const cbm_file_info_t *const *)b;
    return strcmp(left->rel_path, right->rel_path);
}

int cbm_design_index(const cbm_design_index_opts_t *opts) {
    if (!opts || !opts->project_name || !opts->repo_path || !opts->gbuf ||
        (!opts->files && opts->file_count > 0) || opts->file_count < 0) {
        return -1;
    }
    design_ctx_t ctx = {.opts = opts};
    const cbm_file_info_t **files = NULL;
    int rc = -1;
    if (design_load_config(&ctx) != 0) {
        goto done;
    }
    if (opts->file_count > 0) {
        files = (const cbm_file_info_t **)malloc((size_t)opts->file_count * sizeof(*files));
        if (!files) {
            goto done;
        }
        for (int i = 0; i < opts->file_count; i++) {
            files[i] = &opts->files[i];
        }
        qsort(files, (size_t)opts->file_count, sizeof(*files), design_file_compare);
    }

    /* Register readable documents first so nearest-ancestor scope inheritance
     * is available to every token source. Frontmatter values are parsed later
     * in source-priority order. */
    for (int i = 0; i < opts->file_count; i++) {
        if (design_is_document(&ctx, files[i]->rel_path) &&
            design_parse_document(&ctx, files[i]) != 0) {
            goto done;
        }
    }

    static const int priorities[] = {230, 220, 210, 130, 120, 110, 30, 20, 10};
    for (size_t phase = 0; phase < sizeof(priorities) / sizeof(priorities[0]); phase++) {
        int priority = priorities[phase];

        for (int i = 0; i < opts->file_count; i++) {
            const cbm_file_info_t *file = files[i];
            if (!design_is_token_source(&ctx, file->rel_path)) {
                continue;
            }
            const char *provenance = design_provenance(&ctx, file->rel_path, "authoritative");
            if (design_source_priority("dtcg", provenance) != priority) {
                continue;
            }
            if (design_parse_dtcg(&ctx, file) != 0) {
                goto done;
            }
        }

        for (int i = 0; i < ctx.documents.count; i++) {
            design_document_t *document = &ctx.documents.items[i];
            const char *provenance = design_provenance(&ctx, document->path, "authoritative");
            if (design_source_priority("design-md", provenance) == priority &&
                design_parse_registered_document(&ctx, document) != 0) {
                goto done;
            }
        }

        for (int i = 0; i < opts->file_count; i++) {
            const cbm_file_info_t *file = files[i];
            if (!design_has_suffix(file->rel_path, ".css") &&
                !design_has_suffix(file->rel_path, ".scss")) {
                continue;
            }
            const char *provenance = design_provenance(&ctx, file->rel_path, "observed");
            if (design_source_priority("css", provenance) == priority &&
                design_parse_css(&ctx, file, true, false) != 0) {
                goto done;
            }
        }
    }

    /* Definitions across all files precede usages so cross-file var()
     * resolution does not depend on discovery order. */
    for (int i = 0; i < opts->file_count; i++) {
        const char *path = files[i]->rel_path;
        if ((design_has_suffix(path, ".css") || design_has_suffix(path, ".scss")) &&
            design_parse_css(&ctx, files[i], false, true) != 0) {
            goto done;
        }
    }

    /* Resolver metadata is indexed after token sources so mode-to-token
     * override edges can be connected without executing the resolver. */
    for (int i = 0; i < opts->file_count; i++) {
        if (design_is_resolver(&ctx, files[i]->rel_path) &&
            design_parse_resolver(&ctx, files[i]) != 0) {
            goto done;
        }
    }
    if (design_materialize_token_definitions(&ctx) != 0) {
        goto done;
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
    rc = 0;

done:
    free(files);
    design_ctx_free(&ctx);
    return rc;
}
