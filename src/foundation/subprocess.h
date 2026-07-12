/*
 * subprocess.h — spawn a child process, supervise it, and classify how it ended.
 *
 * Generalized from the crash-isolating index spawn in src/ui/http_server.c so the
 * crash/hang supervisor (Track C) can reuse one primitive across platforms.
 *
 * Beyond a plain spawn+wait it adds the two things a supervisor needs and the
 * ad-hoc harness lacked:
 *   1. Exit CLASSIFICATION — {clean, exit-nonzero, crash, hang, killed} — from
 *      POSIX WIFSIGNALED/WTERMSIG and the Windows NTSTATUS exception exit codes
 *      (0xC0000005 access-violation, 0xC00000FD stack-overflow, …).
 *   2. A quiet-timeout — kill + report HANG when the child makes no progress
 *      (emits no new log line) for a configurable window. This catches external
 *      tree-sitter scanners that infinite-loop (a hang, not a crash).
 *
 * The reap loop is EINTR-safe. Line tailing keeps a partial final line buffered
 * (an incomplete, un-newline-terminated line is not yet "progress" and is not
 * mis-read as a completed marker).
 */
#ifndef CBM_SUBPROCESS_H
#define CBM_SUBPROCESS_H

#include <stdbool.h>
#include <stddef.h> /* size_t (cbm_build_win_cmdline) */
#include <stdint.h>
#include <stdatomic.h>

/* How a supervised child ended. */
typedef enum {
    CBM_PROC_CLEAN = 0,    /* exited with code 0 */
    CBM_PROC_EXIT_NONZERO, /* exited with a nonzero code (a graceful failure) */
    CBM_PROC_CRASH,        /* died from a fault: POSIX SIGSEGV/BUS/ILL/FPE/ABRT/SYS,
                            * or a Windows NTSTATUS exception exit code (>= 0xC0000000) */
    CBM_PROC_HANG,         /* made no progress within the quiet-timeout; we killed it */
    CBM_PROC_OUTPUT_LIMIT, /* exceeded the configured output byte ceiling; we killed it */
    CBM_PROC_KILLED,       /* terminated by a non-fault signal we did not initiate */
    CBM_PROC_SPAWN_FAILED  /* fork/exec/CreateProcess failed — no child ever ran */
} cbm_proc_outcome_t;

typedef struct {
    cbm_proc_outcome_t outcome;
    int exit_code;   /* WEXITSTATUS / GetExitCodeProcess; -1 when terminated by a POSIX signal */
    int term_signal; /* WTERMSIG on POSIX; 0 otherwise */
} cbm_proc_result_t;

/* Called for each newly-completed (newline-terminated) log line while the child
 * runs. A completed line also resets the quiet-timeout (it is progress). */
typedef void (*cbm_proc_log_cb)(const char *line, void *ud);

/* Stable control plane for one supervised run. The caller owns this object and
 * must keep it alive until cbm_subprocess_run() returns. A kill request never
 * signals a numeric PID directly: the supervisor that still owns the POSIX
 * child/wait relationship or Windows process HANDLE performs the termination.
 * This prevents a stale PID from targeting an unrelated reused process. */
typedef struct {
    atomic_int state;
    _Atomic uint64_t pid;
} cbm_proc_control_t;

/* Initialize before first use (and only while no run is active). A zeroed
 * static object is also a valid initial state. */
void cbm_proc_control_init(cbm_proc_control_t *control);

/* Return the PID of the currently supervised child, or 0 when no run is live. */
uint64_t cbm_proc_control_pid(const cbm_proc_control_t *control);

/* Atomically request that the owning supervisor kill its current child. The
 * expected PID prevents a stale UI observation from selecting a different run.
 * Returns false when the control is not live or the PID no longer matches. */
bool cbm_proc_control_request_kill(cbm_proc_control_t *control, uint64_t expected_pid);

typedef struct {
    const char *bin;             /* executable path; also argv[0] when argv is NULL */
    const char *const *argv;     /* NULL-terminated argv; NULL => { bin, NULL } */
    const char *log_file;        /* child stdout+stderr are redirected here and tailed;
                                  * NULL => discard child output, no tailing */
    cbm_proc_log_cb on_log_line; /* optional per-line callback */
    void *log_ud;                /* user data for on_log_line */
    int quiet_timeout_ms;        /* <= 0 => no timeout; else kill+HANG after this many
                                  * ms with no new completed log line */
    bool delete_log_on_exit;     /* unlink log_file after reaping */
    bool discard_stderr;         /* redirect stderr to the null device instead of log_file */
    bool use_output_fd;          /* redirect stdout to output_fd without reopening a path */
    int output_fd;               /* caller-owned descriptor; valid when use_output_fd. When
                                  * on_log_line is set it must also be readable: supervision
                                  * tails this exact fd positionally without changing the
                                  * child's shared append offset. */
    size_t max_output_bytes;     /* 0 = unlimited; kill if output_fd grows beyond this */
    cbm_proc_control_t *control; /* optional stable child control; caller-owned and exclusive
                                  * to this run until cbm_subprocess_run() returns */
} cbm_proc_opts_t;

/* Spawn opts->bin, supervise (tail + optional quiet-timeout), block until it ends,
 * and classify the result into *out. Returns 0 if a child was spawned and reaped
 * (out filled), or -1 if the spawn itself failed (out->outcome == CBM_PROC_SPAWN_FAILED). */
int cbm_subprocess_run(const cbm_proc_opts_t *opts, cbm_proc_result_t *out);

/* Run an argv-based subprocess and capture its exact stdout byte stream
 * through one private temporary-file descriptor (stderr follows the supplied
 * discard_stderr policy). The process must exit cleanly and the
 * complete output must fit max_output_bytes; otherwise the call fails and no
 * partial bytes are returned. Binary output (including NULs) is supported.
 * out_sha256 is the digest of the logical output bytes. */
int cbm_subprocess_capture(const cbm_proc_opts_t *opts, size_t max_output_bytes, char **out_data,
                           size_t *out_len, char out_sha256[65], cbm_proc_result_t *out_result);

/* Pure outcome classifier — exposed so the platform-specific exit-code mapping
 * (notably the Windows NTSTATUS crash codes) is unit-testable on every platform.
 *   exited_normally: the child returned an exit code (POSIX WIFEXITED; always true
 *                    on Windows, which has no signals — crashes surface as codes).
 *   exit_code:       the exit / exception code (meaningful when exited_normally).
 *   term_signal:     POSIX terminating signal (meaningful when !exited_normally).
 *   timed_out:       we killed the child for exceeding the quiet-timeout. */
cbm_proc_outcome_t cbm_proc_classify(bool exited_normally, int exit_code, int term_signal,
                                     bool timed_out);

/* Stable lowercase name for an outcome (for structured logs / skip reasons). */
const char *cbm_proc_outcome_str(cbm_proc_outcome_t o);

/* Build a Windows CreateProcess command line from a NULL-terminated argv, applying
 * the Microsoft C runtime quoting rules (quote-wrap + escape embedded quotes and
 * their preceding backslashes) so the spawned child re-parses byte-identical argv.
 * Returns true on success, false on overflow (on overflow buf is set to an empty
 * string, never left unterminated).
 *
 * CreateProcess re-parses a SINGLE command string into argv, so a naive `"%s"` wrap
 * silently corrupts any element containing a double-quote — e.g. the index worker's
 * JSON arg {"repo_path":"…"} arrives as {repo_path:…}, the Windows index-worker bug.
 * Exposed (and compiled on every platform — it is pure string logic) so the quoting
 * is unit-tested on Linux/macOS CI, and so both spawn sites (cbm_subprocess_run and
 * the UI http_server index spawn) escape through one shared, tested implementation. */
bool cbm_build_win_cmdline(char *buf, size_t cap, const char *const *argv);

#endif /* CBM_SUBPROCESS_H */
