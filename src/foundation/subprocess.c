/*
 * subprocess.c — cross-platform spawn + supervise + classify.
 * See subprocess.h. The spawn/reap skeleton mirrors src/ui/http_server.c's
 * index subprocess; this generalizes it and adds crash/hang classification.
 */
#include "subprocess.h"

#include "compat.h" /* cbm_nanosleep */
#include "compat_fs.h"
#include "platform.h" /* cbm_now_ms */
#include "sha256.h"

#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include "win_utf8.h" /* cbm_utf8_to_wide — spawn the worker with a wide command line so a
                       * non-ASCII repo path survives CreateProcess (#423/#20) */
#include <io.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

/* NTSTATUS severity ERROR (top two bits set) covers the Windows crash exception
 * exit codes: 0xC0000005 (access violation), 0xC00000FD (stack overflow),
 * 0xC000001D (illegal instruction), 0xC0000094 (integer divide by zero), … */
#define CBM_WIN_CRASH_CODE_MIN 0xC0000000u

#ifndef _WIN32
static bool cbm_is_fault_signal(int sig) {
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
    case SIGFPE:
    case SIGABRT:
    case SIGSYS:
        return true;
    default:
        return false;
    }
}
#endif

cbm_proc_outcome_t cbm_proc_classify(bool exited_normally, int exit_code, int term_signal,
                                     bool timed_out) {
    if (timed_out) {
        return CBM_PROC_HANG;
    }
    if (!exited_normally) {
        /* POSIX signal death. */
#ifndef _WIN32
        if (cbm_is_fault_signal(term_signal)) {
            return CBM_PROC_CRASH;
        }
#else
        (void)term_signal;
#endif
        return CBM_PROC_KILLED;
    }
    /* Exited with a code. A Windows NTSTATUS exception code is a crash; on POSIX
     * exit codes are 0..255 so this branch never misfires there. */
    if ((unsigned)exit_code >= CBM_WIN_CRASH_CODE_MIN) {
        return CBM_PROC_CRASH;
    }
    return (exit_code == 0) ? CBM_PROC_CLEAN : CBM_PROC_EXIT_NONZERO;
}

const char *cbm_proc_outcome_str(cbm_proc_outcome_t o) {
    switch (o) {
    case CBM_PROC_CLEAN:
        return "clean";
    case CBM_PROC_EXIT_NONZERO:
        return "exit_nonzero";
    case CBM_PROC_CRASH:
        return "crash";
    case CBM_PROC_HANG:
        return "hang";
    case CBM_PROC_OUTPUT_LIMIT:
        return "output_limit";
    case CBM_PROC_KILLED:
        return "killed";
    case CBM_PROC_SPAWN_FAILED:
    default:
        return "spawn_failed";
    }
}

enum {
    CBM_PROC_CONTROL_IDLE = 0,
    CBM_PROC_CONTROL_RUNNING = 1,
    CBM_PROC_CONTROL_KILL_REQUESTED = 2,
    CBM_PROC_CONTROL_FINISHED = 3,
};

void cbm_proc_control_init(cbm_proc_control_t *control) {
    if (!control)
        return;
    atomic_store_explicit(&control->pid, 0, memory_order_relaxed);
    atomic_store_explicit(&control->state, CBM_PROC_CONTROL_IDLE, memory_order_release);
}

uint64_t cbm_proc_control_pid(const cbm_proc_control_t *control) {
    if (!control)
        return 0;
    int state = atomic_load_explicit(&control->state, memory_order_acquire);
    if (state != CBM_PROC_CONTROL_RUNNING && state != CBM_PROC_CONTROL_KILL_REQUESTED)
        return 0;
    uint64_t pid = atomic_load_explicit(&control->pid, memory_order_acquire);
    state = atomic_load_explicit(&control->state, memory_order_acquire);
    return (state == CBM_PROC_CONTROL_RUNNING || state == CBM_PROC_CONTROL_KILL_REQUESTED) ? pid
                                                                                           : 0;
}

bool cbm_proc_control_request_kill(cbm_proc_control_t *control, uint64_t expected_pid) {
    if (!control || expected_pid == 0 ||
        atomic_load_explicit(&control->pid, memory_order_acquire) != expected_pid) {
        return false;
    }
    int expected_state = CBM_PROC_CONTROL_RUNNING;
    return atomic_compare_exchange_strong_explicit(&control->state, &expected_state,
                                                   CBM_PROC_CONTROL_KILL_REQUESTED,
                                                   memory_order_acq_rel, memory_order_acquire);
}

static bool cbm_proc_control_prepare(cbm_proc_control_t *control) {
    if (!control)
        return true;
    int state = atomic_load_explicit(&control->state, memory_order_acquire);
    if (state == CBM_PROC_CONTROL_RUNNING || state == CBM_PROC_CONTROL_KILL_REQUESTED)
        return false;
    atomic_store_explicit(&control->pid, 0, memory_order_relaxed);
    atomic_store_explicit(&control->state, CBM_PROC_CONTROL_IDLE, memory_order_release);
    return true;
}

static void cbm_proc_control_publish(cbm_proc_control_t *control, uint64_t pid) {
    if (!control)
        return;
    atomic_store_explicit(&control->pid, pid, memory_order_relaxed);
    atomic_store_explicit(&control->state, CBM_PROC_CONTROL_RUNNING, memory_order_release);
}

static bool cbm_proc_control_kill_requested(const cbm_proc_control_t *control) {
    return control && atomic_load_explicit(&control->state, memory_order_acquire) ==
                          CBM_PROC_CONTROL_KILL_REQUESTED;
}

static void cbm_proc_control_finish(cbm_proc_control_t *control) {
    if (!control)
        return;
    /* Stop accepting requests before the native PID/HANDLE can become reusable. */
    atomic_store_explicit(&control->state, CBM_PROC_CONTROL_FINISHED, memory_order_release);
    atomic_store_explicit(&control->pid, 0, memory_order_release);
}

/* Tail newly-appended complete lines from the child log, starting at *tail_pos.
 * A partial (non-newline-terminated) final line is left buffered: *tail_pos is
 * not advanced past it, so it is re-read once completed. Returns true if any
 * complete line was consumed (i.e. there was progress). */
#ifdef _WIN32
static bool cbm_tail_log(const char *log_file, long *tail_pos, cbm_proc_log_cb cb, void *ud) {
    if (!log_file) {
        return false;
    }
    FILE *lf = cbm_fopen(log_file, "r");
    if (!lf) {
        return false;
    }
    bool progressed = false;
    if (fseek(lf, *tail_pos, SEEK_SET) == 0) {
        char line[1024];
        for (;;) {
            long before = ftell(lf);
            if (!fgets(line, sizeof(line), lf)) {
                break;
            }
            size_t l = strlen(line);
            bool complete = (l > 0 && line[l - 1] == '\n');
            if (complete) {
                line[l - 1] = '\0';
                if (l > 1 && line[l - 2] == '\r') {
                    line[l - 2] = '\0';
                }
                *tail_pos = ftell(lf);
                progressed = true;
                if (line[0] && cb) {
                    cb(line, ud);
                }
            } else if (l == sizeof(line) - 1) {
                /* Oversized line filled the buffer without a newline — consume it
                 * anyway (counts as progress) so we never stall on one long line. */
                *tail_pos = ftell(lf);
                progressed = true;
                if (cb) {
                    cb(line, ud);
                }
            } else {
                /* Genuine partial final line — keep it buffered for next poll. */
                *tail_pos = before;
                break;
            }
        }
    }
    fclose(lf);
    return progressed;
}

/* Read the caller-owned output descriptor without changing its shared file
 * pointer.  DuplicateHandle/_dup are insufficient here: duplicated Windows
 * file handles share the current offset with the child, so seeking to tail
 * output can redirect or overwrite a concurrent child write.  A read-only
 * file mapping addresses the already-published bytes by offset instead. */
static bool cbm_tail_log_fd(int fd, uint64_t *tail_pos, cbm_proc_log_cb cb, void *ud) {
    if (fd < 0 || !tail_pos || !cb) {
        return false;
    }
    intptr_t raw = _get_osfhandle(fd);
    if (raw == -1) {
        return false;
    }
    HANDLE file = (HANDLE)raw;
    LARGE_INTEGER size_value;
    if (!GetFileSizeEx(file, &size_value) || size_value.QuadPart < 0 ||
        (uint64_t)size_value.QuadPart <= *tail_pos) {
        return false;
    }
    uint64_t size = (uint64_t)size_value.QuadPart;
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        return false;
    }
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    uint64_t granularity = (uint64_t)system_info.dwAllocationGranularity;
    bool progressed = false;
    char line[1024];
    while (*tail_pos < size) {
        uint64_t aligned = (*tail_pos / granularity) * granularity;
        size_t delta = (size_t)(*tail_pos - aligned);
        uint64_t remaining = size - *tail_pos;
        size_t available = remaining < sizeof(line) - 1U ? (size_t)remaining : sizeof(line) - 1U;
        SIZE_T view_len = (SIZE_T)(delta + available);
        const unsigned char *view = (const unsigned char *)MapViewOfFile(
            mapping, FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)aligned, view_len);
        if (!view) {
            break;
        }
        memcpy(line, view + delta, available);
        UnmapViewOfFile(view);
        char *newline = (char *)memchr(line, '\n', available);
        if (newline) {
            size_t consumed = (size_t)(newline - line) + 1U;
            line[consumed - 1U] = '\0';
            if (consumed > 1U && line[consumed - 2U] == '\r') {
                line[consumed - 2U] = '\0';
            }
            *tail_pos += (uint64_t)consumed;
            progressed = true;
            if (line[0]) {
                cb(line, ud);
            }
            continue;
        }
        if (available == sizeof(line) - 1U) {
            line[available] = '\0';
            *tail_pos += (uint64_t)available;
            progressed = true;
            cb(line, ud);
            continue;
        }
        /* Genuine partial final line — leave it for the next poll. */
        break;
    }
    CloseHandle(mapping);
    return progressed;
}
#endif

#ifndef _WIN32
/* Tail the exact descriptor opened before fork. pread() leaves the shared
 * write offset untouched, so a pathname replacement cannot redirect progress
 * tracking and the child can continue appending through the same open file. */
static bool cbm_tail_log_fd(int fd, uint64_t *tail_pos, cbm_proc_log_cb cb, void *ud) {
    if (fd < 0 || !tail_pos || *tail_pos > (uint64_t)INT64_MAX) {
        return false;
    }
    bool progressed = false;
    char line[1024];
    for (;;) {
        ssize_t got;
        do {
            got = pread(fd, line, sizeof(line) - 1U, (off_t)*tail_pos);
        } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            break;
        }
        size_t available = (size_t)got;
        char *newline = memchr(line, '\n', available);
        if (newline) {
            size_t consumed = (size_t)(newline - line) + 1U;
            line[consumed - 1U] = '\0';
            if (consumed > 1U && line[consumed - 2U] == '\r') {
                line[consumed - 2U] = '\0';
            }
            *tail_pos += (uint64_t)consumed;
            progressed = true;
            if (line[0] && cb) {
                cb(line, ud);
            }
            continue;
        }
        if (available == sizeof(line) - 1U) {
            line[available] = '\0';
            *tail_pos += (uint64_t)available;
            progressed = true;
            if (cb) {
                cb(line, ud);
            }
            continue;
        }
        /* Partial trailing line: wait for its newline. */
        break;
    }
    return progressed;
}
#endif

/* Binary captures such as `git log -z` may contain no newlines at all, so the
 * line tailer cannot be the quiet-timeout progress signal. Observe descriptor
 * length instead without disturbing its shared write offset. */
static bool cbm_output_fd_grew(int fd, uint64_t *last_size) {
    if (fd < 0 || !last_size) {
        return false;
    }
#ifdef _WIN32
    __int64 size = _filelengthi64(fd);
    if (size < 0) {
        return false;
    }
    uint64_t current = (uint64_t)size;
#else
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        return false;
    }
    uint64_t current = (uint64_t)st.st_size;
#endif
    if (current <= *last_size) {
        return false;
    }
    *last_size = current;
    return true;
}

/* ── Windows command-line quoting (pure; unit-tested on every platform) ─────── */

/* Append char `c` to buf[cap], reserving the final byte for a NUL terminator.
 * On overflow: sets *ovf, stops writing, and returns pos UNCHANGED — callers detect
 * the overflow via the *ovf flag (not via the return value). */
static size_t cbm_cmdline_put(char *buf, size_t cap, size_t pos, char c, bool *ovf) {
    if (pos + 1 >= cap) {
        *ovf = true;
        return pos;
    }
    buf[pos] = c;
    return pos + 1;
}

/* Append one argv element to the command line using the Microsoft C runtime
 * quoting rules (see MS "Parsing C Command-Line Arguments"). CreateProcess takes
 * a SINGLE string that the child re-parses back into argv, so any element with a
 * space, tab or double-quote must be wrapped in quotes and its embedded quotes /
 * preceding backslashes escaped. Without this a JSON argument like
 * {"repo_path":"C:/r"} loses its inner quotes and the child receives the invalid
 * {repo_path:C:/r} — the Windows-only index-worker cmdline-quoting bug (the worker exited
 * non-zero at JSON-arg parse, misattributed to the last-marked file). POSIX is
 * unaffected: cbm_run_posix passes the argv array straight to execv. */
static size_t cbm_cmdline_append_arg(char *buf, size_t cap, size_t pos, const char *arg, bool first,
                                     bool *ovf) {
    if (!first) {
        pos = cbm_cmdline_put(buf, cap, pos, ' ', ovf);
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    for (const char *p = arg; *p;) {
        size_t nbs = 0;
        while (*p == '\\') {
            nbs++;
            p++;
        }
        if (*p == '\0') {
            /* Trailing backslashes precede the closing quote: double them so the
             * quote stays a delimiter, not an escaped literal. */
            for (size_t k = 0; k < nbs * 2; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            break;
        }
        if (*p == '"') {
            /* N backslashes then a quote -> 2N+1 backslashes then an escaped quote. */
            for (size_t k = 0; k < nbs * 2 + 1; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
            p++;
        } else {
            for (size_t k = 0; k < nbs; k++) {
                pos = cbm_cmdline_put(buf, cap, pos, '\\', ovf);
            }
            pos = cbm_cmdline_put(buf, cap, pos, *p, ovf);
            p++;
        }
    }
    pos = cbm_cmdline_put(buf, cap, pos, '"', ovf);
    return pos;
}

/* Build a full Windows CreateProcess command line from a NULL-terminated argv,
 * applying the MS C runtime quoting rules so the child re-parses byte-identical
 * argv. Returns true on success, false if the result would overflow `buf`.
 *
 * Defined unconditionally (pure string logic, no Windows headers) so the quoting
 * contract is unit-tested on Linux/macOS CI too — even though the real spawn path
 * only runs on Windows. Shared by cbm_run_win AND the UI http_server index spawn
 * so both escape identically; a naive `"%s"` wrap silently corrupts any argument
 * containing a quote (e.g. the index JSON {"repo_path":"…"}), corrupting the
 * spawned child's argv. */
bool cbm_build_win_cmdline(char *buf, size_t cap, const char *const *argv) {
    if (!buf || cap == 0 || !argv) {
        return false;
    }
    size_t pos = 0;
    bool ovf = false;
    for (int i = 0; argv[i]; i++) {
        pos = cbm_cmdline_append_arg(buf, cap, pos, argv[i], i == 0, &ovf);
        if (ovf) {
            buf[0] = '\0'; /* overflow: leave buf a valid (empty) string, never unterminated */
            return false;
        }
    }
    buf[pos] = '\0';
    return true;
}

#ifdef _WIN32

static int cbm_run_win(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    const char *bin = opts->bin;
    const char *const default_argv[] = {bin, NULL};
    const char *const *argv = opts->argv ? opts->argv : default_argv;

    char cmdline[8192];
    if (!cbm_build_win_cmdline(cmdline, sizeof(cmdline), argv)) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    /* Spawn via CreateProcessW with a WIDE command line. CreateProcessA would
     * re-interpret our UTF-8 cmdline bytes through the ANSI code page (CP_ACP),
     * re-mangling a non-ASCII repo path at the parent->worker boundary — so the
     * worker's own wide-argv read could never recover it (#423/#20). */
    wchar_t *wcmd = cbm_utf8_to_wide(cmdline);
    if (!wcmd) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }

    HANDLE hlog = INVALID_HANDLE_VALUE;
    HANDLE herr = INVALID_HANDLE_VALUE;
    HANDLE hin = INVALID_HANDLE_VALUE;
    STARTUPINFOEXW sx;
    memset(&sx, 0, sizeof(sx));
    sx.StartupInfo.cb = sizeof(sx);
    SECURITY_ATTRIBUTES inherit = {
        .nLength = sizeof(inherit), .lpSecurityDescriptor = NULL, .bInheritHandle = TRUE};
    if (opts->use_output_fd) {
        intptr_t raw_handle = _get_osfhandle(opts->output_fd);
        if (raw_handle != -1 &&
            !DuplicateHandle(GetCurrentProcess(), (HANDLE)raw_handle, GetCurrentProcess(), &hlog, 0,
                             TRUE, DUPLICATE_SAME_ACCESS)) {
            hlog = INVALID_HANDLE_VALUE;
        }
    } else if (opts->log_file) {
        wchar_t *wlog = cbm_utf8_to_wide(opts->log_file);
        if (wlog) {
            hlog = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
            free(wlog);
        }
    } else {
        hlog = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hlog != INVALID_HANDLE_VALUE) {
        hin = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (opts->discard_stderr) {
            herr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }

    bool attributes_initialized = false;
    bool startup_ok = hlog != INVALID_HANDLE_VALUE && hin != INVALID_HANDLE_VALUE &&
                      (!opts->discard_stderr || herr != INVALID_HANDLE_VALUE);
    if (startup_ok) {
        HANDLE inherited[3];
        SIZE_T inherited_count = 0;
        sx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        sx.StartupInfo.hStdInput = hin;
        sx.StartupInfo.hStdOutput = hlog;
        sx.StartupInfo.hStdError = herr != INVALID_HANDLE_VALUE ? herr : hlog;
        inherited[inherited_count++] = hin;
        inherited[inherited_count++] = hlog;
        if (herr != INVALID_HANDLE_VALUE && herr != hlog) {
            inherited[inherited_count++] = herr;
        }
        SIZE_T attribute_bytes = 0;
        (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
        if (attribute_bytes == 0) {
            startup_ok = false;
        } else {
            sx.lpAttributeList =
                (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attribute_bytes);
            attributes_initialized =
                sx.lpAttributeList &&
                InitializeProcThreadAttributeList(sx.lpAttributeList, 1, 0, &attribute_bytes);
            startup_ok = attributes_initialized &&
                         UpdateProcThreadAttribute(
                             sx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                             inherited_count * sizeof(inherited[0]), NULL, NULL);
        }
    }

    PROCESS_INFORMATION pi = {0};
    /* A NULL application name makes CreateProcess resolve argv[0] through the
     * normal executable search path. Passing a bare name as lpApplicationName
     * does not search PATH, which made tools such as git fail to spawn. */
    BOOL ok =
        startup_ok && CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                     NULL, NULL, &sx.StartupInfo, &pi);
    if (sx.lpAttributeList) {
        if (attributes_initialized) {
            DeleteProcThreadAttributeList(sx.lpAttributeList);
        }
        HeapFree(GetProcessHeap(), 0, sx.lpAttributeList);
    }
    free(wcmd);
    if (hlog != INVALID_HANDLE_VALUE) {
        CloseHandle(hlog);
    }
    if (herr != INVALID_HANDLE_VALUE) {
        CloseHandle(herr);
    }
    if (hin != INVALID_HANDLE_VALUE) {
        CloseHandle(hin);
    }
    if (!ok) {
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    cbm_proc_control_publish(opts->control, (uint64_t)pi.dwProcessId);

    long path_tail_pos = 0;
    uint64_t fd_tail_pos = 0;
    uint64_t output_size = 0;
    uint64_t last_activity = cbm_now_ms();
    bool timed_out = false;
    bool output_limited = false;
    bool control_killed = false;
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        bool line_progress =
            opts->use_output_fd
                ? cbm_tail_log_fd(opts->output_fd, &fd_tail_pos, opts->on_log_line, opts->log_ud)
                : cbm_tail_log(opts->log_file, &path_tail_pos, opts->on_log_line, opts->log_ud);
        if (line_progress) {
            last_activity = cbm_now_ms();
        }
        if (opts->use_output_fd && cbm_output_fd_grew(opts->output_fd, &output_size)) {
            last_activity = cbm_now_ms();
        }
        if (opts->max_output_bytes > 0 && output_size > opts->max_output_bytes) {
            if (w != WAIT_OBJECT_0) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, INFINITE);
            }
            output_limited = true;
            break;
        }
        if (w == WAIT_OBJECT_0) {
            break;
        }
        if (cbm_proc_control_kill_requested(opts->control)) {
            (void)TerminateProcess(pi.hProcess, 1);
            (void)WaitForSingleObject(pi.hProcess, INFINITE);
            control_killed = true;
            break;
        }
        if (opts->quiet_timeout_ms > 0 &&
            (cbm_now_ms() - last_activity) >= (uint64_t)opts->quiet_timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            timed_out = true;
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    /* Stop accepting requests while the retained handle still prevents reuse. */
    cbm_proc_control_finish(opts->control);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (opts->log_file && opts->delete_log_on_exit) {
        wchar_t *wlog = cbm_utf8_to_wide(opts->log_file);
        if (wlog) {
            (void)DeleteFileW(wlog);
            free(wlog);
        }
    }

    out->exit_code = (int)code;
    out->term_signal = 0;
    out->outcome = cbm_proc_classify(true, (int)code, 0, timed_out);
    if (output_limited) {
        out->outcome = CBM_PROC_OUTPUT_LIMIT;
    } else if (control_killed) {
        out->outcome = CBM_PROC_KILLED;
    }
    return 0;
}

#else /* POSIX */

static bool cbm_kill_and_reap(pid_t pid, int *wstatus) {
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        return false;
    }
    pid_t wr;
    do {
        wr = waitpid(pid, wstatus, 0);
    } while (wr < 0 && errno == EINTR);
    return wr == pid;
}

static int cbm_prepare_log_fd(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    /* O_RDWR is intentional: the parent tails this exact descriptor with
     * pread(), while the child appends through its inherited duplicate. */
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    struct stat opened;
    struct stat named;
    if (fstat(fd, &opened) != 0 || lstat(path, &named) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino || opened.st_nlink != 1 ||
        opened.st_uid != geteuid() || ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Called after fork and before exec: keep only stdio. Prefer one kernel call on
 * Linux; the bounded close loop is the portable fallback. All inputs are
 * computed by the parent, so this path stays async-signal-safe. */
static void cbm_child_close_nonstdio(int max_fd) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, (unsigned int)(STDERR_FILENO + 1), ~0U, 0U) == 0) {
        return;
    }
#endif
    for (int fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
        (void)close(fd);
    }
}

/* Determine the highest descriptor that exists immediately before fork. This
 * avoids a million close() syscalls on macOS where OPEN_MAX is commonly
 * 1,048,575. Project-owned descriptors are also CLOEXEC, covering descriptors
 * opened by another thread in the narrow scan-to-fork window. */
static int cbm_parent_close_bound(void) {
    int highest = STDERR_FILENO;
    DIR *dir = opendir("/dev/fd");
    if (!dir)
        dir = opendir("/proc/self/fd");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char *end = NULL;
            long value = strtol(entry->d_name, &end, 10);
            if (end != entry->d_name && *end == '\0' && value > highest && value < INT_MAX)
                highest = (int)value;
        }
        (void)closedir(dir);
        return highest + 1;
    }
    long open_max = sysconf(_SC_OPEN_MAX);
    return open_max > STDERR_FILENO && open_max <= 65536 ? (int)open_max : 65536;
}

static int cbm_run_posix(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    int prepared_log_fd = -1;
    if (!opts->use_output_fd && opts->log_file) {
        prepared_log_fd = cbm_prepare_log_fd(opts->log_file);
        if (prepared_log_fd < 0) {
            out->outcome = CBM_PROC_SPAWN_FAILED;
            out->exit_code = -1;
            out->term_signal = 0;
            return -1;
        }
    }
    int max_fd = cbm_parent_close_bound();
    pid_t pid = fork();
    if (pid < 0) {
        if (prepared_log_fd >= 0) {
            close(prepared_log_fd);
        }
        out->outcome = CBM_PROC_SPAWN_FAILED;
        out->exit_code = -1;
        out->term_signal = 0;
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout+stderr to the log (or discard), then exec.
         * Use open()+dup2() (async-signal-safe, no malloc) rather than freopen():
         * the parent may be multithreaded (the MCP server holds worker/watcher/http
         * threads plus mimalloc/sqlite global state), and a fork() copies
         * only the calling thread — a malloc between fork and exec could deadlock on
         * a lock another thread held at fork time. open/dup2/execv touch no heap. */
        const char *bin = opts->bin;
        const char *const default_argv[] = {bin, NULL};
        const char *const *argv = opts->argv ? opts->argv : default_argv;
        int fd = opts->use_output_fd
                     ? opts->output_fd
                     : (prepared_log_fd >= 0 ? prepared_log_fd : open("/dev/null", O_WRONLY));
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
            (!opts->discard_stderr && dup2(fd, STDERR_FILENO) < 0)) {
            _exit(127);
        }
        int infd = open("/dev/null", O_RDONLY);
        if (infd < 0 || dup2(infd, STDIN_FILENO) < 0) {
            _exit(127);
        }
        if (infd > STDERR_FILENO) {
            (void)close(infd);
        }
        if (fd > STDERR_FILENO) {
            (void)close(fd);
        }
        if (opts->discard_stderr) {
            int errfd = open("/dev/null", O_WRONLY);
            if (errfd < 0 || dup2(errfd, STDERR_FILENO) < 0) {
                _exit(127);
            }
            if (errfd > STDERR_FILENO) {
                (void)close(errfd);
            }
        }
        cbm_child_close_nonstdio(max_fd);
        execv(bin, (char *const *)argv);
        _exit(127); /* exec failed */
    }
    cbm_proc_control_publish(opts->control, (uint64_t)pid);

    uint64_t tail_pos = 0;
    uint64_t output_size = 0;
    uint64_t last_activity = cbm_now_ms();
    bool timed_out = false;
    bool output_limited = false;
    bool control_killed = false;
    bool wait_failed = false;
    int wstatus = 0;
    for (;;) {
        pid_t wr;
        do {
            wr = waitpid(pid, &wstatus, WNOHANG);
        } while (wr < 0 && errno == EINTR);
        if (wr < 0) {
            wait_failed = true;
            break;
        }
        bool done = (wr == pid);

        int tail_fd = opts->use_output_fd ? opts->output_fd : prepared_log_fd;
        if (cbm_tail_log_fd(tail_fd, &tail_pos, opts->on_log_line, opts->log_ud)) {
            last_activity = cbm_now_ms();
        }
        if (opts->use_output_fd && cbm_output_fd_grew(opts->output_fd, &output_size)) {
            last_activity = cbm_now_ms();
        }
        if (opts->max_output_bytes > 0 && output_size > opts->max_output_bytes) {
            if (!done && !cbm_kill_and_reap(pid, &wstatus)) {
                wait_failed = true;
            }
            output_limited = true;
            break;
        }
        if (done) {
            break;
        }
        if (cbm_proc_control_kill_requested(opts->control)) {
            if (!cbm_kill_and_reap(pid, &wstatus)) {
                wait_failed = true;
            } else {
                control_killed = true;
            }
            break;
        }
        if (opts->quiet_timeout_ms > 0 &&
            (cbm_now_ms() - last_activity) >= (uint64_t)opts->quiet_timeout_ms) {
            if (!cbm_kill_and_reap(pid, &wstatus)) {
                wait_failed = true;
            }
            timed_out = true;
            break;
        }
        struct timespec ts = {0, 100000000L}; /* 100 ms poll */
        cbm_nanosleep(&ts, NULL);
    }

    if (opts->log_file && opts->delete_log_on_exit) {
        (void)unlink(opts->log_file);
    }
    if (prepared_log_fd >= 0) {
        (void)close(prepared_log_fd);
    }
    cbm_proc_control_finish(opts->control);

    if (wait_failed) {
        out->exit_code = -1;
        out->term_signal = 0;
        out->outcome = CBM_PROC_KILLED;
    } else if (WIFEXITED(wstatus)) {
        out->exit_code = WEXITSTATUS(wstatus);
        out->term_signal = 0;
        out->outcome = cbm_proc_classify(true, out->exit_code, 0, timed_out);
    } else if (WIFSIGNALED(wstatus)) {
        out->exit_code = -1;
        out->term_signal = WTERMSIG(wstatus);
        out->outcome = cbm_proc_classify(false, -1, out->term_signal, timed_out);
    } else {
        out->exit_code = -1;
        out->term_signal = 0;
        out->outcome = timed_out ? CBM_PROC_HANG : CBM_PROC_KILLED;
    }
    if (output_limited && !wait_failed) {
        out->outcome = CBM_PROC_OUTPUT_LIMIT;
    } else if (control_killed && !wait_failed) {
        out->outcome = CBM_PROC_KILLED;
    }
    return 0;
}

#endif

int cbm_subprocess_run(const cbm_proc_opts_t *opts, cbm_proc_result_t *out) {
    cbm_proc_result_t local;
    if (!out) {
        out = &local;
    }
    out->outcome = CBM_PROC_SPAWN_FAILED;
    out->exit_code = -1;
    out->term_signal = 0;
    if (!opts || !opts->bin || !opts->bin[0] || (opts->use_output_fd && opts->output_fd < 0))
        return -1;
    if (!cbm_proc_control_prepare(opts->control))
        return -1;
    int rc;
#ifdef _WIN32
    rc = cbm_run_win(opts, out);
#else
    rc = cbm_run_posix(opts, out);
#endif
    if (rc != 0)
        cbm_proc_control_finish(opts->control);
    return rc;
}

static int cbm_capture_fd_read_hash(int fd, size_t max_output_bytes, char **out_data,
                                    size_t *out_len, char out_sha256[65]) {
#ifdef _WIN32
    struct _stat64 before;
    if (_fstat64(fd, &before) != 0 || before.st_size < 0 ||
        (uint64_t)before.st_size > (uint64_t)max_output_bytes ||
        (uint64_t)before.st_size > (uint64_t)(SIZE_MAX - 1U) || _lseeki64(fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    size_t expected = (size_t)before.st_size;
#else
    struct stat before;
    if (fstat(fd, &before) != 0 || before.st_size < 0 ||
        (uint64_t)before.st_size > (uint64_t)max_output_bytes ||
        (uint64_t)before.st_size > (uint64_t)(SIZE_MAX - 1U) || lseek(fd, 0, SEEK_SET) < 0) {
        return -1;
    }
    size_t expected = (size_t)before.st_size;
#endif
    char *data = malloc(expected + 1U);
    if (!data) {
        return -1;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    size_t total = 0;
    while (total < expected) {
        size_t remaining = expected - total;
#ifdef _WIN32
        unsigned int chunk =
            remaining > (size_t)INT_MAX ? (unsigned int)INT_MAX : (unsigned int)remaining;
        int nread = _read(fd, data + total, chunk);
        if (nread <= 0) {
            free(data);
            return -1;
        }
        size_t got = (size_t)nread;
#else
        ssize_t nread = read(fd, data + total, remaining);
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        if (nread <= 0) {
            free(data);
            return -1;
        }
        size_t got = (size_t)nread;
#endif
        cbm_sha256_update(&hash, data + total, got);
        total += got;
    }

    char extra;
#ifdef _WIN32
    int extra_read = _read(fd, &extra, 1U);
    struct _stat64 after;
    bool stable = extra_read == 0 && _fstat64(fd, &after) == 0 && after.st_size == before.st_size &&
                  after.st_mtime == before.st_mtime;
#else
    ssize_t extra_read;
    do {
        extra_read = read(fd, &extra, 1U);
    } while (extra_read < 0 && errno == EINTR);
    struct stat after;
    bool stable = extra_read == 0 && fstat(fd, &after) == 0 && after.st_dev == before.st_dev &&
                  after.st_ino == before.st_ino && after.st_size == before.st_size &&
                  after.st_mtime == before.st_mtime;
#endif
    if (!stable) {
        free(data);
        return -1;
    }
    data[total] = '\0';
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out_sha256[i * 2] = hex[digest[i] >> 4];
        out_sha256[i * 2 + 1] = hex[digest[i] & 0x0fU];
    }
    out_sha256[CBM_SHA256_HEX_LEN] = '\0';
    *out_data = data;
    *out_len = total;
    return 0;
}

int cbm_subprocess_capture(const cbm_proc_opts_t *opts, size_t max_output_bytes, char **out_data,
                           size_t *out_len, char out_sha256[65], cbm_proc_result_t *out_result) {
    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out_sha256) {
        out_sha256[0] = '\0';
    }
    cbm_proc_result_t local_result = {
        .outcome = CBM_PROC_SPAWN_FAILED, .exit_code = -1, .term_signal = 0};
    cbm_proc_result_t *result = out_result ? out_result : &local_result;
    *result = local_result;
    if (!opts || !out_data || !out_len || !out_sha256 || max_output_bytes == 0) {
        return -1;
    }

    const char *tmp_dir = cbm_tmpdir();
    if (!tmp_dir) {
        return -1;
    }
    size_t tmp_len = strlen(tmp_dir);
    static const char suffix[] = "/cbm-subprocess-XXXXXX";
    if (tmp_len > SIZE_MAX - sizeof(suffix)) {
        return -1;
    }
    char *tmp_path = (char *)malloc(tmp_len + sizeof(suffix));
    if (!tmp_path) {
        return -1;
    }
    memcpy(tmp_path, tmp_dir, tmp_len);
    memcpy(tmp_path + tmp_len, suffix, sizeof(suffix));
    int tmp_fd = cbm_mkstemp(tmp_path);
    if (tmp_fd < 0) {
        free(tmp_path);
        return -1;
    }
#ifndef _WIN32
    if (fcntl(tmp_fd, F_SETFD, FD_CLOEXEC) != 0) {
        (void)close(tmp_fd);
        (void)unlink(tmp_path);
        free(tmp_path);
        return -1;
    }
#endif
#ifndef _WIN32
    /* The child inherits this descriptor directly; remove the directory entry
     * before spawning so no path substitution can affect captured bytes. */
    (void)unlink(tmp_path);
#endif

    cbm_proc_opts_t capture_opts = *opts;
    capture_opts.log_file = NULL;
    capture_opts.on_log_line = NULL;
    capture_opts.log_ud = NULL;
    capture_opts.delete_log_on_exit = false;
    capture_opts.use_output_fd = true;
    capture_opts.output_fd = tmp_fd;
    capture_opts.max_output_bytes = max_output_bytes;
    int run_rc = cbm_subprocess_run(&capture_opts, result);
    int read_rc = -1;
    if (run_rc == 0 && result->outcome == CBM_PROC_CLEAN && result->exit_code == 0) {
        read_rc = cbm_capture_fd_read_hash(tmp_fd, max_output_bytes, out_data, out_len, out_sha256);
    }
#ifdef _WIN32
    (void)_close(tmp_fd);
#else
    (void)close(tmp_fd);
#endif
    (void)cbm_unlink(tmp_path);
    free(tmp_path);
    if (read_rc != 0) {
        free(*out_data);
        *out_data = NULL;
        *out_len = 0;
        out_sha256[0] = '\0';
        return -1;
    }
    return 0;
}
