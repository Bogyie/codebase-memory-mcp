/*
 * test_platform.c — RED phase tests for foundation/platform.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/foundation/compat.h" /* cbm_setenv / cbm_unsetenv (Windows-portable) */
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/platform.h"
#include "../src/foundation/system_info_internal.h"
#include <stdlib.h>
#include <unistd.h>

#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#endif

#ifdef __linux__
/* Linux-only cgroup tests need stdio for FILE*, stdlib for mkdtemp,
 * string for strncpy/strchr, sys/stat for mkdir, dirent for the
 * shell-free recursive teardown. */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#endif

typedef struct {
    const char *name;
    char *value;
    bool was_set;
} platform_env_snapshot_t;

static bool platform_save_env(platform_env_snapshot_t *snapshot, const char *name) {
    const char *value = getenv(name);
    snapshot->name = name;
    snapshot->was_set = value != NULL;
    snapshot->value = value ? cbm_strdup(value) : NULL;
    return !value || snapshot->value != NULL;
}

static void platform_restore_env(platform_env_snapshot_t *snapshot) {
    if (snapshot->was_set) {
        (void)cbm_setenv(snapshot->name, snapshot->value, 1);
    } else {
        (void)cbm_unsetenv(snapshot->name);
    }
    free(snapshot->value);
    snapshot->value = NULL;
}

TEST(platform_now_ns) {
    uint64_t t1 = cbm_now_ns();
    ASSERT_GT(t1, 0);
    /* Busy-wait a tiny bit */
    for (volatile int i = 0; i < 100000; i++) {}
    uint64_t t2 = cbm_now_ns();
    ASSERT_GT(t2, t1);
    PASS();
}

TEST(platform_qpc_scaling_avoids_intermediate_overflow) {
    const uint64_t frequency = 10000000ULL;
    const uint64_t ticks = UINT64_MAX / 1000000000ULL + 1234567ULL;
    const uint64_t expected =
        (ticks / frequency) * 1000000000ULL + ((ticks % frequency) * 1000000000ULL) / frequency;
    ASSERT_GT(ticks, UINT64_MAX / 1000000000ULL);
    ASSERT_EQ(cbm_qpc_ticks_to_ns(ticks, frequency), expected);
    ASSERT_EQ(cbm_qpc_ticks_to_ns(ticks, 0), 0);
    PASS();
}

TEST(platform_posix_resolvers_preserve_backslash) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX path semantics");
#else
    char saved[1024] = "";
    bool had_saved = cbm_safe_getenv("CBM_CACHE_DIR", saved, sizeof(saved), NULL) != NULL;
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", "/tmp/a\\b", 1), 0);
    ASSERT_STR_EQ(cbm_resolve_cache_dir(), "/tmp/a\\b");
    if (had_saved) {
        ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", saved, 1), 0);
    } else {
        ASSERT_EQ(cbm_unsetenv("CBM_CACHE_DIR"), 0);
    }
    PASS();
#endif
}

#ifdef _WIN32
TEST(platform_safe_getenv_decodes_wide_unicode) {
    static const wchar_t name[] = L"CBM_TEST_PLATFORM_WIDE_ENV";
    wchar_t saved[32768];
    DWORD saved_len = GetEnvironmentVariableW(name, saved, 32768U);
    bool had_saved = saved_len > 0 && saved_len < 32768U;
    ASSERT_TRUE(SetEnvironmentVariableW(name, L"C:\\사용자\\캐시") != FALSE);
    char value[256];
    ASSERT_NOT_NULL(cbm_safe_getenv("CBM_TEST_PLATFORM_WIDE_ENV", value, sizeof(value), NULL));
    ASSERT_STR_EQ(value, "C:\\사용자\\캐시");
    (void)SetEnvironmentVariableW(name, had_saved ? saved : NULL);
    PASS();
}
#endif

TEST(platform_now_ms) {
    uint64_t t1 = cbm_now_ms();
    ASSERT_GT(t1, 0);
    PASS();
}

TEST(platform_nprocs) {
    int n = cbm_nprocs();
    ASSERT_GT(n, 0);
    ASSERT_LT(n, 10000); /* sanity */
    PASS();
}

TEST(platform_file_exists) {
    /* This test file should exist */
    ASSERT_TRUE(cbm_file_exists("tests/test_platform.c"));
    ASSERT_FALSE(cbm_file_exists("nonexistent_file_xyz.txt"));
    PASS();
}

TEST(platform_mkdir_p_rejects_file_components) {
    char *base = th_mktempdir("cbm_mkdir_p");
    ASSERT_NOT_NULL(base);
    char final_file[1024];
    char below_file[1024];
    char nested_dir[1024];
    snprintf(final_file, sizeof(final_file), "%s/blocker", base);
    snprintf(below_file, sizeof(below_file), "%s/blocker/child", base);
    snprintf(nested_dir, sizeof(nested_dir), "%s/real/child", base);
    th_write_file(final_file, "regular file\n");

    ASSERT_FALSE(cbm_mkdir_p(final_file, 0700));
    ASSERT_FALSE(cbm_mkdir_p(below_file, 0700));
    ASSERT_TRUE(cbm_mkdir_p(nested_dir, 0700));
    ASSERT_TRUE(cbm_mkdir_p(nested_dir, 0700)); /* existing directory */

#ifndef _WIN32
    char target[1024];
    char alias[1024];
    char through_alias[1024];
    snprintf(target, sizeof(target), "%s/target", base);
    snprintf(alias, sizeof(alias), "%s/alias", base);
    snprintf(through_alias, sizeof(through_alias), "%s/alias/child", base);
    ASSERT_TRUE(cbm_mkdir_p(target, 0700));
    ASSERT_EQ(symlink(target, alias), 0);
    ASSERT_TRUE(cbm_mkdir_p(through_alias, 0700));
#endif

    th_cleanup(base);
    PASS();
}

TEST(platform_is_dir) {
    ASSERT_TRUE(cbm_is_dir("tests"));
    ASSERT_FALSE(cbm_is_dir("tests/test_platform.c"));
    ASSERT_FALSE(cbm_is_dir("nonexistent_dir"));
    PASS();
}

TEST(platform_file_size) {
    int64_t sz = cbm_file_size("tests/test_platform.c");
    ASSERT_GT(sz, 0);
    ASSERT_EQ(cbm_file_size("nonexistent_file_xyz.txt"), -1);
    PASS();
}

TEST(platform_mmap) {
    /* mmap this test file and verify first bytes */
    size_t sz = 0;
    void *data = cbm_mmap_read("tests/test_platform.c", &sz);
    ASSERT_NOT_NULL(data);
    ASSERT_GT(sz, 0);
    /* First line should be the comment */
    ASSERT(memcmp(data, "/*", 2) == 0);
    cbm_munmap(data, sz);
    PASS();
}

TEST(platform_mmap_nonexistent) {
    size_t sz = 0;
    void *data = cbm_mmap_read("nonexistent_xyz.txt", &sz);
    ASSERT_NULL(data);
    PASS();
}

TEST(platform_safe_getenv_rejects_invalid_arguments) {
    char buf[16] = "sentinel";
    ASSERT_NULL(cbm_safe_getenv(NULL, buf, sizeof(buf), NULL));
    ASSERT_EQ(buf[0], '\0');

    memcpy(buf, "sentinel", sizeof("sentinel"));
    ASSERT_NULL(cbm_safe_getenv("", buf, sizeof(buf), NULL));
    ASSERT_EQ(buf[0], '\0');

    memcpy(buf, "sentinel", sizeof("sentinel"));
    ASSERT_NULL(cbm_safe_getenv("CBM_TEST_PLATFORM_ENV", buf, 0, NULL));
    ASSERT_EQ(buf[0], 's'); /* a zero-sized buffer cannot be cleared */
    ASSERT_NULL(cbm_safe_getenv("CBM_TEST_PLATFORM_ENV", NULL, sizeof(buf), NULL));
    PASS();
}

TEST(platform_safe_getenv_rejects_truncation) {
    const char *name = "CBM_TEST_PLATFORM_SAFE_GETENV";
    platform_env_snapshot_t snapshot;
    ASSERT_TRUE(platform_save_env(&snapshot, name));

    char buf[8] = "";
    bool ok = cbm_setenv(name, "1234567", 1) == 0;
    const char *result = ok ? cbm_safe_getenv(name, buf, sizeof(buf), "fallback") : NULL;
    bool exact_env_ok = result == buf && strcmp(buf, "1234567") == 0;

    ok = ok && cbm_setenv(name, "12345678", 1) == 0;
    memcpy(buf, "stale", sizeof("stale"));
    result = ok ? cbm_safe_getenv(name, buf, sizeof(buf), "short") : buf;
    bool long_env_rejected = result == NULL && buf[0] == '\0';

    ok = ok && cbm_unsetenv(name) == 0;
    result = ok ? cbm_safe_getenv(name, buf, sizeof(buf), "1234567") : NULL;
    bool exact_fallback_ok = result == buf && strcmp(buf, "1234567") == 0;

    memcpy(buf, "stale", sizeof("stale"));
    result = ok ? cbm_safe_getenv(name, buf, sizeof(buf), "12345678") : buf;
    bool long_fallback_rejected = result == NULL && buf[0] == '\0';

    platform_restore_env(&snapshot);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(exact_env_ok);
    ASSERT_TRUE(long_env_rejected);
    ASSERT_TRUE(exact_fallback_ok);
    ASSERT_TRUE(long_fallback_rejected);
    PASS();
}

TEST(platform_path_resolvers_reject_truncation) {
#ifdef _WIN32
    const char *config_env = "APPDATA";
    const char *local_env = "LOCALAPPDATA";
    const char *names[] = {"HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "CBM_CACHE_DIR"};
#else
    const char *config_env = "XDG_CONFIG_HOME";
    const char *names[] = {"HOME", "USERPROFILE", "XDG_CONFIG_HOME", "CBM_CACHE_DIR"};
#endif
    enum { ENV_COUNT = sizeof(names) / sizeof(names[0]), PATH_CAP = 1024 };
    platform_env_snapshot_t snapshots[ENV_COUNT];
    for (size_t i = 0; i < ENV_COUNT; i++) {
        ASSERT_TRUE(platform_save_env(&snapshots[i], names[i]));
    }

    char oversized[PATH_CAP + 1];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    char max_sized[PATH_CAP];
    memset(max_sized, 'h', sizeof(max_sized) - 1);
    max_sized[sizeof(max_sized) - 1] = '\0';

    bool setup_ok = cbm_setenv("HOME", oversized, 1) == 0 &&
                    cbm_setenv("USERPROFILE", "/fallback-must-not-be-used", 1) == 0;
    bool home_truncation_rejected = setup_ok && cbm_get_home_dir() == NULL;

    setup_ok = setup_ok && cbm_setenv("HOME", "/tmp/cbm-platform-home", 1) == 0 &&
               cbm_setenv(config_env, oversized, 1) == 0;
    bool config_truncation_rejected = setup_ok && cbm_app_config_dir() == NULL;

#ifdef _WIN32
    setup_ok = setup_ok && cbm_setenv(local_env, oversized, 1) == 0;
#endif
    bool local_truncation_rejected = setup_ok && cbm_app_local_dir() == NULL;

    setup_ok = setup_ok && cbm_setenv("CBM_CACHE_DIR", oversized, 1) == 0;
    bool cache_truncation_rejected = setup_ok && cbm_resolve_cache_dir() == NULL;

    /* The base itself fits exactly, but each derived suffix must fail closed. */
    setup_ok = setup_ok && cbm_setenv("HOME", max_sized, 1) == 0 &&
               cbm_unsetenv("USERPROFILE") == 0 && cbm_unsetenv(config_env) == 0 &&
               cbm_unsetenv("CBM_CACHE_DIR") == 0;
#ifdef _WIN32
    setup_ok = setup_ok && cbm_unsetenv(local_env) == 0;
#endif
    const char *home = setup_ok ? cbm_get_home_dir() : NULL;
    bool max_home_accepted = home != NULL && strlen(home) == PATH_CAP - 1;
    bool config_join_rejected = setup_ok && cbm_app_config_dir() == NULL;
    bool local_join_rejected = setup_ok && cbm_app_local_dir() == NULL;
    bool cache_join_rejected = setup_ok && cbm_resolve_cache_dir() == NULL;

    for (size_t i = ENV_COUNT; i > 0; i--) {
        platform_restore_env(&snapshots[i - 1]);
    }
    ASSERT_TRUE(setup_ok);
    ASSERT_TRUE(home_truncation_rejected);
    ASSERT_TRUE(config_truncation_rejected);
    ASSERT_TRUE(local_truncation_rejected);
    ASSERT_TRUE(cache_truncation_rejected);
    ASSERT_TRUE(max_home_accepted);
    ASSERT_TRUE(config_join_rejected);
    ASSERT_TRUE(local_join_rejected);
    ASSERT_TRUE(cache_join_rejected);
    PASS();
}

#ifndef _WIN32
typedef struct {
    const char *home;
    const char *config;
    const char *local;
    const char *cache;
    bool content_ok;
} platform_tls_result_t;

static atomic_int platform_tls_ready;
static atomic_bool platform_tls_release;

static void *platform_tls_worker(void *opaque) {
    platform_tls_result_t *result = opaque;
    result->home = cbm_get_home_dir();
    result->config = cbm_app_config_dir();
    result->local = cbm_app_local_dir();
    result->cache = cbm_resolve_cache_dir();
    result->content_ok =
        result->home && strcmp(result->home, "/tmp/cbm-platform-tls-home") == 0 && result->config &&
        strcmp(result->config, "/tmp/cbm-platform-tls-config") == 0 && result->local &&
        strcmp(result->local, "/tmp/cbm-platform-tls-config") == 0 && result->cache &&
        strcmp(result->cache, "/tmp/cbm-platform-tls-cache") == 0;
    atomic_fetch_add_explicit(&platform_tls_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&platform_tls_release, memory_order_acquire)) {
        cbm_usleep(1000);
    }
    return NULL;
}

TEST(platform_path_resolvers_use_thread_local_buffers) {
    const char *names[] = {"HOME", "USERPROFILE", "XDG_CONFIG_HOME", "CBM_CACHE_DIR"};
    enum { ENV_COUNT = sizeof(names) / sizeof(names[0]) };
    platform_env_snapshot_t snapshots[ENV_COUNT];
    for (size_t i = 0; i < ENV_COUNT; i++) {
        ASSERT_TRUE(platform_save_env(&snapshots[i], names[i]));
    }

    bool setup_ok = cbm_setenv("HOME", "/tmp/cbm-platform-tls-home", 1) == 0 &&
                    cbm_unsetenv("USERPROFILE") == 0 &&
                    cbm_setenv("XDG_CONFIG_HOME", "/tmp/cbm-platform-tls-config", 1) == 0 &&
                    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-platform-tls-cache", 1) == 0;
    atomic_store_explicit(&platform_tls_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&platform_tls_release, false, memory_order_relaxed);

    pthread_t threads[2];
    platform_tls_result_t results[2] = {{0}};
    int create_rc[2] = {-1, -1};
    if (setup_ok) {
        create_rc[0] = pthread_create(&threads[0], NULL, platform_tls_worker, &results[0]);
        create_rc[1] = pthread_create(&threads[1], NULL, platform_tls_worker, &results[1]);
    }

    bool ready = false;
    if (create_rc[0] == 0 && create_rc[1] == 0) {
        for (int i = 0; i < 10000; i++) {
            if (atomic_load_explicit(&platform_tls_ready, memory_order_acquire) == 2) {
                ready = true;
                break;
            }
            cbm_usleep(1000);
        }
    }
    bool distinct = ready && results[0].home != results[1].home &&
                    results[0].config != results[1].config &&
                    results[0].local != results[1].local && results[0].cache != results[1].cache;
    atomic_store_explicit(&platform_tls_release, true, memory_order_release);
    if (create_rc[0] == 0) {
        (void)pthread_join(threads[0], NULL);
    }
    if (create_rc[1] == 0) {
        (void)pthread_join(threads[1], NULL);
    }

    for (size_t i = ENV_COUNT; i > 0; i--) {
        platform_restore_env(&snapshots[i - 1]);
    }
    ASSERT_TRUE(setup_ok);
    ASSERT_EQ(create_rc[0], 0);
    ASSERT_EQ(create_rc[1], 0);
    ASSERT_TRUE(ready);
    ASSERT_TRUE(results[0].content_ok);
    ASSERT_TRUE(results[1].content_ok);
    ASSERT_TRUE(distinct);
    PASS();
}
#endif

/*
 * CBM_WORKERS env override for cbm_default_worker_count.
 *
 * Containers running cbm on a host with more CPUs than the cgroup's
 * effective quota currently see ~host_cpu workers spawned because
 * sysconf(_SC_NPROCESSORS_ONLN) is not cgroup-aware (see GitHub
 * issue for the cgroup-detection ask). CBM_WORKERS is the smaller,
 * explicit-override path that ships independently.
 */
TEST(platform_default_workers_env_override) {
    cbm_setenv("CBM_WORKERS", "4", 1);
    int n = cbm_default_worker_count(true);
    ASSERT_EQ(n, 4);
    /* initial=false should also honor the explicit override. */
    int m = cbm_default_worker_count(false);
    ASSERT_EQ(m, 4);
    cbm_unsetenv("CBM_WORKERS");
    PASS();
}

TEST(platform_default_workers_env_invalid) {
    /* Out-of-range values (< 1 or > 256) and non-numeric strings
     * fall back to the sysconf-derived default. */
    int baseline = cbm_default_worker_count(true);
    ASSERT_GT(baseline, 0);

    cbm_setenv("CBM_WORKERS", "0", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "-1", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "9999", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_setenv("CBM_WORKERS", "not-a-number", 1);
    ASSERT_EQ(cbm_default_worker_count(true), baseline);

    cbm_unsetenv("CBM_WORKERS");
    PASS();
}

TEST(platform_default_workers_env_unset) {
    /* When CBM_WORKERS is unset the result matches today's behaviour
     * (info.total_cores for initial=true, perf_cores-1 for false). */
    cbm_unsetenv("CBM_WORKERS");
    cbm_system_info_t info = cbm_system_info();
    ASSERT_EQ(cbm_default_worker_count(true), info.total_cores);
    PASS();
}

/* ── cgroup-aware detection (Linux only) ─────────────────────────── */

#ifdef __linux__

/* Create a unique tmp directory the caller will own; returns 0 on success. */
static int cgroup_test_setup(char *root, size_t root_sz) {
    strncpy(root, "/tmp/cbm_cgroup_test_XXXXXX", root_sz);
    return mkdtemp(root) != NULL ? 0 : -1;
}

/* Write `content` to "<root>/<relpath>". Creates parent subdir if needed.
 * Returns 0 on success, -1 on any failure. */
static int cgroup_test_write(const char *root, const char *relpath, const char *content) {
    char path[1024];
    const char *slash = strchr(relpath, '/');
    if (slash != NULL) {
        char subdir[1024];
        size_t n = (size_t)(slash - relpath);
        if (n >= sizeof(subdir)) {
            return -1;
        }
        memcpy(subdir, relpath, n);
        subdir[n] = '\0';
        snprintf(path, sizeof(path), "%s/%s", root, subdir);
        (void)mkdir(path, S_IRWXU);
    }
    snprintf(path, sizeof(path), "%s/%s", root, relpath);
    FILE *fp = fopen(path, "we");
    if (fp == NULL) {
        return -1;
    }
    size_t n = strlen(content);
    int rc = (fwrite(content, 1, n, fp) == n) ? 0 : -1;
    fclose(fp);
    return rc;
}

/* Recursively remove a tmp dir created by cgroup_test_setup. Best-effort.
 * Uses opendir/unlink/rmdir rather than system("rm -rf ...") to avoid
 * spawning a shell from the test binary. */
static void cgroup_test_teardown(const char *root) {
    DIR *d = opendir(root);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", root, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                cgroup_test_teardown(child); /* recurse into subdir */
            } else {
                (void)unlink(child);
            }
        }
        closedir(d);
    }
    (void)rmdir(root);
}

TEST(cgroup_v2_cpu_quota) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 200ms quota in a 100ms period → 2 effective CPUs. */
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "200000 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_cpu_quota_rounds_up) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 150ms quota / 100ms period = 1.5 → ceil = 2. */
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "150000 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_cpu_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu.max", "max 100000\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_cpu_quota) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_quota_us", "200000"), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_period_us", "100000"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), 2);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_cpu_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* quota=-1 is the cgroup-v1 sentinel for "no quota". */
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_quota_us", "-1"), 0);
    ASSERT_EQ(cgroup_test_write(root, "cpu/cpu.cfs_period_us", "100000"), 0);
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_no_cpu_files) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* Empty tmp dir: no v2 file, no v1 file → fall through to sysconf. */
    ASSERT_EQ(cbm_detect_cgroup_cpus(root), -1);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_mem) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 2 GiB. */
    ASSERT_EQ(cgroup_test_write(root, "memory.max", "2147483648\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)2147483648UL);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v2_mem_unlimited) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cgroup_test_write(root, "memory.max", "max\n"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_mem) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* 1 GiB. */
    ASSERT_EQ(cgroup_test_write(root, "memory/memory.limit_in_bytes", "1073741824"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)1073741824UL);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_v1_mem_unlimited_sentinel) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    /* cgroup v1 reports a huge near-ULLONG_MAX value when unlimited
     * (PAGE_COUNTER_MAX). Our parser treats anything >= ULLONG_MAX/2
     * as effectively unlimited. */
    ASSERT_EQ(cgroup_test_write(root, "memory/memory.limit_in_bytes", "9223372036854775807"), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

TEST(cgroup_no_mem_files) {
    char root[64];
    ASSERT_EQ(cgroup_test_setup(root, sizeof(root)), 0);
    ASSERT_EQ(cbm_detect_cgroup_mem(root), (size_t)0);
    cgroup_test_teardown(root);
    PASS();
}

#endif /* __linux__ */

SUITE(platform) {
    RUN_TEST(platform_now_ns);
    RUN_TEST(platform_qpc_scaling_avoids_intermediate_overflow);
    RUN_TEST(platform_posix_resolvers_preserve_backslash);
#ifdef _WIN32
    RUN_TEST(platform_safe_getenv_decodes_wide_unicode);
#endif
    RUN_TEST(platform_now_ms);
    RUN_TEST(platform_nprocs);
    RUN_TEST(platform_file_exists);
    RUN_TEST(platform_mkdir_p_rejects_file_components);
    RUN_TEST(platform_is_dir);
    RUN_TEST(platform_file_size);
    RUN_TEST(platform_mmap);
    RUN_TEST(platform_mmap_nonexistent);
    RUN_TEST(platform_safe_getenv_rejects_invalid_arguments);
    RUN_TEST(platform_safe_getenv_rejects_truncation);
    RUN_TEST(platform_path_resolvers_reject_truncation);
#ifndef _WIN32
    RUN_TEST(platform_path_resolvers_use_thread_local_buffers);
#endif
    RUN_TEST(platform_default_workers_env_override);
    RUN_TEST(platform_default_workers_env_invalid);
    RUN_TEST(platform_default_workers_env_unset);
#ifdef __linux__
    RUN_TEST(cgroup_v2_cpu_quota);
    RUN_TEST(cgroup_v2_cpu_quota_rounds_up);
    RUN_TEST(cgroup_v2_cpu_unlimited);
    RUN_TEST(cgroup_v1_cpu_quota);
    RUN_TEST(cgroup_v1_cpu_unlimited);
    RUN_TEST(cgroup_no_cpu_files);
    RUN_TEST(cgroup_v2_mem);
    RUN_TEST(cgroup_v2_mem_unlimited);
    RUN_TEST(cgroup_v1_mem);
    RUN_TEST(cgroup_v1_mem_unlimited_sentinel);
    RUN_TEST(cgroup_no_mem_files);
#endif
}
