#include "pipeline/pipeline_internal.h"

#include "design/design.h"
#include "foundation/limits.h"
#include "foundation/log.h"
#include "foundation/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    cbm_gbuf_t *staging;
    bool failed;
} design_seed_ctx_t;

typedef struct {
    CBMHashTable *by_path;
    char auxiliary_hash[CBM_SHA256_HEX_LEN + 1];
    bool auxiliary_config_seen;
    bool enforce_selected_config;
    bool selected_config_should_be_read;
    const char *selected_config_hash;
} design_snapshot_ctx_t;

static cbm_pipeline_snapshot_hook_fn g_design_config_test_hook = NULL;
static void *g_design_config_test_userdata = NULL;

void cbm_pipeline_set_design_config_hook_for_test(cbm_pipeline_snapshot_hook_fn hook,
                                                  void *userdata) {
    g_design_config_test_hook = hook;
    g_design_config_test_userdata = userdata;
}

static void design_run_config_hook_for_test(void) {
    cbm_pipeline_snapshot_hook_fn hook = g_design_config_test_hook;
    void *userdata = g_design_config_test_userdata;
    g_design_config_test_hook = NULL;
    g_design_config_test_userdata = NULL;
    if (hook) {
        hook(userdata);
    }
}

static int design_verify_snapshot_source(void *userdata, const char *rel_path,
                                         const char *source_sha256) {
    design_snapshot_ctx_t *snapshot = (design_snapshot_ctx_t *)userdata;
    if (!snapshot || !rel_path || !source_sha256 || strlen(source_sha256) != CBM_SHA256_HEX_LEN) {
        return -1;
    }
    cbm_file_version_snapshot_t *version =
        snapshot->by_path ? (cbm_file_version_snapshot_t *)cbm_ht_get(snapshot->by_path, rel_path)
                          : NULL;
    if (version) {
        if (version->content_skipped) {
            return -1;
        }
        if (!version->verified) {
            memcpy(version->sha256, source_sha256, sizeof(version->sha256));
            version->verified = true;
            return 0;
        }
        return strcmp(version->sha256, source_sha256) == 0 ? 0 : -1;
    }
    /* The repository config is historically read even when ignored from code
     * discovery. Bind that one auxiliary input with a two-sided stable hash
     * instead of silently dropping its Design settings. */
    if (strcmp(rel_path, ".codebase-memory.json") != 0) {
        return -1;
    }
    design_run_config_hook_for_test();
    if (snapshot->enforce_selected_config &&
        (!snapshot->selected_config_should_be_read || !snapshot->selected_config_hash ||
         strcmp(snapshot->selected_config_hash, source_sha256) != 0)) {
        return -1;
    }
    if (snapshot->auxiliary_config_seen) {
        return strcmp(snapshot->auxiliary_hash, source_sha256) == 0 ? 0 : -1;
    }
    memcpy(snapshot->auxiliary_hash, source_sha256, sizeof(snapshot->auxiliary_hash));
    snapshot->auxiliary_config_seen = true;
    return 0;
}

static int design_verify_auxiliary_config(const cbm_pipeline_ctx_t *ctx,
                                          const design_snapshot_ctx_t *snapshot) {
    if (snapshot->enforce_selected_config &&
        snapshot->selected_config_should_be_read != snapshot->auxiliary_config_seen) {
        return -1;
    }
    if (!snapshot->auxiliary_config_seen) {
        return 0;
    }
    char path[CBM_SZ_4K];
    if (snprintf(path, sizeof(path), "%s/.codebase-memory.json", ctx->repo_path) >=
        (int)sizeof(path)) {
        return -1;
    }
    char live_hash[CBM_SHA256_HEX_LEN + 1];
    size_t max_bytes = 1024U * 1024U;
    long global_limit = cbm_max_file_bytes();
    if (global_limit > 0 && (size_t)global_limit < max_bytes) {
        max_bytes = (size_t)global_limit;
    }
    return cbm_sha256_file_hex_limited(path, max_bytes, live_hash) == 0 &&
                   strcmp(live_hash, snapshot->auxiliary_hash) == 0
               ? 0
               : -1;
}

/* The design pass is built in a staging buffer so a parse/allocation failure
 * cannot erase the last good derived subgraph. File and Markdown Section
 * nodes are the only existing graph boundaries the design index links to. */
static void design_seed_boundary_node(const cbm_gbuf_node_t *node, void *userdata) {
    design_seed_ctx_t *seed = (design_seed_ctx_t *)userdata;
    if (!node || !node->label ||
        (strcmp(node->label, "File") != 0 && strcmp(node->label, "Section") != 0)) {
        return;
    }
    if (cbm_gbuf_upsert_node(seed->staging, node->label, node->name, node->qualified_name,
                             node->file_path, node->start_line, node->end_line,
                             node->properties_json) <= 0) {
        seed->failed = true;
    }
}

int cbm_pipeline_pass_design(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                             int file_count) {
    if (!ctx || !ctx->gbuf || file_count < 0 || (!files && file_count > 0)) {
        return -1;
    }

    _Atomic int64_t staging_ids = cbm_gbuf_next_id(ctx->gbuf);
    cbm_gbuf_t *staging = cbm_gbuf_new_shared_ids(ctx->project_name, ctx->repo_path, &staging_ids);
    if (!staging) {
        return -1;
    }
    design_seed_ctx_t seed = {.staging = staging};
    cbm_gbuf_foreach_node(ctx->gbuf, design_seed_boundary_node, &seed);
    if (seed.failed) {
        cbm_gbuf_free(staging);
        return -1;
    }

    bool bind_snapshot = ctx->source_version_count == file_count &&
                         (file_count == 0 || (ctx->source_version_files && ctx->source_versions));
    design_snapshot_ctx_t snapshot = {0};
    const cbm_userconfig_snapshot_t *selected_config =
        ctx->pipeline ? cbm_pipeline_userconfig_snapshot(ctx->pipeline) : NULL;
    if (selected_config) {
        size_t selected_len = 0;
        snapshot.enforce_selected_config = true;
        int selected_present = cbm_userconfig_snapshot_project_source(
            selected_config, &snapshot.selected_config_hash, &selected_len);
        size_t selected_design_cap = 1024U * 1024U;
        long global_cap = cbm_max_file_bytes();
        if (global_cap > 0 && (size_t)global_cap < selected_design_cap) {
            selected_design_cap = (size_t)global_cap;
        }
        snapshot.selected_config_should_be_read =
            selected_present == 1 && selected_len > 0 && selected_len <= selected_design_cap;
    }
    if (bind_snapshot && file_count > 0) {
        uint32_t capacity = (uint32_t)file_count * 2U;
        snapshot.by_path = cbm_ht_create(capacity);
        if (!snapshot.by_path) {
            cbm_gbuf_free(staging);
            return -1;
        }
        for (int i = 0; i < file_count; i++) {
            if (ctx->source_version_files[i].rel_path) {
                (void)cbm_ht_set(snapshot.by_path, ctx->source_version_files[i].rel_path,
                                 &ctx->source_versions[i]);
            }
        }
    }

    cbm_design_index_opts_t opts = {
        .project_name = ctx->project_name,
        .repo_path = ctx->repo_path,
        .gbuf = staging,
        .files = files,
        .file_count = file_count,
        .mode = ctx->mode,
        .verify_source = bind_snapshot ? design_verify_snapshot_source : NULL,
        .verify_source_userdata = bind_snapshot ? &snapshot : NULL,
    };
    int rc = cbm_design_index(&opts);
    if (rc == 0 && bind_snapshot) {
        rc = design_verify_auxiliary_config(ctx, &snapshot);
    }
    if (rc != 0) {
        cbm_log_warn("pass.design_context_failed", "reason", "invalid_or_out_of_memory");
        cbm_ht_free(snapshot.by_path);
        cbm_gbuf_free(staging);
        return rc;
    }

    /* Swap the derived labels only after staging is complete. This keeps
     * nested-scope deletion deterministic without exposing a partial rebuild. */
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignToken");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignComponent");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignMode");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignSystem");
    rc = cbm_gbuf_merge(ctx->gbuf, staging);
    cbm_ht_free(snapshot.by_path);
    cbm_gbuf_free(staging);
    if (rc != 0) {
        cbm_log_warn("pass.design_context_failed", "reason", "staging_merge_failed");
    }
    return rc;
}
