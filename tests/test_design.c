#include "test_framework.h"
#include "test_helpers.h"

#include "design/design.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"

#include <stdlib.h>
#include <string.h>

static void design_add_file_node(cbm_gbuf_t *gb, const char *project, const char *rel_path) {
    char *qn = cbm_pipeline_fqn_compute(project, rel_path, "__file__");
    if (qn) {
        cbm_gbuf_upsert_node(gb, "File", rel_path, qn, rel_path, 1, 1, "{}");
        free(qn);
    }
}

static int design_edge_count(cbm_gbuf_t *gb, const char *type) {
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    (void)cbm_gbuf_find_edges_by_type(gb, type, &edges, &count);
    return count;
}

TEST(design_indexes_dtcg_design_md_and_css) {
    char *base_raw = th_mktempdir("cbm_design_graph");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);

    const char *design_md = "---\n"
                            "version: alpha\n"
                            "name: Aurora\n"
                            "colors:\n"
                            "  primary: \"#2255ff\"\n"
                            "components:\n"
                            "  button-primary:\n"
                            "    backgroundColor: \"{colors.primary}\"\n"
                            "---\n"
                            "## Colors\nUse blue for the primary action.\n";
    const char *tokens = "{\"color\":{\"$type\":\"color\",\"base\":{\"$value\":\"#112233\"},"
                         "\"action\":{\"$value\":\"{color.base}\",\"$description\":\"CTA\"}}}";
    const char *css = ":root { --surface-color: #fff; }\n.button { color: var(--color-action); }\n";
    ASSERT_EQ(th_write_file(TH_PATH(base, "DESIGN.md"), design_md), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/tokens/core.tokens.json"), tokens), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/styles/app.css"), css), 0);

    char p0[1024], p1[1024], p2[1024];
    snprintf(p0, sizeof(p0), "%s/DESIGN.md", base);
    snprintf(p1, sizeof(p1), "%s/design/tokens/core.tokens.json", base);
    snprintf(p2, sizeof(p2), "%s/src/styles/app.css", base);
    cbm_file_info_t files[] = {
        {.path = p0,
         .rel_path = "DESIGN.md",
         .language = CBM_LANG_MARKDOWN,
         .size = (int64_t)strlen(design_md)},
        {.path = p1,
         .rel_path = "design/tokens/core.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(tokens)},
        {.path = p2,
         .rel_path = "src/styles/app.css",
         .language = CBM_LANG_CSS,
         .size = (int64_t)strlen(css)},
    };

    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    for (int i = 0; i < 3; i++) {
        design_add_file_node(gb, "test", files[i].rel_path);
    }
    cbm_gbuf_upsert_node(gb, "Section", "Colors", "test.design.colors", "DESIGN.md", 10, 11, "{}");
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 3,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);

    const cbm_gbuf_node_t *system = cbm_gbuf_find_by_qn(gb, "test.design.system.root");
    ASSERT_NOT_NULL(system);
    ASSERT_STR_EQ(system->name, "Aurora");
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.colors.primary"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.color.base"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.color.action"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.surface.color"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.component.root.button-primary"));
    ASSERT_GTE(design_edge_count(gb, "ALIASES_TO"), 2);
    ASSERT_GTE(design_edge_count(gb, "USES_TOKEN"), 1);
    ASSERT_GTE(design_edge_count(gb, "DOCUMENTED_BY"), 1);
    ASSERT_GTE(design_edge_count(gb, "GUIDED_BY"), 1);
    ASSERT_EQ(design_edge_count(gb, "GENERATED_AS"), 0);

    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_uses_nearest_nested_document_scope) {
    char *base_raw = th_mktempdir("cbm_design_scope");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *doc = "## Overview\nNested product.\n";
    const char *tokens = "{\"spacing\":{\"sm\":{\"$type\":\"dimension\",\"$value\":\"8px\"}}}";
    ASSERT_EQ(th_write_file(TH_PATH(base, "DESIGN.md"), "## Overview\nRoot.\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "packages/app/DESIGN.md"), doc), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "packages/app/design/app.tokens.json"), tokens), 0);

    char p0[1024], p1[1024], p2[1024];
    snprintf(p0, sizeof(p0), "%s/DESIGN.md", base);
    snprintf(p1, sizeof(p1), "%s/packages/app/DESIGN.md", base);
    snprintf(p2, sizeof(p2), "%s/packages/app/design/app.tokens.json", base);
    cbm_file_info_t files[] = {
        {.path = p0, .rel_path = "DESIGN.md", .language = CBM_LANG_MARKDOWN, .size = 18},
        {.path = p1,
         .rel_path = "packages/app/DESIGN.md",
         .language = CBM_LANG_MARKDOWN,
         .size = (int64_t)strlen(doc)},
        {.path = p2,
         .rel_path = "packages/app/design/app.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(tokens)},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    for (int i = 0; i < 3; i++) {
        design_add_file_node(gb, "test", files[i].rel_path);
    }
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 3,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.packages.app"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.packages.app.spacing.sm"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.spacing.sm"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_generated_css_does_not_overwrite_authoritative_token) {
    char *base_raw = th_mktempdir("cbm_design_generated");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *config = "{\"design\":{\"generated\":[\"src/generated/*.css\"],"
                         "\"token_sources\":[\"design/tokens/*.tokens.json\"]}}";
    const char *tokens = "{\"color\":{\"action\":{\"$type\":\"color\",\"$value\":\"#123456\"}}}";
    const char *css = ":root { --color-action: #abcdef; }\n";
    ASSERT_EQ(th_write_file(TH_PATH(base, ".codebase-memory.json"), config), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/tokens/core.tokens.json"), tokens), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/generated/tokens.css"), css), 0);

    char p0[1024], p1[1024];
    snprintf(p0, sizeof(p0), "%s/design/tokens/core.tokens.json", base);
    snprintf(p1, sizeof(p1), "%s/src/generated/tokens.css", base);
    cbm_file_info_t files[] = {
        {.path = p0,
         .rel_path = "design/tokens/core.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(tokens)},
        {.path = p1,
         .rel_path = "src/generated/tokens.css",
         .language = CBM_LANG_CSS,
         .size = (int64_t)strlen(css)},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    for (int i = 0; i < 2; i++) {
        design_add_file_node(gb, "test", files[i].rel_path);
    }
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 2,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *token = cbm_gbuf_find_by_qn(gb, "test.design.token.root.color.action");
    ASSERT_NOT_NULL(token);
    ASSERT_NOT_NULL(strstr(token->properties_json, "#123456"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "\"authoritative\":true"));
    ASSERT_GTE(design_edge_count(gb, "GENERATED_AS"), 1);
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_indexes_dtcg_resolver_modes_without_following_remote_or_parent_refs) {
    char *base_raw = th_mktempdir("cbm_design_resolver");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *light = "{\"color\":{\"light\":{\"surface\":{\"$type\":\"color\","
                        "\"$value\":\"#ffffff\"}}}}";
    const char *dark = "{\"color\":{\"dark\":{\"surface\":{\"$type\":\"color\","
                       "\"$value\":\"#111111\"}}}}";
    const char *resolver =
        "{\"version\":\"2025.10\",\"modifiers\":{\"theme\":{"
        "\"description\":\"Color scheme\",\"default\":\"light\",\"contexts\":{"
        "\"light\":[{\"$ref\":\"theme/light.tokens.json\"},{\"$ref\":"
        "\"https://example.test/remote.tokens.json\"},{\"$ref\":\"../../escape.tokens.json\"}],"
        "\"dark\":[{\"$ref\":\"theme/dark.tokens.json\"}]}}},"
        "\"resolutionOrder\":[{\"$ref\":\"#/modifiers/theme\"}]}";
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/theme/light.tokens.json"), light), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/theme/dark.tokens.json"), dark), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/theme.resolver.json"), resolver), 0);

    char p0[1024], p1[1024], p2[1024];
    snprintf(p0, sizeof(p0), "%s/design/theme/light.tokens.json", base);
    snprintf(p1, sizeof(p1), "%s/design/theme/dark.tokens.json", base);
    snprintf(p2, sizeof(p2), "%s/design/theme.resolver.json", base);
    cbm_file_info_t files[] = {
        {.path = p0,
         .rel_path = "design/theme/light.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(light)},
        {.path = p1,
         .rel_path = "design/theme/dark.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(dark)},
        {.path = p2,
         .rel_path = "design/theme.resolver.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(resolver)},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    for (int i = 0; i < 3; i++) {
        design_add_file_node(gb, "test", files[i].rel_path);
    }
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 3,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *light_mode =
        cbm_gbuf_find_by_qn(gb, "test.design.mode.root.theme.light");
    ASSERT_NOT_NULL(light_mode);
    ASSERT_NOT_NULL(strstr(light_mode->properties_json, "\"default\":true"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.mode.root.theme.dark"));
    ASSERT_EQ(design_edge_count(gb, "OVERRIDES"), 2);
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_pass_rebuild_removes_stale_incremental_context) {
    char *base_raw = th_mktempdir("cbm_design_rebuild");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *tokens = "{\"space\":{\"sm\":{\"$type\":\"dimension\",\"$value\":\"8px\"}}}";
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/core.tokens.json"), tokens), 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/design/core.tokens.json", base);
    cbm_file_info_t file = {.path = path,
                            .rel_path = "design/core.tokens.json",
                            .language = CBM_LANG_JSON,
                            .size = (int64_t)strlen(tokens)};
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    design_add_file_node(gb, "test", file.rel_path);
    cbm_pipeline_ctx_t ctx = {
        .project_name = "test", .repo_path = base, .gbuf = gb, .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_pipeline_pass_design(&ctx, &file, 1), 0);
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.space.sm"));
    ASSERT_EQ(cbm_pipeline_pass_design(&ctx, NULL, 0), 0);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.space.sm"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.root"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

void suite_design(void) {
    RUN_TEST(design_indexes_dtcg_design_md_and_css);
    RUN_TEST(design_uses_nearest_nested_document_scope);
    RUN_TEST(design_generated_css_does_not_overwrite_authoritative_token);
    RUN_TEST(design_indexes_dtcg_resolver_modes_without_following_remote_or_parent_refs);
    RUN_TEST(design_pass_rebuild_removes_stale_incremental_context);
}
