/*
 * test_subprocess.c — foundation/subprocess: spawn + supervise + classify.
 *
 * Two layers:
 *   1. cbm_proc_classify() — pure, exercised on EVERY platform (so the Windows
 *      NTSTATUS crash-code mapping is guarded on Linux/macOS CI too, not just an
 *      untested Windows branch).
 *   2. cbm_subprocess_run() — real spawn/reap, exercised on POSIX via /bin/sh
 *      (SKIP_PLATFORM on Windows, which lacks it).
 */
#include "test_framework.h"
#include "../src/foundation/subprocess.h"
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/compat_thread.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

static char *subprocess_test_binary(void) {
    wchar_t wide[32768];
    DWORD length = GetModuleFileNameW(NULL, wide, (DWORD)(sizeof(wide) / sizeof(wide[0])));
    if (length == 0 || length >= (DWORD)(sizeof(wide) / sizeof(wide[0]))) {
        return NULL;
    }
    return cbm_wide_to_utf8(wide);
}
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

typedef struct {
    char lines[4][64];
    int count;
    cbm_proc_control_t *control;
    uint64_t observed_pid;
} subprocess_log_capture_t;

static void capture_subprocess_line(const char *line, void *userdata) {
    subprocess_log_capture_t *capture = (subprocess_log_capture_t *)userdata;
    if (!capture || !line || capture->count >= 4) {
        return;
    }
    snprintf(capture->lines[capture->count], sizeof(capture->lines[capture->count]), "%s", line);
    capture->count++;
    if (capture->control)
        capture->observed_pid = cbm_proc_control_pid(capture->control);
}

/* ── Layer 1: pure classifier (all platforms) ─────────────────────────────── */

TEST(subprocess_classify_clean) {
    ASSERT_EQ(cbm_proc_classify(true, 0, 0, false), CBM_PROC_CLEAN);
    PASS();
}

TEST(subprocess_classify_exit_nonzero) {
    ASSERT_EQ(cbm_proc_classify(true, 3, 0, false), CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(cbm_proc_classify(true, 127, 0, false), CBM_PROC_EXIT_NONZERO);
    PASS();
}

/* Windows exception exit codes classify as CRASH — the key reason to unit-test
 * the classifier cross-platform. 0xC0000005 = access violation (SIGSEGV analog),
 * 0xC00000FD = stack overflow (the #668 reporter's 0xC00000FD). */
TEST(subprocess_classify_windows_crash_codes) {
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC0000005u, 0, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC00000FDu, 0, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(true, (int)0xC000001Du, 0, false), CBM_PROC_CRASH);
    PASS();
}

TEST(subprocess_classify_posix_fault_signal_is_crash) {
#ifndef _WIN32
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGSEGV, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGABRT, false), CBM_PROC_CRASH);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGBUS, false), CBM_PROC_CRASH);
#endif
    PASS();
}

TEST(subprocess_classify_non_fault_signal_is_killed) {
#ifndef _WIN32
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGTERM, false), CBM_PROC_KILLED);
    ASSERT_EQ(cbm_proc_classify(false, -1, SIGKILL, false), CBM_PROC_KILLED);
#endif
    PASS();
}

/* timed_out dominates every other signal — a killed-for-hang child is HANG,
 * not KILLED, even though we deliver SIGKILL/TerminateProcess to end it. */
TEST(subprocess_classify_timeout_dominates) {
    ASSERT_EQ(cbm_proc_classify(false, -1, 9 /*SIGKILL*/, true), CBM_PROC_HANG);
    ASSERT_EQ(cbm_proc_classify(true, 0, 0, true), CBM_PROC_HANG);
    PASS();
}

TEST(subprocess_outcome_str) {
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_CLEAN), "clean");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_CRASH), "crash");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_HANG), "hang");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_OUTPUT_LIMIT), "output_limit");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_EXIT_NONZERO), "exit_nonzero");
    ASSERT_STR_EQ(cbm_proc_outcome_str(CBM_PROC_KILLED), "killed");
    PASS();
}

/* ── Layer 2: real spawn/reap (POSIX) ─────────────────────────────────────── */

#ifndef _WIN32
static cbm_proc_result_t run_sh(const char *script, int quiet_timeout_ms) {
    const char *argv[] = {"/bin/sh", "-c", script, NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/bin/sh";
    opts.argv = argv;
    opts.log_file = NULL;
    opts.quiet_timeout_ms = quiet_timeout_ms;
    cbm_proc_result_t r;
    cbm_subprocess_run(&opts, &r);
    return r;
}
#endif

TEST(subprocess_run_clean) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("exit 0", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(r.exit_code, 0);
    PASS();
#endif
}

TEST(subprocess_run_exit_nonzero) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("exit 7", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(r.exit_code, 7);
    PASS();
#endif
}

/* A child that dies of SIGSEGV must classify as CRASH — NOT exit_nonzero and NOT
 * killed. This is the whole point of the primitive: distinguish a crash from a
 * clean failure so the supervisor can quarantine the culprit. */
TEST(subprocess_run_crash_is_crash) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX signal semantics");
#else
    cbm_proc_result_t r = run_sh("kill -SEGV $$", 0);
    ASSERT_EQ(r.outcome, CBM_PROC_CRASH);
    ASSERT_EQ(r.term_signal, SIGSEGV);
    PASS();
#endif
}

/* A child that makes no progress within the quiet-timeout is killed and reported
 * as HANG — the sibling failure mode of a crash (external-scanner infinite loop). */
TEST(subprocess_run_hang_is_hang) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh spawn");
#else
    cbm_proc_result_t r = run_sh("sleep 30", 300 /* ms quiet-timeout */);
    ASSERT_EQ(r.outcome, CBM_PROC_HANG);
    PASS();
#endif
}

/* A spawn of a non-existent binary fails cleanly (no child), not a crash. */
TEST(subprocess_run_spawn_failure) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX exec semantics");
#else
    /* execv of a bogus path: fork succeeds, child _exit(127). We classify the
     * reaped 127 as exit_nonzero — spawn_failed is reserved for fork() failing. */
    const char *argv[] = {"/nonexistent/cbm-bogus-binary", NULL};
    cbm_proc_opts_t opts = {0};
    opts.bin = "/nonexistent/cbm-bogus-binary";
    opts.argv = argv;
    cbm_proc_result_t r;
    int rc = cbm_subprocess_run(&opts, &r);
    ASSERT_EQ(rc, 0); /* fork itself succeeded */
    ASSERT_EQ(r.outcome, CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(r.exit_code, 127);
    PASS();
#endif
}

TEST(subprocess_run_null_bin_rejected) {
    cbm_proc_control_t control = {0};
    cbm_proc_control_init(&control);
    cbm_proc_opts_t opts = {0};
    opts.bin = NULL;
    opts.control = &control;
    cbm_proc_result_t r;
    int rc = cbm_subprocess_run(&opts, &r);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(r.outcome, CBM_PROC_SPAWN_FAILED);
    ASSERT_EQ(cbm_proc_control_pid(&control), 0);
    PASS();
}

TEST(subprocess_child_stdin_and_descriptors_are_isolated) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX descriptor semantics");
#else
    int inherited = open("/dev/null", O_RDONLY);
    ASSERT_GTE(inherited, 0);
    char script[256];
    snprintf(script, sizeof(script),
             "if [ -e /dev/fd/%d ]; then exit 41; fi; "
             "if IFS= read -r line; then exit 42; fi; exit 0",
             inherited);
    cbm_proc_result_t result = run_sh(script, 1000);
    ASSERT_EQ(close(inherited), 0);
    ASSERT_EQ(result.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(result.exit_code, 0);
    PASS();
#endif
}

TEST(subprocess_capture_binary_exact) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh binary capture");
#else
    const char *argv[] = {"/bin/sh", "-c", "printf 'A\\000B'", NULL};
    cbm_proc_opts_t opts = {
        .bin = "/bin/sh", .argv = argv, .quiet_timeout_ms = 1000, .discard_stderr = true};
    char *data = NULL;
    size_t len = 0;
    char sha256[65];
    cbm_proc_result_t process;
    ASSERT_EQ(cbm_subprocess_capture(&opts, 1024, &data, &len, sha256, &process), 0);
    ASSERT_EQ(process.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(len, 3);
    ASSERT_EQ(data[0], 'A');
    ASSERT_EQ(data[1], '\0');
    ASSERT_EQ(data[2], 'B');
    ASSERT_EQ(strlen(sha256), 64);
    free(data);
    PASS();
#endif
}

TEST(subprocess_capture_rejects_oversized_output) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX /bin/sh capture limit");
#else
    const char *argv[] = {"/bin/sh", "-c", "printf '0123456789'", NULL};
    cbm_proc_opts_t opts = {.bin = "/bin/sh", .argv = argv, .discard_stderr = true};
    char *data = NULL;
    size_t len = 0;
    char sha256[65];
    cbm_proc_result_t process;
    ASSERT_EQ(cbm_subprocess_capture(&opts, 5, &data, &len, sha256, &process), -1);
    ASSERT_EQ(process.outcome, CBM_PROC_OUTPUT_LIMIT);
    ASSERT(data == NULL);
    ASSERT_EQ(len, 0);
    ASSERT_EQ(sha256[0], '\0');
    PASS();
#endif
}

TEST(subprocess_log_path_rejects_symlink_and_closed_output_fd) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX descriptor and symlink semantics");
#else
    char victim[512];
    char log_path[512];
    snprintf(victim, sizeof(victim), "%s/cbm-subprocess-victim-%d", cbm_tmpdir(), (int)getpid());
    snprintf(log_path, sizeof(log_path), "%s/cbm-subprocess-log-%d", cbm_tmpdir(), (int)getpid());
    (void)cbm_unlink(victim);
    (void)cbm_unlink(log_path);
    FILE *file = fopen(victim, "w");
    ASSERT_NOT_NULL(file);
    ASSERT_TRUE(fputs("do-not-truncate", file) >= 0);
    ASSERT_EQ(fclose(file), 0);
    ASSERT_EQ(symlink(victim, log_path), 0);

    const char *argv[] = {"/bin/sh", "-c", "printf attacker", NULL};
    cbm_proc_opts_t opts = {.bin = "/bin/sh", .argv = argv, .log_file = log_path};
    cbm_proc_result_t result;
    ASSERT_EQ(cbm_subprocess_run(&opts, &result), -1);
    ASSERT_EQ(result.outcome, CBM_PROC_SPAWN_FAILED);
    char check[32] = {0};
    file = fopen(victim, "r");
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(fgets(check, sizeof(check), file));
    ASSERT_EQ(fclose(file), 0);
    ASSERT_STR_EQ(check, "do-not-truncate");
    ASSERT_EQ(cbm_unlink(log_path), 0);
    ASSERT_EQ(cbm_unlink(victim), 0);

    int dead_fd = open("/dev/null", O_WRONLY);
    ASSERT_GTE(dead_fd, 0);
    ASSERT_EQ(close(dead_fd), 0);
    opts = (cbm_proc_opts_t){
        .bin = "/bin/sh", .argv = argv, .use_output_fd = true, .output_fd = dead_fd};
    ASSERT_EQ(cbm_subprocess_run(&opts, &result), 0);
    ASSERT_EQ(result.outcome, CBM_PROC_EXIT_NONZERO);
    ASSERT_EQ(result.exit_code, 127);
    PASS();
#endif
}

TEST(subprocess_output_fd_is_tailed_without_reopening_path) {
    char output_path[512];
    snprintf(output_path, sizeof(output_path), "%s/cbm-subprocess-tail-XXXXXX", cbm_tmpdir());
    int output_fd = cbm_mkstemp(output_path);
    ASSERT_GTE(output_fd, 0);
#ifndef _WIN32
    /* Unlink before spawn: the callback can only succeed by reading the exact
     * still-open descriptor, never by reopening its former pathname. */
    ASSERT_EQ(cbm_unlink(output_path), 0);
#endif

#ifdef _WIN32
    char *test_binary = subprocess_test_binary();
    if (!test_binary) {
        _close(output_fd);
        cbm_unlink(output_path);
        SKIP_PLATFORM("test executable path unavailable");
    }
    const char *argv[] = {test_binary, "__cbm_subprocess_emit", NULL};
#else
    const char *argv[] = {"/bin/sh", "-c", "printf 'alpha\\nbeta\\n'", NULL};
#endif
    cbm_proc_control_t control = {0};
    cbm_proc_control_init(&control);
    subprocess_log_capture_t capture = {.control = &control};
    cbm_proc_opts_t opts = {
#ifdef _WIN32
        .bin = test_binary,
#else
        .bin = "/bin/sh",
#endif
        .argv = argv,
        .on_log_line = capture_subprocess_line,
        .log_ud = &capture,
        .quiet_timeout_ms = 1000,
        .use_output_fd = true,
        .output_fd = output_fd,
        .control = &control,
    };
    cbm_proc_result_t result;
    ASSERT_EQ(cbm_subprocess_run(&opts, &result), 0);
    ASSERT_EQ(result.outcome, CBM_PROC_CLEAN);
    ASSERT_EQ(capture.count, 2);
    ASSERT_STR_EQ(capture.lines[0], "alpha");
    ASSERT_STR_EQ(capture.lines[1], "beta");
    ASSERT_TRUE(capture.observed_pid > 0);
    ASSERT_EQ(cbm_proc_control_pid(&control), 0);
#ifdef _WIN32
    ASSERT_EQ(_close(output_fd), 0);
    ASSERT_EQ(cbm_unlink(output_path), 0);
    free(test_binary);
#else
    ASSERT_EQ(close(output_fd), 0);
#endif
    PASS();
}

typedef struct {
    cbm_proc_opts_t opts;
    cbm_proc_result_t result;
    int rc;
} subprocess_control_run_t;

static void *run_controlled_subprocess(void *arg) {
    subprocess_control_run_t *run = (subprocess_control_run_t *)arg;
    run->rc = cbm_subprocess_run(&run->opts, &run->result);
    return NULL;
}

TEST(subprocess_control_kill_is_consumed_by_owner) {
#ifdef _WIN32
    char *test_binary = subprocess_test_binary();
    if (!test_binary)
        SKIP_PLATFORM("test executable path unavailable");
    const char *bin = test_binary;
    const char *argv[] = {bin, "__cbm_subprocess_spin", NULL};
#else
    const char *bin = "/bin/sh";
    const char *argv[] = {bin, "-c", "while :; do :; done", NULL};
#endif
    cbm_proc_control_t control = {0};
    cbm_proc_control_init(&control);
    subprocess_control_run_t run;
    memset(&run, 0, sizeof(run));
    run.opts.bin = bin;
    run.opts.argv = argv;
    run.opts.quiet_timeout_ms = 5000;
    run.opts.control = &control;

    cbm_thread_t thread;
    ASSERT_EQ(cbm_thread_create(&thread, 0, run_controlled_subprocess, &run), 0);
    uint64_t pid = 0;
    for (int i = 0; i < 200 && pid == 0; i++) {
        pid = cbm_proc_control_pid(&control);
        if (pid == 0)
            cbm_usleep(10000);
    }

    bool wrong_pid_rejected = pid > 0 && !cbm_proc_control_request_kill(&control, pid + 1U);
    bool accepted = pid > 0 && cbm_proc_control_request_kill(&control, pid);
    bool duplicate_rejected = !cbm_proc_control_request_kill(&control, pid);
    ASSERT_EQ(cbm_thread_join(&thread), 0);

    ASSERT_TRUE(pid > 0);
    ASSERT_TRUE(wrong_pid_rejected);
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(duplicate_rejected);
    ASSERT_EQ(run.rc, 0);
    ASSERT_EQ(run.result.outcome, CBM_PROC_KILLED);
    ASSERT_EQ(cbm_proc_control_pid(&control), 0);
    ASSERT_FALSE(cbm_proc_control_request_kill(&control, pid));
#ifdef _WIN32
    free(test_binary);
#endif
    PASS();
}

/* ── Layer 3: Windows command-line quoting (pure; every platform) ─────────────
 *
 * The Windows index-worker "crash" was a quoting bug: the Windows spawn wrapped each
 * argv element in bare quotes without escaping, so a JSON argument like
 * {"repo_path":"C:/r"} lost its inner quotes when the child re-parsed the command
 * line — the worker then failed at JSON-arg parse and exited non-zero, which the
 * supervisor misreported as a per-file crash. We guard cbm_build_win_cmdline by
 * ROUND-TRIP: a reference implementation of the Windows CommandLineToArgvW rules
 * (the inverse of the builder) must re-parse the emitted line back into the exact
 * original argv. Testing the invariant — not a hand-computed escaped string — keeps
 * the guard honest and readable, and runs on Linux/macOS CI (the builder is pure). */

/* Reference re-parser: the subset of CommandLineToArgvW our builder emits (every
 * arg quote-wrapped; \" for embedded quotes; backslashes doubled before a quote). */
static int parse_win_cmdline(const char *cmd, char out[][256], int max_args) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || argc >= max_args) {
            break;
        }
        char *o = out[argc];
        size_t oi = 0;
        bool in_quotes = false;
        /* Guard every write: each out[] row is 256 bytes; test args stay well under
         * that, but cap defensively so a future longer arg fails a length assertion
         * rather than smashing the stack. */
#define PUTO(ch)            \
    do {                    \
        if (oi < 255) {     \
            o[oi++] = (ch); \
        }                   \
    } while (0)
        for (;;) {
            size_t nbs = 0;
            while (*p == '\\') {
                nbs++;
                p++;
            }
            if (*p == '"') {
                for (size_t k = 0; k < nbs / 2; k++) {
                    PUTO('\\');
                }
                if (nbs % 2) {
                    PUTO('"'); /* odd run → the quote is an escaped literal */
                } else {
                    in_quotes = !in_quotes; /* even run → the quote is a delimiter */
                }
                p++;
            } else {
                for (size_t k = 0; k < nbs; k++) {
                    PUTO('\\');
                }
                if (*p == '\0' || (!in_quotes && (*p == ' ' || *p == '\t'))) {
                    break;
                }
                PUTO(*p);
                p++;
            }
        }
#undef PUTO
        o[oi] = '\0';
        argc++;
    }
    return argc;
}

static bool cmdline_roundtrips(const char *const *argv) {
    char cmd[4096];
    if (!cbm_build_win_cmdline(cmd, sizeof(cmd), argv)) {
        return false;
    }
    char parsed[16][256];
    int pc = parse_win_cmdline(cmd, parsed, 16);
    int oc = 0;
    while (argv[oc]) {
        oc++;
    }
    if (pc != oc) {
        return false;
    }
    for (int i = 0; i < oc; i++) {
        if (strcmp(argv[i], parsed[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* The exact index-worker argv: the command line with a JSON arg full of quotes.
 * Round-trips, AND the emitted line must contain an ESCAPED quote (\") — the bare
 * `"%s"` wrap that caused the bug never would. */
TEST(win_cmdline_index_worker_json) {
    const char *const argv[] = {"C:/bin/cbm.exe",           "cli",
                                "--index-worker",           "index_repository",
                                "{\"repo_path\":\"C:/r\"}", "--response-out",
                                "C:/c/w.response",          NULL};
    ASSERT(cmdline_roundtrips(argv));
    char cmd[4096];
    ASSERT(cbm_build_win_cmdline(cmd, sizeof(cmd), argv));
    ASSERT(strstr(cmd, "\\\"repo_path\\\"") != NULL); /* inner quotes are escaped */
    PASS();
}

/* A battery of adversarial argv (spaces, tabs, embedded quotes, backslash runs,
 * trailing backslashes, backslash-before-quote, real Windows paths) must all
 * round-trip byte-for-byte through the builder + reference parser. */
TEST(win_cmdline_roundtrip_battery) {
    const char *const a1[] = {"a", "b", NULL};
    const char *const a2[] = {"has space", "tab\there", NULL};
    const char *const a3[] = {"trailing\\", "a\\b\\c", NULL};
    const char *const a4[] = {"a\\\"b", "\"", "\\\\\"", NULL};
    const char *const a5[] = {"C:\\Users\\me\\my repo", "{\"name\":\"a b\",\"x\":\"y\\\\z\"}",
                              NULL};
    const char *const a6[] = {"", "plain", NULL};
    ASSERT(cmdline_roundtrips(a1));
    ASSERT(cmdline_roundtrips(a2));
    ASSERT(cmdline_roundtrips(a3));
    ASSERT(cmdline_roundtrips(a4));
    ASSERT(cmdline_roundtrips(a5));
    ASSERT(cmdline_roundtrips(a6));
    PASS();
}

/* Overflow is reported (false), never a silent truncation that would spawn a
 * corrupted command line. */
TEST(win_cmdline_overflow_rejected) {
    const char *const argv[] = {"aaaaaaaaaa", "bbbbbbbbbb", NULL};
    char tiny[8];
    ASSERT_FALSE(cbm_build_win_cmdline(tiny, sizeof(tiny), argv));
    PASS();
}

/* Reproduce-first guard for the overflow CONTRACT (subprocess.h): on overflow buf
 * must be left a valid (empty) string. RED on the pre-fix code — the overflow path
 * returned without terminating buf, so buf[0] held the first quoted byte ('"') —
 * and GREEN once the overflow path sets buf[0] = '\0'. */
TEST(win_cmdline_overflow_leaves_empty_string) {
    char buf[8];
    const char *const argv[] = {"averylongprogramname", "x", NULL};
    ASSERT_FALSE(cbm_build_win_cmdline(buf, sizeof(buf), argv)); /* overflows cap */
    ASSERT_EQ(buf[0], '\0'); /* pre-fix: buf[0] == '"' (a partial byte), not NUL */
    PASS();
}

SUITE(subprocess) {
    RUN_TEST(subprocess_classify_clean);
    RUN_TEST(subprocess_classify_exit_nonzero);
    RUN_TEST(subprocess_classify_windows_crash_codes);
    RUN_TEST(subprocess_classify_posix_fault_signal_is_crash);
    RUN_TEST(subprocess_classify_non_fault_signal_is_killed);
    RUN_TEST(subprocess_classify_timeout_dominates);
    RUN_TEST(subprocess_outcome_str);
    RUN_TEST(subprocess_run_clean);
    RUN_TEST(subprocess_run_exit_nonzero);
    RUN_TEST(subprocess_run_crash_is_crash);
    RUN_TEST(subprocess_run_hang_is_hang);
    RUN_TEST(subprocess_run_spawn_failure);
    RUN_TEST(subprocess_run_null_bin_rejected);
    RUN_TEST(subprocess_child_stdin_and_descriptors_are_isolated);
    RUN_TEST(subprocess_capture_binary_exact);
    RUN_TEST(subprocess_capture_rejects_oversized_output);
    RUN_TEST(subprocess_log_path_rejects_symlink_and_closed_output_fd);
    RUN_TEST(subprocess_output_fd_is_tailed_without_reopening_path);
    RUN_TEST(subprocess_control_kill_is_consumed_by_owner);
    RUN_TEST(win_cmdline_index_worker_json);
    RUN_TEST(win_cmdline_roundtrip_battery);
    RUN_TEST(win_cmdline_overflow_rejected);
    RUN_TEST(win_cmdline_overflow_leaves_empty_string);
}
