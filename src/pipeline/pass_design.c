#include "pipeline/pipeline_internal.h"

#include "design/design.h"
#include "foundation/log.h"

typedef struct {
    cbm_gbuf_t *staging;
    bool failed;
} design_seed_ctx_t;

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

    cbm_design_index_opts_t opts = {
        .project_name = ctx->project_name,
        .repo_path = ctx->repo_path,
        .gbuf = staging,
        .files = files,
        .file_count = file_count,
        .mode = ctx->mode,
    };
    int rc = cbm_design_index(&opts);
    if (rc != 0) {
        cbm_log_warn("pass.design_context_failed", "reason", "invalid_or_out_of_memory");
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
    cbm_gbuf_free(staging);
    if (rc != 0) {
        cbm_log_warn("pass.design_context_failed", "reason", "staging_merge_failed");
    }
    return rc;
}
