/*
 * platform.c — OS abstraction implementations.
 *
 * macOS, Linux, and Windows. Platform-specific code behind #ifdef guards.
 */
#include "platform.h"

#include "compat.h"
#include "foundation/constants.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Canonicalize a Windows drive letter to upper-case in place: "c:/x" -> "C:/x".
 * Windows drive letters are case-insensitive, but a lowercase one (as agent
 * CWDs often report, e.g. Claude Code's "c:\...") otherwise produces a distinct
 * project key ("c-..." vs "C-...") and, on a case-insensitive FS, a colliding
 * cache file that clobbers the good index (#227/#367/#394). Folding to a single
 * canonical form here — at the one path-normalization choke point — keeps the
 * project key, cache file and integrity check consistent regardless of case.
 * Only the strict drive-root form `X:/` or bare `X:` is touched, so ordinary
 * POSIX paths (which never start that way) are unaffected. */
static void cbm_canonicalize_drive(char *path) {
    if (path && path[0] >= 'a' && path[0] <= 'z' && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\0')) {
        path[0] = (char)(path[0] - 'a' + 'A');
    }
}

uint64_t cbm_qpc_ticks_to_ns(uint64_t ticks, uint64_t frequency) {
    if (frequency == 0) {
        return 0;
    }
    /* QPC frequencies are bounded well below UINT64_MAX / 1e9 on supported
     * Windows systems. Splitting the quotient and remainder avoids the much
     * earlier overflow in ticks * 1e9 (about 30 minutes at 10 MHz). */
    uint64_t seconds = ticks / frequency;
    uint64_t remainder = ticks % frequency;
    return seconds * 1000000000ULL + (remainder * 1000000000ULL) / frequency;
}

#ifdef _WIN32

/* ── Windows implementation ────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include "foundation/win_utf8.h"

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return NULL;
    }

    HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(wpath);
        return NULL;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart == 0) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    void *addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    CloseHandle(file);
    free(wpath);
    if (!addr) {
        return NULL;
    }
    *out_size = (size_t)sz.QuadPart;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    (void)size;
    if (addr) {
        UnmapViewOfFile(addr);
    }
}

uint64_t cbm_now_ns(void) {
    LARGE_INTEGER freq, count;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&count) || count.QuadPart < 0) {
        return 0;
    }
    return cbm_qpc_ticks_to_ns((uint64_t)count.QuadPart, (uint64_t)freq.QuadPart);
}

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

int cbm_nprocs(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool cbm_file_exists(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool cbm_is_dir(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t cbm_file_size(const char *path) {
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) {
        return CBM_NOT_FOUND;
    }
    LARGE_INTEGER sz;
    sz.HighPart = (LONG)fad.nFileSizeHigh; // cppcheck-suppress unreadVariable
    sz.LowPart = fad.nFileSizeLow;         // cppcheck-suppress unreadVariable
    return (int64_t)sz.QuadPart;
}

char *cbm_normalize_path_sep(char *path) {
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#else /* POSIX (macOS + Linux) */

/* ── POSIX implementation ────────────────────────────────── */

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#else
#include <sched.h>
#endif

/* ── Memory mapping ──────────────────────────── */

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return NULL;
    }

    void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        return NULL;
    }
    *out_size = (size_t)st.st_size;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    if (addr && size > 0) {
        munmap(addr, size);
    }
}

/* ── Timing ───────────────────────────── */

#ifdef __APPLE__
static mach_timebase_info_data_t timebase_info;
static int timebase_init = 0;

uint64_t cbm_now_ns(void) {
    if (!timebase_init) {
        mach_timebase_info(&timebase_info);
        timebase_init = SKIP_ONE;
    }
    uint64_t ticks = mach_absolute_time();
    return ticks * timebase_info.numer / timebase_info.denom;
}
#else
uint64_t cbm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

/* ── System info ───────────────────────────── */

int cbm_nprocs(void) {
#ifdef __APPLE__
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        return ncpu;
    }
    enum { FILE_EXISTS = 1 };
    return FILE_EXISTS;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ── File system ──────────────────────────── */

bool cbm_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool cbm_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t cbm_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return CBM_NOT_FOUND;
    }
    return (int64_t)st.st_size;
}

char *cbm_normalize_path_sep(char *path) {
    /* This helper also accepts serialized/remote Windows paths on POSIX. Local
     * POSIX environment resolvers deliberately do not call it because a
     * backslash is an ordinary filename byte there. */
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#endif /* _WIN32 */

/* ── Environment variables ──────────────────────────── */

/* Copy environment values instead of returning process-environment storage.
 * Like getenv(), this requires callers not to mutate the process environment
 * concurrently. */
#if defined(__APPLE__)
#include <crt_externs.h>
#define CBM_ENVIRON (*_NSGetEnviron())
#elif !defined(_WIN32)
extern char **environ;
#define CBM_ENVIRON environ
#endif

const char *cbm_safe_getenv(const char *name, char *buf, size_t buf_sz, const char *fallback) {
    if (!buf || buf_sz == 0) {
        return NULL;
    }
    if (!name || !name[0]) {
        buf[0] = '\0';
        return NULL;
    }

#ifdef _WIN32
    wchar_t *wide_name = cbm_utf8_to_wide(name);
    if (!wide_name) {
        buf[0] = '\0';
        return NULL;
    }
    SetLastError(ERROR_SUCCESS);
    DWORD needed = GetEnvironmentVariableW(wide_name, NULL, 0);
    DWORD error = GetLastError();
    if (needed == 0 && error == ERROR_ENVVAR_NOT_FOUND) {
        free(wide_name);
        if (!fallback) {
            buf[0] = '\0';
            return NULL;
        }
        size_t fallback_len = strlen(fallback);
        if (fallback_len >= buf_sz) {
            buf[0] = '\0';
            return NULL;
        }
        memmove(buf, fallback, fallback_len + 1U);
        return buf;
    }
    if (needed == 0 && error == ERROR_SUCCESS) {
        /* Present-but-empty is a valid environment value. Path resolvers treat
         * it as unset, matching POSIX getenv semantics. */
        buf[0] = '\0';
        free(wide_name);
        return buf;
    }
    if (needed == 0) {
        free(wide_name);
        buf[0] = '\0';
        return NULL;
    }
    wchar_t *wide_value = malloc((size_t)needed * sizeof(*wide_value));
    if (!wide_value) {
        free(wide_name);
        buf[0] = '\0';
        return NULL;
    }
    DWORD written = GetEnvironmentVariableW(wide_name, wide_value, needed);
    free(wide_name);
    if (written >= needed) {
        free(wide_value);
        buf[0] = '\0';
        return NULL;
    }
    char *value = cbm_wide_to_utf8(wide_value);
    free(wide_value);
    if (!value) {
        buf[0] = '\0';
        return NULL;
    }
    size_t value_len = strlen(value);
    if (value_len >= buf_sz) {
        free(value);
        buf[0] = '\0';
        return NULL;
    }
    memmove(buf, value, value_len + 1U);
    free(value);
    return buf;
#else
    const char *value = NULL;
    char **env = CBM_ENVIRON;
    if (env) {
        size_t nlen = strlen(name);
        for (; *env; env++) {
            if (strncmp(*env, name, nlen) == 0 && (*env)[nlen] == '=') {
                value = *env + nlen + SKIP_ONE;
                break;
            }
        }
    }
    if (!value) {
        value = fallback;
    }
    if (!value) {
        buf[0] = '\0';
        return NULL;
    }

    size_t value_len = strlen(value);
    if (value_len >= buf_sz) {
        buf[0] = '\0';
        return NULL;
    }
    memmove(buf, value, value_len + 1);
    return buf;
#endif
}

/* ── Home directory (cross-platform) ───────────────────── */

static const char *cbm_path_with_suffix(char *buf, size_t buf_sz, const char *base,
                                        const char *suffix) {
    if (!buf || buf_sz == 0 || !base || !suffix) {
        if (buf && buf_sz > 0) {
            buf[0] = '\0';
        }
        return NULL;
    }
    int written = snprintf(buf, buf_sz, "%s%s", base, suffix);
    if (written < 0 || (size_t)written >= buf_sz) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}

const char *cbm_get_home_dir(void) {
    static CBM_TLS char buf[CBM_SZ_1K];

    if (!cbm_safe_getenv("HOME", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
#ifdef _WIN32
        cbm_normalize_path_sep(buf);
#endif
        return buf;
    }

    if (!cbm_safe_getenv("USERPROFILE", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
#ifdef _WIN32
        cbm_normalize_path_sep(buf);
#endif
        return buf;
    }
    return NULL;
}

/* ── App config directories (cross-platform) ────────── */

const char *cbm_app_config_dir(void) {
    static CBM_TLS char buf[CBM_SZ_1K];
#ifdef _WIN32
    if (!cbm_safe_getenv("APPDATA", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
        cbm_normalize_path_sep(buf);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    return cbm_path_with_suffix(buf, sizeof(buf), home, "/AppData/Roaming");
#else
    /* macOS/Linux: XDG_CONFIG_HOME or ~/.config */
    if (!cbm_safe_getenv("XDG_CONFIG_HOME", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    return cbm_path_with_suffix(buf, sizeof(buf), home, "/.config");
#endif /* _WIN32 */
}

const char *cbm_app_local_dir(void) {
    static CBM_TLS char buf[CBM_SZ_1K];
#ifdef _WIN32
    if (!cbm_safe_getenv("LOCALAPPDATA", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
        cbm_normalize_path_sep(buf);
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    return cbm_path_with_suffix(buf, sizeof(buf), home, "/AppData/Local");
#else
    const char *config = cbm_app_config_dir();
    if (!config) {
        buf[0] = '\0';
        return NULL;
    }
    return cbm_path_with_suffix(buf, sizeof(buf), config, "");
#endif
}

/* ── Cache directory ────────────────────────── */

const char *cbm_resolve_cache_dir(void) {
    static CBM_TLS char buf[CBM_SZ_1K];
    if (!cbm_safe_getenv("CBM_CACHE_DIR", buf, sizeof(buf), "")) {
        return NULL;
    }
    if (buf[0]) {
#ifdef _WIN32
        cbm_normalize_path_sep(buf);
#endif
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    return cbm_path_with_suffix(buf, sizeof(buf), home, "/.cache/codebase-memory-mcp");
}
