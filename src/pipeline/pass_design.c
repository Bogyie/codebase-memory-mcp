#include "pipeline/pipeline_internal.h"

#include "design/design.h"
#include "foundation/log.h"

int cbm_pipeline_pass_design(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                             int file_count) {
    if (!ctx || !ctx->gbuf || file_count < 0 || (!files && file_count > 0)) {
        return -1;
    }

    /* Rebuild the small derived subgraph as one unit. This makes incremental
     * scope changes (for example deleting a nested DESIGN.md) deterministic and
     * prevents stale aliases/usages from surviving. */
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignToken");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignComponent");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignMode");
    (void)cbm_gbuf_delete_by_label(ctx->gbuf, "DesignSystem");

    cbm_design_index_opts_t opts = {
        .project_name = ctx->project_name,
        .repo_path = ctx->repo_path,
        .gbuf = ctx->gbuf,
        .files = files,
        .file_count = file_count,
        .mode = ctx->mode,
    };
    int rc = cbm_design_index(&opts);
    if (rc != 0) {
        cbm_log_warn("pass.design_context_failed", "reason", "invalid_or_out_of_memory");
    }
    return rc;
}
