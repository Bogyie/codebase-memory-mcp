#include "foundation/rooted_file.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foundation/win_utf8.h"
#include <windows.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static cbm_rooted_file_test_hook_fn rooted_file_test_hook;
static void *rooted_file_test_hook_context;

void cbm_rooted_file_set_test_hook(cbm_rooted_file_test_hook_fn hook, void *context) {
    rooted_file_test_hook = hook;
    rooted_file_test_hook_context = context;
}

static void rooted_file_run_test_hook(void) {
    cbm_rooted_file_test_hook_fn hook = rooted_file_test_hook;
    void *context = rooted_file_test_hook_context;
    /* One-shot by construction: a failing assertion or early return in a test
     * cannot leak race injection into unrelated reads. */
    rooted_file_test_hook = NULL;
    rooted_file_test_hook_context = NULL;
    if (hook) {
        hook(context);
    }
}

void cbm_rooted_file_free(cbm_rooted_file_t *file) {
    if (!file) {
        return;
    }
    free(file->data);
    memset(file, 0, sizeof(*file));
}

bool cbm_rooted_relative_path_valid(const char *relative_path) {
    if (!relative_path || relative_path[0] == '\0' || relative_path[0] == '/' ||
        relative_path[0] == '\\') {
        return false;
    }
#ifdef _WIN32
    /* Colons enable drive paths and NTFS alternate data streams. Neither is a
     * repository-relative source path. */
    if (strchr(relative_path, ':')) {
        return false;
    }
#endif
    const char *component = relative_path;
    for (const char *p = relative_path;; p++) {
        bool separator = *p == '/';
#ifdef _WIN32
        separator = separator || *p == '\\';
#endif
        if (*p == '\0' || separator) {
            size_t len = (size_t)(p - component);
            if (len == 0 || (len == 1 && component[0] == '.') ||
                (len == 2 && component[0] == '.' && component[1] == '.')) {
                return false;
            }
            if (*p == '\0') {
                return true;
            }
            component = p + 1;
        }
    }
}

static void rooted_digest_hex(cbm_sha256_ctx *ctx, char out[CBM_SHA256_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(ctx, digest);
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

#ifdef _WIN32

static cbm_rooted_file_status_t rooted_windows_error_status(DWORD error) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_INVALID_NAME || error == ERROR_BAD_PATHNAME) {
        return CBM_ROOTED_FILE_NOT_FOUND;
    }
    return CBM_ROOTED_FILE_UNAVAILABLE;
}

static bool rooted_windows_info_same(const BY_HANDLE_FILE_INFORMATION *left,
                                     const BY_HANDLE_FILE_INFORMATION *right) {
    return left->dwVolumeSerialNumber == right->dwVolumeSerialNumber &&
           left->nFileIndexHigh == right->nFileIndexHigh &&
           left->nFileIndexLow == right->nFileIndexLow &&
           left->nFileSizeHigh == right->nFileSizeHigh &&
           left->nFileSizeLow == right->nFileSizeLow &&
           left->ftLastWriteTime.dwHighDateTime == right->ftLastWriteTime.dwHighDateTime &&
           left->ftLastWriteTime.dwLowDateTime == right->ftLastWriteTime.dwLowDateTime &&
           left->dwFileAttributes == right->dwFileAttributes;
}

static bool rooted_windows_mtime_ns(const FILETIME *mtime, int64_t *out) {
    uint64_t ticks = ((uint64_t)mtime->dwHighDateTime << 32U) | mtime->dwLowDateTime;
    const uint64_t windows_to_unix_epoch_100ns = 116444736000000000ULL;
    if (ticks < windows_to_unix_epoch_100ns) {
        *out = 0;
        return true;
    }
    uint64_t unix_ticks = ticks - windows_to_unix_epoch_100ns;
    uint64_t seconds = unix_ticks / 10000000ULL;
    if (seconds > (uint64_t)INT64_MAX / 1000000000ULL) {
        return false;
    }
    /* MinGW's stat() exposes second-resolution mtimes. File versions stored
     * during indexing use that representation, so expose the same precision
     * here. Generation checks above still compare the full FILETIME value. */
    *out = (int64_t)(seconds * 1000000000ULL);
    return true;
}

static wchar_t *rooted_windows_final_path(HANDLE handle, bool *out_of_memory) {
    if (out_of_memory) {
        *out_of_memory = false;
    }
    DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD needed = GetFinalPathNameByHandleW(handle, NULL, 0, flags);
    if (needed == 0 || needed >= (DWORD)(SIZE_MAX / sizeof(wchar_t)) - 1U) {
        return NULL;
    }
    size_t capacity = (size_t)needed + 1U;
    wchar_t *path = (wchar_t *)malloc(capacity * sizeof(wchar_t));
    if (!path) {
        if (out_of_memory) {
            *out_of_memory = true;
        }
        return NULL;
    }
    DWORD length = GetFinalPathNameByHandleW(handle, path, (DWORD)capacity, flags);
    if (length == 0 || (size_t)length >= capacity) {
        free(path);
        return NULL;
    }
    path[length] = L'\0';
    for (DWORD i = 0; i < length; i++) {
        if (path[i] == L'/') {
            path[i] = L'\\';
        }
    }
    return path;
}

static bool rooted_windows_path_within(const wchar_t *root, const wchar_t *file) {
    if (!root || !file) {
        return false;
    }
    size_t root_len = wcslen(root);
    size_t file_len = wcslen(file);
    if (root_len == 0 || file_len < root_len || _wcsnicmp(root, file, root_len) != 0) {
        return false;
    }
    bool root_has_separator = root[root_len - 1] == L'\\' || root[root_len - 1] == L'/';
    return file_len == root_len || root_has_separator || file[root_len] == L'\\' ||
           file[root_len] == L'/';
}

static wchar_t *rooted_windows_join(const char *root_path, const char *relative_path) {
    size_t root_len = strlen(root_path);
    size_t relative_len = strlen(relative_path);
    if (root_len > SIZE_MAX - relative_len - 2U) {
        return NULL;
    }
    size_t joined_len = root_len + relative_len + 2U;
    char *joined = (char *)malloc(joined_len);
    if (!joined) {
        return NULL;
    }
    bool separator =
        root_len > 0 && (root_path[root_len - 1] == '/' || root_path[root_len - 1] == '\\');
    int written =
        snprintf(joined, joined_len, separator ? "%s%s" : "%s/%s", root_path, relative_path);
    if (written < 0 || (size_t)written >= joined_len) {
        free(joined);
        return NULL;
    }
    wchar_t *wide = cbm_utf8_to_wide(joined);
    free(joined);
    return wide;
}

static cbm_rooted_file_status_t rooted_windows_read(const char *root_path,
                                                    const char *relative_path, size_t max_bytes,
                                                    cbm_rooted_file_t *out) {
    wchar_t *wide_root = cbm_utf8_to_wide(root_path);
    if (!wide_root) {
        return CBM_ROOTED_FILE_INVALID;
    }
    HANDLE root = CreateFileW(wide_root, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide_root);
    if (root == INVALID_HANDLE_VALUE) {
        return rooted_windows_error_status(GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION root_info;
    wchar_t *root_final = NULL;
    bool path_oom = false;
    if (GetFileType(root) != FILE_TYPE_DISK || !GetFileInformationByHandle(root, &root_info) ||
        !(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !(root_final = rooted_windows_final_path(root, &path_oom))) {
        CloseHandle(root);
        free(root_final);
        return path_oom ? CBM_ROOTED_FILE_OUT_OF_MEMORY : CBM_ROOTED_FILE_UNAVAILABLE;
    }

    wchar_t *wide_file = rooted_windows_join(root_path, relative_path);
    if (!wide_file) {
        free(root_final);
        CloseHandle(root);
        return CBM_ROOTED_FILE_OUT_OF_MEMORY;
    }
    HANDLE file = CreateFileW(
        wide_file, GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        cbm_rooted_file_status_t status = rooted_windows_error_status(GetLastError());
        free(wide_file);
        free(root_final);
        CloseHandle(root);
        return status;
    }

    cbm_rooted_file_status_t status = CBM_ROOTED_FILE_UNAVAILABLE;
    BY_HANDLE_FILE_INFORMATION before;
    wchar_t *file_final_before = NULL;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &before) ||
        (before.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
        !(file_final_before = rooted_windows_final_path(file, &path_oom)) ||
        !rooted_windows_path_within(root_final, file_final_before)) {
        if (path_oom) {
            status = CBM_ROOTED_FILE_OUT_OF_MEMORY;
        }
        goto cleanup;
    }

    uint64_t size64 = ((uint64_t)before.nFileSizeHigh << 32U) | before.nFileSizeLow;
    int64_t mtime_ns = 0;
    out->metadata_valid =
        size64 <= INT64_MAX && rooted_windows_mtime_ns(&before.ftLastWriteTime, &mtime_ns);
    out->size = out->metadata_valid ? (int64_t)size64 : -1;
    out->mtime_ns = out->metadata_valid ? mtime_ns : 0;
    if (!out->metadata_valid) {
        goto cleanup;
    }

    bool too_large = size64 > max_bytes || size64 > SIZE_MAX - 1U;
    rooted_file_run_test_hook();

    char *data = NULL;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    if (!too_large) {
        size_t expected = (size_t)size64;
        data = (char *)malloc(expected + 1U);
        if (!data) {
            status = CBM_ROOTED_FILE_OUT_OF_MEMORY;
            goto cleanup;
        }
        size_t total = 0;
        while (total < expected) {
            DWORD wanted = (DWORD)((expected - total) > 65536U ? 65536U : (expected - total));
            DWORD got = 0;
            if (!ReadFile(file, data + total, wanted, &got, NULL) || got == 0) {
                status = CBM_ROOTED_FILE_CHANGED;
                goto cleanup_data;
            }
            cbm_sha256_update(&hash, data + total, got);
            total += got;
        }
        unsigned char extra;
        DWORD extra_count = 0;
        if (!ReadFile(file, &extra, 1, &extra_count, NULL) || extra_count != 0) {
            status = CBM_ROOTED_FILE_CHANGED;
            goto cleanup_data;
        }
        data[expected] = '\0';
    }

    BY_HANDLE_FILE_INFORMATION after;
    wchar_t *file_final_after = NULL;
    HANDLE live = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION live_info;
    wchar_t *live_final = NULL;
    if (!GetFileInformationByHandle(file, &after) || !rooted_windows_info_same(&before, &after) ||
        !(file_final_after = rooted_windows_final_path(file, &path_oom)) ||
        !rooted_windows_path_within(root_final, file_final_after) ||
        _wcsicmp(file_final_before, file_final_after) != 0) {
        free(file_final_after);
        status = path_oom ? CBM_ROOTED_FILE_OUT_OF_MEMORY : CBM_ROOTED_FILE_CHANGED;
        goto cleanup_data;
    }
    free(file_final_after);

    live = CreateFileW(wide_file, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (live == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(live, &live_info) ||
        (live_info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
        !rooted_windows_info_same(&after, &live_info) ||
        !(live_final = rooted_windows_final_path(live, &path_oom)) ||
        !rooted_windows_path_within(root_final, live_final)) {
        free(live_final);
        if (live != INVALID_HANDLE_VALUE) {
            CloseHandle(live);
        }
        status = path_oom ? CBM_ROOTED_FILE_OUT_OF_MEMORY : CBM_ROOTED_FILE_CHANGED;
        goto cleanup_data;
    }
    free(live_final);
    CloseHandle(live);

    if (too_large) {
        status = CBM_ROOTED_FILE_TOO_LARGE;
        goto cleanup;
    }
    rooted_digest_hex(&hash, out->sha256);
    out->data = data;
    out->len = (size_t)size64;
    status = CBM_ROOTED_FILE_OK;
    data = NULL;

cleanup_data:
    free(data);
cleanup:
    free(file_final_before);
    CloseHandle(file);
    free(wide_file);
    free(root_final);
    CloseHandle(root);
    return status;
}

#else

static bool rooted_stat_mtime_ns(const struct stat *st, int64_t *out) {
#ifdef __APPLE__
    intmax_t seconds = (intmax_t)st->st_mtimespec.tv_sec;
    long nanoseconds = st->st_mtimespec.tv_nsec;
#else
    intmax_t seconds = (intmax_t)st->st_mtim.tv_sec;
    long nanoseconds = st->st_mtim.tv_nsec;
#endif
    const int64_t ns_per_second = 1000000000LL;
    if (nanoseconds < 0 || nanoseconds >= ns_per_second || seconds > INT64_MAX / ns_per_second ||
        seconds < INT64_MIN / ns_per_second) {
        return false;
    }
    int64_t base = (int64_t)seconds * ns_per_second;
    if (base > INT64_MAX - nanoseconds) {
        return false;
    }
    *out = base + nanoseconds;
    return true;
}

static bool rooted_stat_identity_equal(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static bool rooted_stat_generation_equal(const struct stat *left, const struct stat *right) {
    if (!rooted_stat_identity_equal(left, right) || left->st_size != right->st_size ||
        left->st_mtime != right->st_mtime || left->st_ctime != right->st_ctime) {
        return false;
    }
#ifdef __APPLE__
    return left->st_mtimespec.tv_nsec == right->st_mtimespec.tv_nsec &&
           left->st_ctimespec.tv_nsec == right->st_ctimespec.tv_nsec;
#else
    return left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
           left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
#endif
}

static int rooted_posix_open_flags(bool directory) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_NOCTTY
    flags |= O_NOCTTY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if (directory) {
        flags |= O_DIRECTORY;
    }
#else
    (void)directory;
#endif
    return flags;
}

static cbm_rooted_file_status_t rooted_posix_missing_status(int error) {
    return error == ENOENT || error == ENOTDIR ? CBM_ROOTED_FILE_NOT_FOUND
                                               : CBM_ROOTED_FILE_UNAVAILABLE;
}

static cbm_rooted_file_status_t rooted_posix_read(const char *root_path, const char *relative_path,
                                                  size_t max_bytes, cbm_rooted_file_t *out) {
    size_t relative_len = strlen(relative_path);
    char *path = (char *)malloc(relative_len + 1U);
    if (!path) {
        return CBM_ROOTED_FILE_OUT_OF_MEMORY;
    }
    memcpy(path, relative_path, relative_len + 1U);

    int root_flags = O_RDONLY;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    root_flags |= O_DIRECTORY;
#endif
    int parent = open(root_path, root_flags);
    if (parent < 0) {
        cbm_rooted_file_status_t status = rooted_posix_missing_status(errno);
        free(path);
        return status;
    }
    struct stat root_stat;
    if (fstat(parent, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        close(parent);
        free(path);
        return CBM_ROOTED_FILE_UNAVAILABLE;
    }

    char *final_component = strrchr(path, '/');
    if (final_component) {
        *final_component++ = '\0';
        char *component = path;
        while (*component) {
            char *separator = strchr(component, '/');
            if (separator) {
                *separator = '\0';
            }
            struct stat named;
            if (fstatat(parent, component, &named, AT_SYMLINK_NOFOLLOW) != 0) {
                cbm_rooted_file_status_t status = rooted_posix_missing_status(errno);
                close(parent);
                free(path);
                return status;
            }
            if (S_ISLNK(named.st_mode) || !S_ISDIR(named.st_mode)) {
                close(parent);
                free(path);
                return CBM_ROOTED_FILE_UNAVAILABLE;
            }
            int child = openat(parent, component, rooted_posix_open_flags(true));
            if (child < 0) {
                cbm_rooted_file_status_t status = rooted_posix_missing_status(errno);
                close(parent);
                free(path);
                return status;
            }
            struct stat opened;
            if (fstat(child, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
                !rooted_stat_identity_equal(&named, &opened)) {
                close(child);
                close(parent);
                free(path);
                return CBM_ROOTED_FILE_CHANGED;
            }
            close(parent);
            parent = child;
            if (!separator) {
                break;
            }
            component = separator + 1;
        }
    } else {
        final_component = path;
    }

    cbm_rooted_file_status_t status = CBM_ROOTED_FILE_UNAVAILABLE;
    struct stat named_before;
    if (fstatat(parent, final_component, &named_before, AT_SYMLINK_NOFOLLOW) != 0) {
        status = rooted_posix_missing_status(errno);
        goto cleanup_parent;
    }
    if (S_ISLNK(named_before.st_mode) || !S_ISREG(named_before.st_mode)) {
        goto cleanup_parent;
    }
    int file = openat(parent, final_component, rooted_posix_open_flags(false));
    if (file < 0) {
        status = rooted_posix_missing_status(errno);
        goto cleanup_parent;
    }
    struct stat before;
    if (fstat(file, &before) != 0 || !S_ISREG(before.st_mode) ||
        !rooted_stat_identity_equal(&named_before, &before)) {
        status = CBM_ROOTED_FILE_CHANGED;
        goto cleanup_file;
    }

    if (before.st_size < 0 || (uintmax_t)before.st_size > INT64_MAX) {
        goto cleanup_file;
    }
    int64_t mtime_ns = 0;
    out->metadata_valid = rooted_stat_mtime_ns(&before, &mtime_ns);
    out->mtime_ns = out->metadata_valid ? mtime_ns : 0;
    out->size = (int64_t)before.st_size;
    if (!out->metadata_valid) {
        goto cleanup_file;
    }
    bool too_large =
        (uintmax_t)before.st_size > max_bytes || (uintmax_t)before.st_size > SIZE_MAX - 1U;

    rooted_file_run_test_hook();

    char *data = NULL;
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    if (!too_large) {
        size_t expected = (size_t)before.st_size;
        data = (char *)malloc(expected + 1U);
        if (!data) {
            status = CBM_ROOTED_FILE_OUT_OF_MEMORY;
            goto cleanup_file;
        }
        size_t total = 0;
        while (total < expected) {
            ssize_t count = read(file, data + total, expected - total);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                status = CBM_ROOTED_FILE_CHANGED;
                goto cleanup_data;
            }
            cbm_sha256_update(&hash, data + total, (size_t)count);
            total += (size_t)count;
        }
        unsigned char extra;
        ssize_t extra_count;
        do {
            extra_count = read(file, &extra, 1);
        } while (extra_count < 0 && errno == EINTR);
        if (extra_count != 0) {
            status = CBM_ROOTED_FILE_CHANGED;
            goto cleanup_data;
        }
        data[expected] = '\0';
    }

    struct stat after;
    struct stat named_after;
    if (fstat(file, &after) != 0 || !rooted_stat_generation_equal(&before, &after) ||
        fstatat(parent, final_component, &named_after, AT_SYMLINK_NOFOLLOW) != 0 ||
        !rooted_stat_generation_equal(&after, &named_after)) {
        status = CBM_ROOTED_FILE_CHANGED;
        goto cleanup_data;
    }
    if (too_large) {
        status = CBM_ROOTED_FILE_TOO_LARGE;
        goto cleanup_file;
    }
    rooted_digest_hex(&hash, out->sha256);
    out->data = data;
    out->len = (size_t)before.st_size;
    data = NULL;
    status = CBM_ROOTED_FILE_OK;

cleanup_data:
    free(data);
cleanup_file:
    if (close(file) != 0 && status == CBM_ROOTED_FILE_OK) {
        cbm_rooted_file_free(out);
        status = CBM_ROOTED_FILE_UNAVAILABLE;
    }
cleanup_parent:
    close(parent);
    free(path);
    return status;
}

#endif

cbm_rooted_file_status_t cbm_rooted_file_read(const char *root_path, const char *relative_path,
                                              size_t max_bytes, cbm_rooted_file_t *out) {
    if (!out) {
        return CBM_ROOTED_FILE_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->size = -1;
    if (!root_path || root_path[0] == '\0' || max_bytes == 0 ||
        !cbm_rooted_relative_path_valid(relative_path)) {
        return CBM_ROOTED_FILE_INVALID;
    }
#ifdef _WIN32
    return rooted_windows_read(root_path, relative_path, max_bytes, out);
#else
    return rooted_posix_read(root_path, relative_path, max_bytes, out);
#endif
}
