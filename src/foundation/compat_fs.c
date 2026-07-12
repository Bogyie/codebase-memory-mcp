/*
 * compat_fs.c — Portable file system operations.
 *
 * POSIX: direct wrappers around opendir/readdir/closedir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_fs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cbm_dir_test_fail_after = -1;

void cbm_dir_set_test_fail_after(int successful_entries) {
    cbm_dir_test_fail_after = successful_entries;
}

#ifdef _WIN32

/* ── Windows implementation ────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h> /* _wmkdir */
#include <errno.h>  /* errno for spawn-failure logging */
#include <fcntl.h>  /* _O_RDONLY */
#include <io.h>     /* _wunlink, _open_osfhandle, _close */
#include <stdint.h> /* intptr_t */
#include "foundation/log.h"
#include "foundation/win_utf8.h"

struct cbm_dir {
    HANDLE find_handle;
    WIN32_FIND_DATAW find_data;
    wchar_t wide_pattern[CBM_PATH_MAX];
    cbm_dirent_t entry;
    bool first;
    bool done;
    bool had_error;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }

    size_t wlen = wcslen(wpath);
    if (wlen == 0 || wlen + 2 >= CBM_PATH_MAX) {
        free(wpath);
        return NULL;
    }

    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        free(wpath);
        return NULL;
    }

    wmemcpy(d->wide_pattern, wpath, wlen + 1);
    wchar_t *p = d->wide_pattern + wlen - SKIP_ONE;
    if (*p != L'\\' && *p != L'/') {
        ++p;
        *p++ = L'\\';
    } else {
        ++p;
    }
    *p++ = L'*';
    *p = L'\0';
    free(wpath);

    d->find_handle = FindFirstFileW(d->wide_pattern, &d->find_data);
    if (d->find_handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = true;
    d->done = false;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || d->done) {
        return NULL;
    }
    if (cbm_dir_test_fail_after == 0) {
        cbm_dir_test_fail_after = -1;
        d->done = true;
        d->had_error = true;
        return NULL;
    }
    if (cbm_dir_test_fail_after > 0) {
        cbm_dir_test_fail_after--;
    }
    if (!d->first) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            d->had_error = GetLastError() != ERROR_NO_MORE_FILES;
            return NULL;
        }
    }
    d->first = false;

    while (d->find_data.cFileName[0] == L'.' &&
           (d->find_data.cFileName[1] == L'\0' ||
            (d->find_data.cFileName[1] == L'.' && d->find_data.cFileName[2] == L'\0'))) {
        if (!FindNextFileW(d->find_handle, &d->find_data)) {
            d->done = true;
            d->had_error = GetLastError() != ERROR_NO_MORE_FILES;
            return NULL;
        }
    }

    char *u8 = cbm_wide_to_utf8(d->find_data.cFileName);
    if (!u8) {
        d->done = true;
        d->had_error = true;
        return NULL;
    }
    size_t nlen = strlen(u8);
    if (nlen >= CBM_DIRENT_NAME_MAX) {
        free(u8);
        d->done = true;
        d->had_error = true;
        return NULL;
    }
    memcpy(d->entry.name, u8, nlen);
    d->entry.name[nlen] = '\0';
    free(u8);
    d->entry.is_dir = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->entry.is_reparse = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    d->entry.d_type = 0;
    return &d->entry;
}

bool cbm_dir_had_error(const cbm_dir_t *d) {
    return !d || d->had_error;
}

bool cbm_path_is_reparse_point(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return true;
    }
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    return attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

int cbm_path_probe(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return -1;
    }
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(d->find_handle);
        }
        free(d);
    }
}

/* Windows _popen replacement that inherits ONLY the child's stdout pipe.
 *
 * The CRT's _popen uses CreateProcess(bInheritHandles=TRUE), which leaks EVERY
 * inheritable handle we hold into the child — listening/client sockets, the
 * Winsock/AFD helper handles created by WSAStartup, the MCP stdio pipe, etc.
 * When the child is git-for-Windows (MSYS2/Cygwin runtime), its startup walks
 * every inherited handle and calls NtQueryObject on each to classify it; on an
 * inherited socket/AFD handle NtQueryObject deadlocks. Since our UI server runs
 * requests on a single thread, that wedges the whole server (list_projects,
 * which shells out to git per project, never returns → the web UI hangs).
 *
 * The fix: spawn via CreateProcessW with STARTUPINFOEXW + an explicit
 * PROC_THREAD_ATTRIBUTE_HANDLE_LIST containing only the stdout write-end and a
 * NUL handle for stdin/stderr. Nothing else crosses into git, so there is no
 * foreign handle to deadlock on. POSIX popen() already sets O_CLOEXEC on its
 * pipe, so the POSIX path is unchanged.
 *
 * There is deliberately NO fallback to _popen when the isolated spawn fails:
 * falling back would silently re-arm the deadlock. cbm_popen logs a structured
 * warning and returns NULL instead (every call site handles NULL). */

enum { CBM_POPEN_MAX = 16 };
static struct {
    FILE *fp;
    HANDLE proc;
} g_popen_tab[CBM_POPEN_MAX];
static CRITICAL_SECTION g_popen_lock;
static INIT_ONCE g_popen_once = INIT_ONCE_STATIC_INIT;

/* Test hook (declared in compat_fs_internal.h): 1 when the most recent
 * cbm_popen(..., "r") stream came from the isolated spawn. Test-only
 * observable; not synchronized across threads. */
static volatile LONG g_popen_last_isolated = 0;

int cbm_popen_last_was_isolated(void) {
    return (int)g_popen_last_isolated;
}

static BOOL CALLBACK cbm_popen_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_popen_lock);
    return TRUE;
}

/* Resolve the shell explicitly — %COMSPEC%, else <system dir>\cmd.exe — so it
 * can be passed as lpApplicationName and CreateProcess never walks the search
 * path (no cmd.exe planting from a hostile CWD). Heap string; caller frees. */
static wchar_t *cbm_resolve_comspec(void) {
    wchar_t buf[MAX_PATH];
    const wchar_t suffix[] = L"\\cmd.exe";
    DWORD n = GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        UINT sn = GetSystemDirectoryW(buf, MAX_PATH);
        if (sn == 0 || (size_t)sn + wcslen(suffix) >= MAX_PATH) {
            return NULL;
        }
        wmemcpy(buf + sn, suffix, wcslen(suffix) + 1);
    }
    return _wcsdup(buf);
}

/* On failure returns NULL with *stage naming the failing step and *gle the
 * GetLastError value captured at that step (0 when errno is the signal). */
static FILE *cbm_popen_isolated(const char *cmd, const char **stage, DWORD *gle) {
    *stage = "";
    *gle = 0;
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        *stage = "pipe";
        *gle = GetLastError();
        return NULL;
    }
    /* The parent read-end must never cross into the child. */
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* NUL for the child's stdin/stderr so it never touches our real stdin
     * pipe. If NUL cannot be opened, fail: STARTF_USESTDHANDLES slots must
     * never carry INVALID_HANDLE_VALUE. */
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        *stage = "nul";
        *gle = GetLastError();
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }

    HANDLE inherit[2];
    inherit[0] = wr;
    inherit[1] = nul;

    SIZE_T attr_sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
    LPPROC_THREAD_ATTRIBUTE_LIST attr = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_sz);
    BOOL attr_init = attr && InitializeProcThreadAttributeList(attr, 1, 0, &attr_sz);
    BOOL prepared =
        attr_init && UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               sizeof(inherit), NULL, NULL);
    DWORD attr_gle = prepared ? 0 : GetLastError();

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attr;

    /* Run through cmd.exe /c so command quoting and `2>NUL` behave as under
     * _popen. The command line is heap-composed (no fixed-size truncation)
     * and widened via UTF-8 so non-ASCII repo paths survive intact. */
    wchar_t *app = cbm_resolve_comspec();
    wchar_t *wcmdline = NULL;
    if (app) {
        size_t u8len = strlen(cmd) + sizeof("cmd.exe /c ");
        char *u8 = (char *)malloc(u8len);
        if (u8) {
            snprintf(u8, u8len, "cmd.exe /c %s", cmd);
            wcmdline = cbm_utf8_to_wide(u8);
            free(u8);
        }
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL created = FALSE;
    if (!prepared) {
        *stage = "attr";
        *gle = attr_gle;
    } else if (!app || !wcmdline) {
        *stage = "cmdline";
        *gle = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        created = CreateProcessW(app, wcmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                 NULL, NULL, &si.StartupInfo, &pi);
        if (!created) {
            *stage = "spawn";
            *gle = GetLastError();
        }
    }

    free(app);
    free(wcmdline);
    if (attr) {
        if (attr_init) {
            DeleteProcThreadAttributeList(attr);
        }
        free(attr);
    }
    CloseHandle(wr); /* the child owns the write-end now */
    CloseHandle(nul);
    if (!created) {
        CloseHandle(rd);
        return NULL;
    }
    CloseHandle(pi.hThread);

    int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY);
    if (fd == -1) {
        *stage = "osfhandle";
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    FILE *fp = _fdopen(fd, "r"); /* takes ownership of fd/rd */
    if (!fp) {
        *stage = "fdopen";
        _close(fd);
        CloseHandle(pi.hProcess);
        return NULL;
    }

    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (!g_popen_tab[i].fp) {
            g_popen_tab[i].fp = fp;
            g_popen_tab[i].proc = pi.hProcess;
            LeaveCriticalSection(&g_popen_lock);
            return fp;
        }
    }
    LeaveCriticalSection(&g_popen_lock);
    /* Table full (shouldn't happen): don't leak the process handle. */
    *stage = "table";
    CloseHandle(pi.hProcess);
    fclose(fp);
    return NULL;
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    /* Our git shell-outs are all read-mode; they MUST use the isolated
     * spawn. On failure, log and fail the call — never fall back to
     * _popen, whose full handle inheritance re-arms the UI hang (#798). */
    if (mode && mode[0] == 'r' && mode[1] == '\0') {
        const char *stage = "";
        DWORD gle = 0;
        FILE *fp = cbm_popen_isolated(cmd, &stage, &gle);
        g_popen_last_isolated = (fp != NULL);
        if (!fp) {
            char glebuf[CBM_SZ_16];
            char errnobuf[CBM_SZ_16];
            snprintf(glebuf, sizeof(glebuf), "%lu", (unsigned long)gle);
            snprintf(errnobuf, sizeof(errnobuf), "%d", errno);
            cbm_log_warn("compat.popen_isolated_failed", "stage", stage, "gle", glebuf, "errno",
                         errnobuf);
        }
        return fp;
    }
    g_popen_last_isolated = 0;
    return _popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    InitOnceExecuteOnce(&g_popen_once, cbm_popen_init, NULL, NULL);

    HANDLE proc = NULL;
    EnterCriticalSection(&g_popen_lock);
    for (int i = 0; i < CBM_POPEN_MAX; i++) {
        if (g_popen_tab[i].fp == f) {
            proc = g_popen_tab[i].proc;
            g_popen_tab[i].fp = NULL;
            g_popen_tab[i].proc = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_popen_lock);

    if (!proc) {
        return _pclose(f); /* opened via _popen (non-read mode) */
    }
    fclose(f);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    BOOL got = GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return got ? (int)code : -1;
}

FILE *cbm_fopen(const char *path, const char *mode) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }
    wchar_t *wmode = cbm_utf8_to_wide(mode);
    if (!wmode) {
        free(wpath);
        return NULL;
    }
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

static bool cbm_wpath_is_directory(const wchar_t *path) {
    if (!path || !path[0]) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool cbm_wmkdir_component(wchar_t *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (_wmkdir(path) == 0) {
        return true;
    }
    /* Preserve the historical symlink/junction policy: an existing reparse
     * point is accepted when Windows resolves it as a directory. A regular
     * file (including a file symlink) is never a successful mkdir -p step. */
    return cbm_wpath_is_directory(path);
}

static bool cbm_wpath_separator(wchar_t value) {
    return value == L'/' || value == L'\\';
}

static wchar_t *cbm_wskip_unc_root(wchar_t *path) {
    /* path points at the server component. Skip both \\server\share because
     * \\server by itself is an authority, not a directory that _wmkdir can
     * validate. */
    wchar_t *p = path;
    while (*p && !cbm_wpath_separator(*p)) {
        p++;
    }
    while (cbm_wpath_separator(*p)) {
        p++;
    }
    while (*p && !cbm_wpath_separator(*p)) {
        p++;
    }
    return *p ? p + SKIP_ONE : p;
}

static wchar_t *cbm_wmkdir_scan_start(wchar_t *path) {
    if (!path || !path[0]) {
        return path;
    }
    if (path[1] == L':' && cbm_wpath_separator(path[2])) {
        return path + 3; /* C:\ is an indivisible root. */
    }
    if (!cbm_wpath_separator(path[0]) || !cbm_wpath_separator(path[1])) {
        return path + SKIP_ONE;
    }
    wchar_t *p = path + 2;
    if (p[0] == L'?' && cbm_wpath_separator(p[1])) {
        p += 2;
        bool extended_unc = (p[0] == L'U' || p[0] == L'u') && (p[1] == L'N' || p[1] == L'n') &&
                            (p[2] == L'C' || p[2] == L'c') && cbm_wpath_separator(p[3]);
        if (extended_unc) {
            return cbm_wskip_unc_root(p + 4);
        }
        if (p[0] && p[1] == L':' && cbm_wpath_separator(p[2])) {
            return p + 3; /* \\?\C:\ */
        }
    }
    return cbm_wskip_unc_root(p);
}

bool cbm_mkdir_p(const char *path, int mode) {
    (void)mode;
    if (!path || !path[0]) {
        return false;
    }
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }
    size_t wlen = wcslen(wpath);
    wchar_t *tmp = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    if (!tmp) {
        free(wpath);
        return false;
    }
    wmemcpy(tmp, wpath, wlen + 1);
    for (wchar_t *p = cbm_wmkdir_scan_start(tmp); p && *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            wchar_t separator = *p;
            *p = L'\0';
            bool component_ok = cbm_wmkdir_component(tmp);
            *p = separator;
            if (!component_ok) {
                free(tmp);
                free(wpath);
                return false;
            }
        }
    }
    bool ok = cbm_wmkdir_component(tmp);
    free(tmp);
    free(wpath);
    return ok;
}

int cbm_unlink(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wunlink(wpath);
    free(wpath);
    return ret;
}

int cbm_rmdir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    int ret = _wrmdir(wpath);
    free(wpath);
    return ret;
}

/* Build a properly-quoted Windows command line from an argv array.
 * Returns a heap-allocated wide string, or NULL on allocation failure.
 * Quoting follows the MSVC CRT convention: arguments containing spaces,
 * tabs, or double-quotes are wrapped in double-quotes, with backslashes
 * before a closing quote doubled and the quote itself escaped. Argument
 * bytes are treated as UTF-8 and converted to wide via cbm_utf8_to_wide,
 * so non-ASCII arguments (e.g. a non-ASCII %USERPROFILE%) survive intact.
 * Declared in compat_fs_internal.h so the test suite can drive it. */
wchar_t *cbm_build_cmdline(const char *const *argv) {
    /* First pass: compute required buffer size. */
    size_t total = 1; /* NUL terminator */
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
            }
        }
        if (i > 0) {
            total++; /* space separator */
        }
        if (needs_quote) {
            total += 2; /* opening and closing quote */
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                } else if (*p == '"') {
                    total += backslashes + 1; /* double backslashes + escape backslash */
                    backslashes = 0;
                } else {
                    backslashes = 0;
                }
                total++;
            }
            /* Trailing backslashes before closing quote must be doubled. */
            total += backslashes;
        } else {
            total += strlen(arg);
        }
    }

    /* Build the quoted command line in UTF-8 first, then widen it as a
     * whole via cbm_utf8_to_wide. Every character the quoting logic acts
     * on (space, tab, '"', '\\') is ASCII and, by UTF-8's design, never
     * appears inside a multibyte sequence, so operating on raw bytes here
     * is safe and keeps multibyte argument bytes intact for conversion. */
    char *buf = (char *)malloc(total);
    if (!buf) {
        return NULL;
    }

    /* Second pass: write the command line bytes. */
    char *w = buf;
    for (int i = 0; argv[i]; i++) {
        const char *arg = argv[i];
        bool needs_quote = (arg[0] == '\0');
        for (const char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                needs_quote = true;
                break;
            }
        }
        if (i > 0) {
            *w++ = ' ';
        }
        if (needs_quote) {
            *w++ = '"';
            size_t backslashes = 0;
            for (const char *p = arg; *p; p++) {
                if (*p == '\\') {
                    backslashes++;
                    *w++ = '\\';
                } else if (*p == '"') {
                    /* Double the preceding backslashes, then escape the quote. */
                    for (size_t b = 0; b < backslashes; b++) {
                        *w++ = '\\';
                    }
                    *w++ = '\\';
                    *w++ = '"';
                    backslashes = 0;
                } else {
                    backslashes = 0;
                    *w++ = *p;
                }
            }
            /* Double trailing backslashes before the closing quote. */
            for (size_t b = 0; b < backslashes; b++) {
                *w++ = '\\';
            }
            *w++ = '"';
        } else {
            for (const char *p = arg; *p; p++) {
                *w++ = *p;
            }
        }
    }
    *w = '\0';

    wchar_t *out = cbm_utf8_to_wide(buf);
    free(buf);
    return out;
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }

    wchar_t *cmdline = cbm_build_cmdline(argv);
    if (!cmdline) {
        return CBM_NOT_FOUND;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        return CBM_NOT_FOUND;
    }
    free(cmdline);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = (DWORD)CBM_NOT_FOUND;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

#else /* POSIX */

/* ── POSIX implementation ────────────────────────────────── */

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct cbm_dir {
    DIR *dir;
    cbm_dirent_t entry;
    bool had_error;
};

cbm_dir_t *cbm_opendir(const char *path) {
    if (!path) {
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return NULL;
    }
    cbm_dir_t *d = (cbm_dir_t *)calloc(CBM_ALLOC_ONE, sizeof(cbm_dir_t));
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->dir = dir;
    return d;
}

cbm_dirent_t *cbm_readdir(cbm_dir_t *d) {
    if (!d || !d->dir) {
        return NULL;
    }
    if (cbm_dir_test_fail_after == 0) {
        cbm_dir_test_fail_after = -1;
        d->had_error = true;
        return NULL;
    }
    if (cbm_dir_test_fail_after > 0) {
        cbm_dir_test_fail_after--;
    }
    struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(d->dir);
        if (!de) {
            d->had_error = errno != 0;
            return NULL;
        }
        /* Skip "." and ".." */
        if (de->d_name[0] == '.' &&
            (de->d_name[SKIP_ONE] == '\0' ||
             (de->d_name[SKIP_ONE] == '.' && de->d_name[PAIR_LEN] == '\0'))) {
            continue;
        }
        size_t nlen = strlen(de->d_name);
        if (nlen >= CBM_DIRENT_NAME_MAX) {
            d->had_error = true;
            return NULL;
        }
        memcpy(d->entry.name, de->d_name, nlen);
        d->entry.name[nlen] = '\0';
        d->entry.is_dir = (de->d_type == DT_DIR);
        d->entry.is_reparse = (de->d_type == DT_LNK);
        d->entry.d_type = de->d_type;
        return &d->entry;
    }
}

bool cbm_dir_had_error(const cbm_dir_t *d) {
    return !d || d->had_error;
}

bool cbm_path_is_reparse_point(const char *path) {
    struct stat st;
    return !path || lstat(path, &st) != 0 || S_ISLNK(st.st_mode);
}

int cbm_path_probe(const char *path) {
    if (!path) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) == 0) {
        return 1;
    }
    return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}

void cbm_closedir(cbm_dir_t *d) {
    if (d) {
        if (d->dir) {
            closedir(d->dir);
        }
        free(d);
    }
}

FILE *cbm_popen(const char *cmd, const char *mode) {
    return popen(cmd, mode);
}

int cbm_pclose(FILE *f) {
    return pclose(f);
}

FILE *cbm_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

static bool cbm_mkdir_component(char *path, mode_t mode) {
    if (!path || !path[0]) {
        return false;
    }
    if (mkdir(path, mode) == 0) {
        return true;
    }
    struct stat st;
    /* stat() intentionally follows symlinks for compatibility: a symlink to
     * a directory remains a valid mkdir -p component, while a regular file or
     * a symlink to one is rejected. */
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool cbm_mkdir_p(const char *path, int mode) {
    if (!path || !path[0]) {
        return false;
    }
    char *tmp = strdup(path);
    if (!tmp) {
        return false;
    }
    for (char *p = tmp + SKIP_ONE; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            bool component_ok = cbm_mkdir_component(tmp, (mode_t)mode);
            *p = '/';
            if (!component_ok) {
                free(tmp);
                return false;
            }
        }
    }
    bool ok = cbm_mkdir_component(tmp, (mode_t)mode);
    free(tmp);
    return ok;
}

int cbm_unlink(const char *path) {
    return unlink(path);
}

int cbm_rmdir(const char *path) {
    return rmdir(path);
}

int cbm_exec_no_shell(const char *const *argv) {
    if (!argv || !argv[0]) {
        return CBM_NOT_FOUND;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return CBM_NOT_FOUND;
    }
    if (pid == 0) {
        /* Child: exec directly — no shell interpretation */
        /* 127 = standard "command not found" exit code (POSIX convention) */
        enum { EXEC_NOT_FOUND = 127 };
        execvp(argv[0], (char *const *)argv);
        _exit(EXEC_NOT_FOUND);
    }
    /* Parent: wait for child */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return CBM_NOT_FOUND;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return CBM_NOT_FOUND; /* killed by signal */
}

#endif /* _WIN32 */

/* Canonicalize an EXISTING path (collapse `..` and resolve symlinks/reparse
 * points): realpath on POSIX; a wide handle plus GetFinalPathNameByHandleW on
 * Windows. GetFullPathNameW is only lexical and leaves directory junctions in
 * place, which is not a safe containment primitive for callers that authorize
 * a path before reading it. The wide APIs also avoid routing UTF-8 through the
 * locale-dependent ANSI CRT (#973).
 * Returns 0 when the path does not exist or cannot be resolved. */
int cbm_canonical_path(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) {
        return 0;
    }
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return 0;
    }
    enum { CANON_WIDE_MAX = 4096 };
    HANDLE handle = CreateFileW(wpath, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wpath);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    wchar_t wfinal[CANON_WIDE_MAX];
    DWORD n = GetFinalPathNameByHandleW(handle, wfinal, CANON_WIDE_MAX,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (n == 0 || n >= CANON_WIDE_MAX) {
        return 0;
    }

    /* GetFinalPathNameByHandleW emits extended-length DOS paths. Convert
     * `\\?\C:\...` to `C:\...` and `\\?\UNC\server\share` to the normal
     * `\\server\share` spelling used by the rest of the codebase. */
    const wchar_t *normalized = wfinal;
    wchar_t wunc[CANON_WIDE_MAX];
    if (wcsncmp(wfinal, L"\\\\?\\UNC\\", 8) == 0) {
        size_t tail_len = wcslen(wfinal + 8);
        if (tail_len > CANON_WIDE_MAX - 3) {
            return 0;
        }
        wunc[0] = L'\\';
        wunc[1] = L'\\';
        wmemcpy(wunc + 2, wfinal + 8, tail_len + 1);
        normalized = wunc;
    } else if (wcsncmp(wfinal, L"\\\\?\\", 4) == 0 &&
               ((wfinal[4] >= L'A' && wfinal[4] <= L'Z') ||
                (wfinal[4] >= L'a' && wfinal[4] <= L'z')) &&
               wfinal[5] == L':') {
        normalized = wfinal + 4;
    }

    char *utf8 = cbm_wide_to_utf8(normalized);
    if (!utf8) {
        return 0;
    }
    size_t len = strlen(utf8);
    if (len >= out_sz) {
        free(utf8);
        return 0;
    }
    memcpy(out, utf8, len + 1);
    free(utf8);
    return 1;
#else
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        return 0;
    }
    size_t len = strlen(resolved);
    if (len >= out_sz) {
        free(resolved);
        return 0;
    }
    memcpy(out, resolved, len + 1U);
    free(resolved);
    return 1;
#endif
}

/* rename() with overwrite semantics on every platform: POSIX rename already
 * replaces atomically; Windows rename fails with EEXIST when the target
 * exists, so use MoveFileExW(MOVEFILE_REPLACE_EXISTING) there (wide paths —
 * raw MoveFileExA would re-mangle non-ASCII cache paths). */
int cbm_rename_replace(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t *wsrc = cbm_utf8_to_wide(src);
    wchar_t *wdst = cbm_utf8_to_wide(dst);
    int ret = CBM_NOT_FOUND;
    if (wsrc && wdst) {
        ret = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                  ? 0
                  : CBM_NOT_FOUND;
    }
    free(wsrc);
    free(wdst);
    return ret;
#else
    return rename(src, dst);
#endif
}

/* Remove a SQLite database's -wal/-shm sidecars (both platforms). Any code
 * path that installs a FRESH database file at a path where a previous
 * generation lived must call this first: SQLite decides whether to replay a
 * WAL purely from the sidecar's own header/checksums, so a leftover WAL
 * from a crashed session is recovered ON TOP of the freshly installed file
 * at the next open, splicing old-generation pages into it (#897). */
void cbm_remove_db_sidecars(const char *db_path) {
    if (!db_path || !db_path[0]) {
        return;
    }
    enum { SIDECAR_PATH_MAX = 4096 };
    char side[SIDECAR_PATH_MAX];
    int n = snprintf(side, sizeof(side), "%s-wal", db_path);
    if (n > 0 && (size_t)n < sizeof(side)) {
        (void)cbm_unlink(side);
    }
    n = snprintf(side, sizeof(side), "%s-shm", db_path);
    if (n > 0 && (size_t)n < sizeof(side)) {
        (void)cbm_unlink(side);
    }
}
