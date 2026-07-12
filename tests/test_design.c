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

TEST(design_preserves_mode_specific_values_for_same_token_path) {
    char *base_raw = th_mktempdir("cbm_design_mode_values");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *light = "{\"color\":{\"surface\":{\"$type\":\"color\",\"$value\":\"#ffffff\"}}}";
    const char *dark = "{\"color\":{\"surface\":{\"$type\":\"color\",\"$value\":\"#111111\"}}}";
    const char *resolver =
        "{\"version\":\"2025.10\",\"modifiers\":{\"theme\":{\"default\":\"light\","
        "\"contexts\":{\"light\":[{\"$ref\":\"theme/light.tokens.json\"}],"
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
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 3,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *token = cbm_gbuf_find_by_qn(gb, "test.design.token.root.color.surface");
    const cbm_gbuf_node_t *light_mode =
        cbm_gbuf_find_by_qn(gb, "test.design.mode.root.theme.light");
    const cbm_gbuf_node_t *dark_mode = cbm_gbuf_find_by_qn(gb, "test.design.mode.root.theme.dark");
    ASSERT_NOT_NULL(token);
    ASSERT_NOT_NULL(light_mode);
    ASSERT_NOT_NULL(dark_mode);
    const cbm_gbuf_edge_t **light_edges = NULL;
    const cbm_gbuf_edge_t **dark_edges = NULL;
    int light_count = 0, dark_count = 0;
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, light_mode->id, "OVERRIDES", &light_edges,
                                                 &light_count),
              0);
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, dark_mode->id, "OVERRIDES", &dark_edges,
                                                 &dark_count),
              0);
    ASSERT_EQ(light_count, 1);
    ASSERT_EQ(dark_count, 1);
    ASSERT_EQ(light_edges[0]->target_id, token->id);
    ASSERT_EQ(dark_edges[0]->target_id, token->id);
    ASSERT_NOT_NULL(strstr(light_edges[0]->properties_json, "#ffffff"));
    ASSERT_NOT_NULL(strstr(light_edges[0]->properties_json, "\"default\":true"));
    ASSERT_NOT_NULL(strstr(dark_edges[0]->properties_json, "#111111"));
    ASSERT_NOT_NULL(strstr(dark_edges[0]->properties_json, "dark.tokens.json"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_indexes_dtcg_structural_references_and_root_tokens) {
    char *base_raw = th_mktempdir("cbm_design_dtcg_structure");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *tokens = "{\"base\":{\"$type\":\"color\",\"$value\":\"#123456\"},"
                         "\"alias\":{\"$type\":\"color\",\"$ref\":\"#/base\"},"
                         "\"semantic\":{\"$type\":\"color\",\"$extends\":\"#/base-group\","
                         "\"$root\":{\"$value\":\"#ffffff\"},\"muted\":{\"$value\":\"#eeeeee\"}}}";
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/core.tokens.json"), tokens), 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/design/core.tokens.json", base);
    cbm_file_info_t file = {.path = path,
                            .rel_path = "design/core.tokens.json",
                            .language = CBM_LANG_JSON,
                            .size = (int64_t)strlen(tokens)};
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = &file,
                                    .file_count = 1,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *alias = cbm_gbuf_find_by_qn(gb, "test.design.token.root.alias");
    const cbm_gbuf_node_t *root =
        cbm_gbuf_find_by_qn(gb, "test.design.token.root.semantic.dollar-root");
    const cbm_gbuf_node_t *muted = cbm_gbuf_find_by_qn(gb, "test.design.token.root.semantic.muted");
    ASSERT_NOT_NULL(alias);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(muted);
    ASSERT_NOT_NULL(strstr(alias->properties_json, "\"reference\":\"#/base\""));
    ASSERT_NOT_NULL(strstr(root->properties_json, "\"token_path\":\"semantic.$root\""));
    ASSERT_NOT_NULL(strstr(root->properties_json, "\"extends\":\"#/base-group\""));
    ASSERT_NOT_NULL(strstr(muted->properties_json, "\"extends\":\"#/base-group\""));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_indexes_google_typography_as_composite_token) {
    char *base_raw = th_mktempdir("cbm_design_typography");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *design_md = "---\nname: Type Test\ntypography:\n  h1:\n"
                            "    fontFamily: Inter\n    fontSize: 32px\n    fontWeight: 700\n---\n";
    ASSERT_EQ(th_write_file(TH_PATH(base, "DESIGN.md"), design_md), 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/DESIGN.md", base);
    cbm_file_info_t file = {.path = path,
                            .rel_path = "DESIGN.md",
                            .language = CBM_LANG_MARKDOWN,
                            .size = (int64_t)strlen(design_md)};
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = &file,
                                    .file_count = 1,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *typography =
        cbm_gbuf_find_by_qn(gb, "test.design.token.root.typography.h1");
    ASSERT_NOT_NULL(typography);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.typography.h1.fontFamily"));
    ASSERT_NOT_NULL(strstr(typography->properties_json, "\"composite\":true"));
    ASSERT_NOT_NULL(strstr(typography->properties_json, "fontFamily"));
    ASSERT_NOT_NULL(strstr(typography->properties_json, "fontSize"));
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
    ASSERT_EQ(cbm_pipeline_pass_design(&ctx, NULL, 1), -1);
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.space.sm"));
    ASSERT_EQ(cbm_pipeline_pass_design(&ctx, NULL, 0), 0);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.token.root.space.sm"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.root"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_skips_unreadable_oversized_and_short_read_documents) {
    char *base_raw = th_mktempdir("cbm_design_bad_docs");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    ASSERT_EQ(th_write_file(TH_PATH(base, "oversized/DESIGN.md"), "# oversized\n"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "short/DESIGN.md"), "abc"), 0);

    char missing[1024], oversized[1024], short_read[1024];
    snprintf(missing, sizeof(missing), "%s/missing/DESIGN.md", base);
    snprintf(oversized, sizeof(oversized), "%s/oversized/DESIGN.md", base);
    snprintf(short_read, sizeof(short_read), "%s/short/DESIGN.md", base);
    cbm_file_info_t files[] = {
        {.path = missing,
         .rel_path = "missing/DESIGN.md",
         .language = CBM_LANG_MARKDOWN,
         .size = 10},
        {.path = oversized,
         .rel_path = "oversized/DESIGN.md",
         .language = CBM_LANG_MARKDOWN,
         .size = 8 * 1024 * 1024 + 1},
        {.path = short_read,
         .rel_path = "short/DESIGN.md",
         .language = CBM_LANG_MARKDOWN,
         .size = 20},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 3,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.missing"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.oversized"));
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.short"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_glob_matching_is_bounded_for_adversarial_patterns) {
    char *base_raw = th_mktempdir("cbm_design_glob_bound");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);

    char pattern[256] = {0};
    char rel_path[128] = {0};
    for (int i = 0; i < 32; i++) {
        strncat(pattern, "*a", sizeof(pattern) - strlen(pattern) - 1);
        strncat(rel_path, "a", sizeof(rel_path) - strlen(rel_path) - 1);
    }
    strncat(pattern, "*b", sizeof(pattern) - strlen(pattern) - 1);
    strncat(rel_path, "c", sizeof(rel_path) - strlen(rel_path) - 1);
    char config[512];
    snprintf(config, sizeof(config), "{\"design\":{\"documents\":[\"%s\"]}}", pattern);
    ASSERT_EQ(th_write_file(TH_PATH(base, ".codebase-memory.json"), config), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, rel_path), "# not matched\n"), 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, rel_path);
    cbm_file_info_t file = {
        .path = path, .rel_path = rel_path, .language = CBM_LANG_MARKDOWN, .size = 14};
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = &file,
                                    .file_count = 1,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.root"));
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_bad_machine_sources_do_not_create_empty_context) {
    char *base_raw = th_mktempdir("cbm_design_bad_sources");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/malformed.tokens.json"), "{bad"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/short.css"), "abc"), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/oversized.scss"), "abc"), 0);

    char malformed[1024], missing_resolver[1024], short_css[1024], oversized_scss[1024];
    snprintf(malformed, sizeof(malformed), "%s/design/malformed.tokens.json", base);
    snprintf(missing_resolver, sizeof(missing_resolver), "%s/design/missing.resolver.json", base);
    snprintf(short_css, sizeof(short_css), "%s/src/short.css", base);
    snprintf(oversized_scss, sizeof(oversized_scss), "%s/src/oversized.scss", base);
    cbm_file_info_t files[] = {
        {.path = malformed,
         .rel_path = "design/malformed.tokens.json",
         .language = CBM_LANG_JSON,
         .size = 4},
        {.path = missing_resolver,
         .rel_path = "design/missing.resolver.json",
         .language = CBM_LANG_JSON,
         .size = 10},
        {.path = short_css, .rel_path = "src/short.css", .language = CBM_LANG_CSS, .size = 20},
        {.path = oversized_scss,
         .rel_path = "src/oversized.scss",
         .language = CBM_LANG_SCSS,
         .size = 8 * 1024 * 1024 + 1},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 4,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    ASSERT_NULL(cbm_gbuf_find_by_qn(gb, "test.design.system.root"));
    ASSERT_EQ(design_edge_count(gb, "DEFINES_TOKEN"), 0);
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

TEST(design_preserves_duplicate_token_definitions_and_canonical_source) {
    char *base_raw = th_mktempdir("cbm_design_duplicate_defs");
    ASSERT_NOT_NULL(base_raw);
    char base[1024];
    snprintf(base, sizeof(base), "%s", base_raw);
    const char *config = "{\"design\":{\"generated\":[\"src/generated/*.css\"]}}";
    const char *a_tokens = "{\"color\":{\"action\":{\"$type\":\"color\",\"$value\":\"#111111\"}}}";
    const char *z_tokens = "{\"color\":{\"action\":{\"$type\":\"color\",\"$value\":\"#222222\"}}}";
    const char *observed_css = ":root { --color-action: #333333; }\n";
    const char *generated_css = ":root { --color-action: #444444; }\n";
    ASSERT_EQ(th_write_file(TH_PATH(base, ".codebase-memory.json"), config), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/a.tokens.json"), a_tokens), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "design/z.tokens.json"), z_tokens), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/styles/local.css"), observed_css), 0);
    ASSERT_EQ(th_write_file(TH_PATH(base, "src/generated/tokens.css"), generated_css), 0);

    char p0[1024], p1[1024], p2[1024], p3[1024];
    snprintf(p0, sizeof(p0), "%s/design/z.tokens.json", base);
    snprintf(p1, sizeof(p1), "%s/src/generated/tokens.css", base);
    snprintf(p2, sizeof(p2), "%s/design/a.tokens.json", base);
    snprintf(p3, sizeof(p3), "%s/src/styles/local.css", base);
    /* Deliberately reverse priority/path order: canonical selection must not
     * depend on the caller's file-array ordering. */
    cbm_file_info_t files[] = {
        {.path = p0,
         .rel_path = "design/z.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(z_tokens)},
        {.path = p1,
         .rel_path = "src/generated/tokens.css",
         .language = CBM_LANG_CSS,
         .size = (int64_t)strlen(generated_css)},
        {.path = p2,
         .rel_path = "design/a.tokens.json",
         .language = CBM_LANG_JSON,
         .size = (int64_t)strlen(a_tokens)},
        {.path = p3,
         .rel_path = "src/styles/local.css",
         .language = CBM_LANG_CSS,
         .size = (int64_t)strlen(observed_css)},
    };
    cbm_gbuf_t *gb = cbm_gbuf_new("test", base);
    ASSERT_NOT_NULL(gb);
    for (int i = 0; i < 4; i++) {
        design_add_file_node(gb, "test", files[i].rel_path);
    }
    cbm_design_index_opts_t opts = {.project_name = "test",
                                    .repo_path = base,
                                    .gbuf = gb,
                                    .files = files,
                                    .file_count = 4,
                                    .mode = CBM_MODE_FULL};
    ASSERT_EQ(cbm_design_index(&opts), 0);
    const cbm_gbuf_node_t *token = cbm_gbuf_find_by_qn(gb, "test.design.token.root.color.action");
    ASSERT_NOT_NULL(token);
    ASSERT_STR_EQ(token->file_path, "design/a.tokens.json");
    ASSERT_NOT_NULL(strstr(token->properties_json, "#111111"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "#222222"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "#333333"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "#444444"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "\"definition_count\":4"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "\"ambiguous\":true"));
    ASSERT_NOT_NULL(strstr(token->properties_json, "\"canonical\":true"));
    ASSERT_EQ(design_edge_count(gb, "DEFINES_TOKEN"), 4);
    ASSERT_EQ(design_edge_count(gb, "GENERATED_AS"), 1);
    cbm_gbuf_free(gb);
    th_cleanup(base);
    PASS();
}

void suite_design(void) {
    RUN_TEST(design_indexes_dtcg_design_md_and_css);
    RUN_TEST(design_uses_nearest_nested_document_scope);
    RUN_TEST(design_generated_css_does_not_overwrite_authoritative_token);
    RUN_TEST(design_indexes_dtcg_resolver_modes_without_following_remote_or_parent_refs);
    RUN_TEST(design_preserves_mode_specific_values_for_same_token_path);
    RUN_TEST(design_indexes_dtcg_structural_references_and_root_tokens);
    RUN_TEST(design_indexes_google_typography_as_composite_token);
    RUN_TEST(design_pass_rebuild_removes_stale_incremental_context);
    RUN_TEST(design_skips_unreadable_oversized_and_short_read_documents);
    RUN_TEST(design_glob_matching_is_bounded_for_adversarial_patterns);
    RUN_TEST(design_bad_machine_sources_do_not_create_empty_context);
    RUN_TEST(design_preserves_duplicate_token_definitions_and_canonical_source);
}
