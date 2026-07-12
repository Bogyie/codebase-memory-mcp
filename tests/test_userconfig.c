/*
 * test_userconfig.c — Tests for user-defined extension→language mappings.
 *
 * Tests cbm_userconfig_load(), cbm_userconfig_lookup(), and the
 * cbm_set_user_lang_config() / cbm_language_for_extension() integration.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/sha256.h"
#include "test_framework.h"
#include "discover/discover.h"
#include "discover/userconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Write a JSON file to path. Returns 0 on success. */
static int write_json(const char *path, const char *json) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fputs(json, f);
    fclose(f);
    return 0;
}

/* ── Tests: project config ───────────────────────────────────────── */

TEST(userconfig_project_basic) {
    /* Write a .codebase-memory.json in a temp dir */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_proj_basic", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755); /* from compat_fs.h via compat.h */

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".blade.php"), CBM_LANG_PHP);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".go"), CBM_LANG_COUNT); /* not in user config */

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: global config ────────────────────────────────────────── */

TEST(userconfig_global_via_env) {
    /* Point config dir to a temp dir via the platform-appropriate env var:
     * XDG_CONFIG_HOME on Linux/macOS, APPDATA on Windows. */
    char cfg_dir[256];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/uctest_global_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", cfg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(write_json(global_path, "{\"extra_extensions\":{\".twig\":\"html\"}}"), 0);

#ifdef _WIN32
    char old_appdata[512] = "";
    cbm_safe_getenv("APPDATA", old_appdata, sizeof(old_appdata), NULL);
    cbm_setenv("APPDATA", cfg_dir, 1);
#else
    cbm_setenv("XDG_CONFIG_HOME", cfg_dir, 1);
#endif
    cbm_userconfig_t *cfg = cbm_userconfig_load(NULL); /* no project dir */
#ifdef _WIN32
    if (old_appdata[0]) {
        cbm_setenv("APPDATA", old_appdata, 1);
    } else {
        cbm_unsetenv("APPDATA");
    }
#else
    cbm_unsetenv("XDG_CONFIG_HOME");
#endif

    ASSERT_NOT_NULL(cfg);
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".twig"), CBM_LANG_HTML);

    cbm_userconfig_free(cfg);
    remove(global_path);
    PASS();
}

/* ── Tests: project wins over global ────────────────────────────── */

TEST(userconfig_project_wins_over_global) {
    /* Global says .xyz → python; project says .xyz → rust */
    char xdg_dir[256];
    snprintf(xdg_dir, sizeof(xdg_dir), "%s/uctest_priority_xdg", cbm_tmpdir());

    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", xdg_dir);
    cbm_mkdir_p(app_dir, 0755);

    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);
    ASSERT_EQ(write_json(global_path, "{\"extra_extensions\":{\".xyz\":\"python\"}}"), 0);

    char proj_dir[256];
    snprintf(proj_dir, sizeof(proj_dir), "%s/uctest_priority_proj", cbm_tmpdir());
    cbm_mkdir_p(proj_dir, 0755);

    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/.codebase-memory.json", proj_dir);
    ASSERT_EQ(write_json(proj_path, "{\"extra_extensions\":{\".xyz\":\"rust\"}}"), 0);

    cbm_setenv("XDG_CONFIG_HOME", xdg_dir, 1);
    cbm_userconfig_t *cfg = cbm_userconfig_load(proj_dir);
    cbm_unsetenv("XDG_CONFIG_HOME");

    ASSERT_NOT_NULL(cfg);
    /* Project definition (rust) must win */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".xyz"), CBM_LANG_RUST);

    cbm_userconfig_free(cfg);
    remove(global_path);
    remove(proj_path);
    PASS();
}

/* ── Tests: unknown language values are skipped ──────────────────── */

TEST(userconfig_unknown_lang_skipped) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_unknown_lang", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    /* "klingon" is not a valid language; ".wasm" should be silently skipped */
    ASSERT_EQ(
        write_json(proj, "{\"extra_extensions\":{\".wasm\":\"klingon\",\".mjs\":\"javascript\"}}"),
        0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* .wasm with unknown lang → not in config */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".wasm"), CBM_LANG_COUNT);
    /* .mjs with valid lang → present */
    ASSERT_EQ(cbm_userconfig_lookup(cfg, ".mjs"), CBM_LANG_JAVASCRIPT);

    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: missing files are silently ignored ───────────────────── */

TEST(userconfig_missing_files_ok) {
    /* Point to a non-existent repo dir */
    cbm_userconfig_t *cfg = cbm_userconfig_load("/tmp/__nonexistent_repo_12345__");
    ASSERT_NOT_NULL(cfg); /* must not return NULL — just empty */
    ASSERT_EQ(cfg->count, 0);
    cbm_userconfig_free(cfg);
    PASS();
}

TEST(userconfig_missing_config_home_does_not_use_shared_tmp) {
    bool shared_fixture_ok = true;
    static const char *const env_names[] = {
        "XDG_CONFIG_HOME",
        "APPDATA",
        "HOME",
        "USERPROFILE",
    };
    char old_values[sizeof(env_names) / sizeof(env_names[0])][1024];
    bool had_values[sizeof(env_names) / sizeof(env_names[0])] = {false};
    for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++) {
        old_values[i][0] = '\0';
        had_values[i] =
            cbm_safe_getenv(env_names[i], old_values[i], sizeof(old_values[i]), NULL) != NULL;
        cbm_unsetenv(env_names[i]);
    }

#ifndef _WIN32
    /* Exercise the historical fallback, not only the no-file case. Create the
     * shared path only when this test exclusively owns its directory, so an
     * existing user path is never modified. */
    const char *shared_dir = "/tmp/codebase-memory-mcp";
    const char *shared_config = "/tmp/codebase-memory-mcp/config.json";
    bool created_shared_dir = mkdir(shared_dir, 0700) == 0;
    bool planted_shared_config = false;
    if (created_shared_dir) {
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(shared_config, flags, 0600);
        if (fd >= 0) {
            static const char payload[] =
                "{\"extra_extensions\":{\".sharedtmpattack\":\"rust\"}}\n";
            ssize_t written = write(fd, payload, sizeof(payload) - 1U);
            int close_rc = close(fd);
            planted_shared_config = written == (ssize_t)(sizeof(payload) - 1U) && close_rc == 0;
            if (!planted_shared_config) {
                (void)unlink(shared_config);
            }
        }
    }
    shared_fixture_ok = !created_shared_dir || planted_shared_config;
#endif

    cbm_userconfig_snapshot_t *snapshot = NULL;
    cbm_userconfig_t *cfg = cbm_userconfig_load_with_snapshot(NULL, &snapshot);
    bool shared_tmp_ignored =
        cfg && cbm_userconfig_lookup(cfg, ".sharedtmpattack") == CBM_LANG_COUNT;

#ifndef _WIN32
    if (planted_shared_config) {
        (void)unlink(shared_config);
    }
    if (created_shared_dir) {
        (void)rmdir(shared_dir);
    }
#endif

    for (size_t i = 0; i < sizeof(env_names) / sizeof(env_names[0]); i++) {
        if (had_values[i]) {
            cbm_setenv(env_names[i], old_values[i], 1);
        } else {
            cbm_unsetenv(env_names[i]);
        }
    }

    ASSERT_NOT_NULL(cfg);
    ASSERT_NOT_NULL(snapshot);
    ASSERT_TRUE(shared_fixture_ok);
    ASSERT_TRUE(shared_tmp_ignored);
    ASSERT_EQ(cfg->count, 0);
    ASSERT_EQ(cbm_userconfig_snapshot_verify(snapshot), 0);
    ASSERT_EQ((int)strlen(cbm_userconfig_snapshot_fingerprint(snapshot)), 64);
    cbm_userconfig_free(cfg);
    cbm_userconfig_snapshot_free(snapshot);
    PASS();
}

/* ── Tests: integration with cbm_language_for_extension ─────────── */

TEST(userconfig_integration_override) {
    /* Verify that setting the global config makes cbm_language_for_extension
     * respect the override. We map ".blade.php" → PHP, which is not in the
     * built-in table. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/uctest_integ", cbm_tmpdir());
    cbm_mkdir_p(dir, 0755);

    char proj[512];
    snprintf(proj, sizeof(proj), "%s/.codebase-memory.json", dir);
    ASSERT_EQ(write_json(proj, "{\"extra_extensions\":{\".blade.php\":\"php\"}}"), 0);

    cbm_userconfig_t *cfg = cbm_userconfig_load(dir);
    ASSERT_NOT_NULL(cfg);

    /* Before setting, .blade.php is unknown */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_COUNT);

    cbm_set_user_lang_config(cfg);
    /* After setting, .blade.php → PHP */
    ASSERT_EQ(cbm_language_for_extension(".blade.php"), CBM_LANG_PHP);
    /* Built-in extensions still work */
    ASSERT_EQ(cbm_language_for_extension(".go"), CBM_LANG_GO);

    /* Clean up global state */
    cbm_set_user_lang_config(NULL);
    cbm_userconfig_free(cfg);
    remove(proj);
    PASS();
}

/* ── Tests: free is NULL-safe ────────────────────────────────────── */

TEST(userconfig_free_null) {
    cbm_userconfig_free(NULL); /* must not crash */
    PASS();
}

TEST(userconfig_snapshot_tracks_both_sources_and_missing_state) {
    char config_home[256];
    snprintf(config_home, sizeof(config_home), "%s/uctest_snapshot_home", cbm_tmpdir());
    char app_dir[512];
    snprintf(app_dir, sizeof(app_dir), "%s/codebase-memory-mcp", config_home);
    cbm_mkdir_p(app_dir, 0755);
    char global_path[768];
    snprintf(global_path, sizeof(global_path), "%s/config.json", app_dir);

    char repo[256];
    snprintf(repo, sizeof(repo), "%s/uctest_snapshot_repo", cbm_tmpdir());
    cbm_mkdir_p(repo, 0755);
    char project_path[512];
    snprintf(project_path, sizeof(project_path), "%s/.codebase-memory.json", repo);

#ifdef _WIN32
    const char *config_env = "APPDATA";
#else
    const char *config_env = "XDG_CONFIG_HOME";
#endif
    char old_config_home[1024] = "";
    bool had_old_config_home =
        cbm_safe_getenv(config_env, old_config_home, sizeof(old_config_home), NULL) != NULL;
    cbm_setenv(config_env, config_home, 1);

    ASSERT_EQ(write_json(global_path, "{\"extra_extensions\":{\".same\":\"python\"}}"), 0);
    ASSERT_EQ(write_json(project_path, "{\"extra_extensions\":{\".same\":\"rust\"}}"), 0);

    cbm_userconfig_snapshot_t *first_snapshot = NULL;
    cbm_userconfig_t *first = cbm_userconfig_load_with_snapshot(repo, &first_snapshot);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(first_snapshot);
    ASSERT_EQ(cbm_userconfig_lookup(first, ".same"), CBM_LANG_RUST);
    const char *first_fp = cbm_userconfig_snapshot_fingerprint(first_snapshot);
    ASSERT_NOT_NULL(first_fp);
    ASSERT_EQ((int)strlen(first_fp), 64);
    char first_fp_copy[65];
    snprintf(first_fp_copy, sizeof(first_fp_copy), "%s", first_fp);
    ASSERT_EQ(cbm_userconfig_snapshot_verify(first_snapshot), 0);

    /* The project override keeps the effective mapping unchanged, but the
     * global source content is still part of the selected config generation. */
    ASSERT_EQ(write_json(global_path, "{\"extra_extensions\":{\".same\":\"javascript\"}}"), 0);
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(first_snapshot), 0);

    cbm_userconfig_snapshot_t *second_snapshot = NULL;
    cbm_userconfig_t *second = cbm_userconfig_load_with_snapshot(repo, &second_snapshot);
    ASSERT_NOT_NULL(second);
    ASSERT_NOT_NULL(second_snapshot);
    ASSERT_EQ(cbm_userconfig_lookup(second, ".same"), CBM_LANG_RUST);
    ASSERT_TRUE(strcmp(first_fp_copy, cbm_userconfig_snapshot_fingerprint(second_snapshot)) != 0);
    ASSERT_EQ(cbm_userconfig_snapshot_verify(second_snapshot), 0);

    /* Presence is domain-separated from content: deleting the ignored project
     * config invalidates the old snapshot and produces a third fingerprint. */
    char second_fp_copy[65];
    snprintf(second_fp_copy, sizeof(second_fp_copy), "%s",
             cbm_userconfig_snapshot_fingerprint(second_snapshot));
    ASSERT_EQ(cbm_unlink(project_path), 0);
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(second_snapshot), 0);
    cbm_userconfig_snapshot_t *third_snapshot = NULL;
    cbm_userconfig_t *third = cbm_userconfig_load_with_snapshot(repo, &third_snapshot);
    ASSERT_NOT_NULL(third);
    ASSERT_NOT_NULL(third_snapshot);
    ASSERT_TRUE(strcmp(second_fp_copy, cbm_userconfig_snapshot_fingerprint(third_snapshot)) != 0);
    ASSERT_EQ(cbm_userconfig_lookup(third, ".same"), CBM_LANG_JAVASCRIPT);

    cbm_userconfig_free(first);
    cbm_userconfig_free(second);
    cbm_userconfig_free(third);
    cbm_userconfig_snapshot_free(first_snapshot);
    cbm_userconfig_snapshot_free(second_snapshot);
    cbm_userconfig_snapshot_free(third_snapshot);
    cbm_unlink(global_path);
    if (had_old_config_home) {
        cbm_setenv(config_env, old_config_home, 1);
    } else {
        cbm_unsetenv(config_env);
    }
    PASS();
}

TEST(userconfig_project_source_is_rooted_and_unreadable_state_is_stable) {
#ifdef _WIN32
    SKIP_PLATFORM("symlink/FIFO fixture is POSIX-specific");
#else
    char repo[256];
    char outside_dir[256];
    char config_home[256];
    snprintf(repo, sizeof(repo), "%s/uctest_rooted_repo", cbm_tmpdir());
    snprintf(outside_dir, sizeof(outside_dir), "%s/uctest_rooted_outside", cbm_tmpdir());
    snprintf(config_home, sizeof(config_home), "%s/uctest_rooted_home", cbm_tmpdir());
    cbm_mkdir_p(repo, 0755);
    cbm_mkdir_p(outside_dir, 0755);
    cbm_mkdir_p(config_home, 0755);
    char outside[512];
    char project[512];
    snprintf(outside, sizeof(outside), "%s/outside.json", outside_dir);
    snprintf(project, sizeof(project), "%s/.codebase-memory.json", repo);
    ASSERT_EQ(write_json(outside, "{\"extra_extensions\":{\".outside\":\"rust\"}}"), 0);
    (void)cbm_unlink(project);
    ASSERT_EQ(symlink(outside, project), 0);

    char old_home[1024] = "";
    bool had_old = cbm_safe_getenv("XDG_CONFIG_HOME", old_home, sizeof(old_home), NULL) != NULL;
    cbm_setenv("XDG_CONFIG_HOME", config_home, 1);
    cbm_userconfig_snapshot_t *snapshot = NULL;
    cbm_userconfig_t *config = cbm_userconfig_load_with_snapshot(repo, &snapshot);
    ASSERT_NOT_NULL(config);
    ASSERT_NOT_NULL(snapshot);
    ASSERT_EQ(cbm_userconfig_lookup(config, ".outside"), CBM_LANG_COUNT);
    const char *project_sha = (const char *)(uintptr_t)1;
    size_t project_len = 99;
    ASSERT_EQ(cbm_userconfig_snapshot_project_source(snapshot, &project_sha, &project_len), 0);
    ASSERT_NULL(project_sha);
    ASSERT_EQ(project_len, 0);
    /* The symlink is an explicit unreadable generation, not silently absent. */
    ASSERT_EQ(cbm_userconfig_snapshot_verify(snapshot), 0);
    ASSERT_EQ(cbm_unlink(project), 0);
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(snapshot), 0);
    cbm_userconfig_free(config);
    cbm_userconfig_snapshot_free(snapshot);

    /* A FIFO must likewise be classified without a blocking open, and its
     * unreadable->missing transition must invalidate the snapshot. */
    ASSERT_EQ(mkfifo(project, 0600), 0);
    snapshot = NULL;
    config = cbm_userconfig_load_with_snapshot(repo, &snapshot);
    ASSERT_NOT_NULL(config);
    ASSERT_NOT_NULL(snapshot);
    ASSERT_EQ(cbm_userconfig_snapshot_verify(snapshot), 0);
    ASSERT_EQ(cbm_unlink(project), 0);
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(snapshot), 0);
    cbm_userconfig_free(config);
    cbm_userconfig_snapshot_free(snapshot);

    cbm_unlink(outside);
    cbm_rmdir(outside_dir);
    cbm_rmdir(repo);
    cbm_rmdir(config_home);
    if (had_old) {
        cbm_setenv("XDG_CONFIG_HOME", old_home, 1);
    } else {
        cbm_unsetenv("XDG_CONFIG_HOME");
    }
    PASS();
#endif
}

TEST(userconfig_auxiliary_state_distinguishes_optional_absence_and_tamper) {
    char repo[256];
    char config_home[256];
    snprintf(repo, sizeof(repo), "%s/uctest_aux_state_repo", cbm_tmpdir());
    snprintf(config_home, sizeof(config_home), "%s/uctest_aux_state_home", cbm_tmpdir());
    cbm_mkdir_p(repo, 0755);
    cbm_mkdir_p(config_home, 0755);

#ifdef _WIN32
    const char *config_env = "APPDATA";
#else
    const char *config_env = "XDG_CONFIG_HOME";
#endif
    char old_config_home[1024] = "";
    bool had_old_config_home =
        cbm_safe_getenv(config_env, old_config_home, sizeof(old_config_home), NULL) != NULL;
    cbm_setenv(config_env, config_home, 1);

    cbm_userconfig_snapshot_t *absent_snapshot = NULL;
    cbm_userconfig_t *absent_config = cbm_userconfig_load_with_snapshot(repo, &absent_snapshot);
    ASSERT_NOT_NULL(absent_config);
    ASSERT_NOT_NULL(absent_snapshot);
    ASSERT_EQ(cbm_userconfig_snapshot_capture_auxiliary(absent_snapshot, repo, NULL, 0), 0);
    ASSERT_EQ(cbm_userconfig_snapshot_auxiliary_source_state(absent_snapshot, "Cargo.toml"),
              CBM_USERCONFIG_AUX_SOURCE_ABSENT);
    ASSERT_EQ(cbm_userconfig_snapshot_verify(absent_snapshot), 0);

    char cargo_path[512];
    snprintf(cargo_path, sizeof(cargo_path), "%s/Cargo.toml", repo);
    ASSERT_EQ(write_json(cargo_path, "[package]\nname = \"fixture\"\nversion = \"0.1.0\"\n"), 0);
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(absent_snapshot), 0);

    cbm_userconfig_snapshot_t *present_snapshot = NULL;
    cbm_userconfig_t *present_config = cbm_userconfig_load_with_snapshot(repo, &present_snapshot);
    ASSERT_NOT_NULL(present_config);
    ASSERT_NOT_NULL(present_snapshot);
    ASSERT_EQ(cbm_userconfig_snapshot_capture_auxiliary(present_snapshot, repo, NULL, 0), 0);
    ASSERT_EQ(cbm_userconfig_snapshot_auxiliary_source_state(present_snapshot, "Cargo.toml"),
              CBM_USERCONFIG_AUX_SOURCE_PRESENT);

    char wrong_sha256[CBM_SHA256_HEX_LEN + 1];
    memset(wrong_sha256, '0', CBM_SHA256_HEX_LEN);
    wrong_sha256[CBM_SHA256_HEX_LEN] = '\0';
    ASSERT_NEQ(cbm_userconfig_snapshot_verify_auxiliary_source(present_snapshot, "Cargo.toml",
                                                               wrong_sha256),
               0);
    /* A loader that observes different bytes makes the generation sticky
     * invalid even if the path is restored before the final rescan. */
    ASSERT_NEQ(cbm_userconfig_snapshot_verify(present_snapshot), 0);

    cbm_userconfig_free(absent_config);
    cbm_userconfig_free(present_config);
    cbm_userconfig_snapshot_free(absent_snapshot);
    cbm_userconfig_snapshot_free(present_snapshot);
    cbm_unlink(cargo_path);
    cbm_rmdir(repo);
    cbm_rmdir(config_home);
    if (had_old_config_home) {
        cbm_setenv(config_env, old_config_home, 1);
    } else {
        cbm_unsetenv(config_env);
    }
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────── */

SUITE(userconfig) {
    RUN_TEST(userconfig_project_basic);
    RUN_TEST(userconfig_global_via_env);
    RUN_TEST(userconfig_project_wins_over_global);
    RUN_TEST(userconfig_unknown_lang_skipped);
    RUN_TEST(userconfig_missing_files_ok);
    RUN_TEST(userconfig_missing_config_home_does_not_use_shared_tmp);
    RUN_TEST(userconfig_integration_override);
    RUN_TEST(userconfig_free_null);
    RUN_TEST(userconfig_snapshot_tracks_both_sources_and_missing_state);
    RUN_TEST(userconfig_project_source_is_rooted_and_unreadable_state_is_stable);
    RUN_TEST(userconfig_auxiliary_state_distinguishes_optional_absence_and_tamper);
}
