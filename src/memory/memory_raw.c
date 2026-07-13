/*
 * memory_raw.c — Local-file ingest authority and immutable raw-object staging.
 */

#include "memory/memory_raw.h"

#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#include "foundation/rooted_file.h"
#include "foundation/sha256.h"

#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    CBM_MEMORY_RAW_PATH_MAX = 4096,
    CBM_MEMORY_RAW_INGEST_ROOTS_MAX = CBM_MEMORY_RAW_PATH_MAX * 4,
};

#ifdef _WIN32
#include "foundation/win_utf8.h"

#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#endif

struct cbm_memory_raw_stage {
    char home[CBM_MEMORY_RAW_PATH_MAX];
    char root[CBM_MEMORY_RAW_PATH_MAX];
    char staged[CBM_MEMORY_RAW_PATH_MAX];
    char target[CBM_MEMORY_RAW_PATH_MAX];
    char target_parent[CBM_MEMORY_RAW_PATH_MAX];
    bool staged_exists;
    bool lease_exists;
    bool target_parent_created;
#ifndef _WIN32
    int home_fd;
    int raw_root_fd;
    int staging_fd;
    int staged_fd;
    int target_parent_fd;
    int target_lease_fd;
    char staged_name[CBM_MEMORY_RAW_PATH_MAX];
    char lease_name[CBM_MEMORY_RAW_PATH_MAX];
    char target_parent_name[CBM_MEMORY_RAW_PATH_MAX];
    char target_name[CBM_MEMORY_RAW_PATH_MAX];
#endif
};

static int memory_raw_create_directory_no_replace(const char *path);
#ifdef _WIN32
static bool memory_raw_final_path_from_handle(HANDLE handle, char out[CBM_MEMORY_RAW_PATH_MAX]);
#endif

static void memory_raw_normalize_native_path(char *path) {
#ifdef _WIN32
    (void)cbm_normalize_path_sep(path);
#else
    (void)path;
#endif
}

static bool memory_raw_env_enabled(const char *name) {
    char value[16] = "";
    if (!cbm_safe_getenv(name, value, sizeof(value), NULL)) {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0;
}

static bool memory_raw_path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
#ifdef _WIN32
    return ((((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
            ((path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\')));
#else
    return path[0] == '/';
#endif
}

static bool memory_raw_plain_directory_exists(const char *path) {
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(wide);
    free(wide);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool memory_raw_path_exists_nofollow(const char *path) {
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(wide);
    free(wide);
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

static bool memory_raw_parent_dir(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0 || snprintf(out, out_size, "%s", path) >= (int)out_size) {
        return false;
    }
    memory_raw_normalize_native_path(out);
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        return false;
    }
    *slash = '\0';
    return true;
}

static bool memory_raw_path_equal(const char *left, const char *right) {
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

static bool memory_raw_path_join(char *out, size_t out_size, const char *left, const char *right) {
    if (!out || out_size == 0 || !left || !right) {
        return false;
    }
    size_t left_len = strlen(left);
    bool has_separator = left_len > 0 && left[left_len - 1] == '/';
#ifdef _WIN32
    has_separator = has_separator || (left_len > 0 && left[left_len - 1] == '\\');
#endif
    bool needs_separator = left_len > 0 && !has_separator;
    int n = snprintf(out, out_size, needs_separator ? "%s/%s" : "%s%s", left, right);
    if (n < 0 || (size_t)n >= out_size) {
        return false;
    }
    memory_raw_normalize_native_path(out);
    return true;
}

static bool memory_raw_canonical_within_root(const char *canonical_root,
                                             const char *canonical_path) {
    if (!canonical_root || !canonical_path) {
        return false;
    }
    size_t root_len = strlen(canonical_root);
    size_t path_len = strlen(canonical_path);
    if (root_len == 0 || path_len < root_len) {
        return false;
    }
#ifdef _WIN32
    bool prefix = _strnicmp(canonical_root, canonical_path, root_len) == 0;
#else
    bool prefix = strncmp(canonical_root, canonical_path, root_len) == 0;
#endif
    if (!prefix) {
        return false;
    }
#ifdef _WIN32
    if (canonical_root[root_len - 1] == '/' || canonical_root[root_len - 1] == '\\') {
        return true;
    }
    return canonical_path[root_len] == '/' || canonical_path[root_len] == '\\' ||
           canonical_path[root_len] == '\0';
#else
    if (canonical_root[root_len - 1] == '/') {
        return true;
    }
    return canonical_path[root_len] == '/' || canonical_path[root_len] == '\0';
#endif
}

int cbm_memory_raw_resolve_directory(const char *path, char *out_path, size_t out_path_size) {
    if (!path || !out_path || out_path_size == 0) {
        return -1;
    }
    char resolved[CBM_MEMORY_RAW_PATH_MAX];
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return -1;
    }
    HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    BY_HANDLE_FILE_INFORMATION info;
    bool valid = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) &&
                 (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                 memory_raw_final_path_from_handle(handle, resolved);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    if (!valid) {
        return -1;
    }
#else
    struct stat st;
    if (!cbm_canonical_path(path, resolved, sizeof(resolved)) || stat(resolved, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
        return -1;
    }
#endif
    memory_raw_normalize_native_path(resolved);
    int n = snprintf(out_path, out_path_size, "%s", resolved);
    return n >= 0 && (size_t)n < out_path_size ? 0 : -1;
}

static bool memory_raw_canonical_home(const char *home, char out[CBM_MEMORY_RAW_PATH_MAX]) {
    return home && cbm_memory_raw_resolve_directory(home, out, CBM_MEMORY_RAW_PATH_MAX) == 0;
}

static int memory_raw_prepare_private_subdir(const char *home, const char *name, char *out_path,
                                             size_t out_path_size, bool require_new) {
    if (!home || !name || !name[0] || strchr(name, '/') || strchr(name, '\\') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !out_path || out_path_size == 0) {
        return -1;
    }
    char canonical_home[CBM_MEMORY_RAW_PATH_MAX];
    char path[CBM_MEMORY_RAW_PATH_MAX];
    if (!memory_raw_canonical_home(home, canonical_home) ||
        !memory_raw_path_join(path, sizeof(path), home, name)) {
        return -1;
    }
    int directory_rc = memory_raw_create_directory_no_replace(path);
    bool created = directory_rc == 0;
    char canonical_path[CBM_MEMORY_RAW_PATH_MAX] = "";
    bool valid =
        directory_rc >= 0 && (!require_new || created) && memory_raw_plain_directory_exists(path) &&
        cbm_memory_raw_resolve_directory(path, canonical_path, sizeof(canonical_path)) == 0;
    if (valid) {
        memory_raw_normalize_native_path(canonical_path);
        valid = !memory_raw_path_equal(canonical_home, canonical_path) &&
                memory_raw_canonical_within_root(canonical_home, canonical_path);
    }
#ifndef _WIN32
    if (valid && chmod(canonical_path, 0700) != 0) {
        valid = false;
    }
#endif
    if (!valid || snprintf(out_path, out_path_size, "%s", canonical_path) >= (int)out_path_size) {
        if (created && canonical_path[0]) {
            (void)cbm_rmdir(canonical_path);
        }
        return -1;
    }
    return 0;
}

int cbm_memory_raw_ensure_private_subdir(const char *home, const char *name, char *out_path,
                                         size_t out_path_size) {
    return memory_raw_prepare_private_subdir(home, name, out_path, out_path_size, false);
}

int cbm_memory_raw_create_private_subdir(const char *home, const char *name, char *out_path,
                                         size_t out_path_size) {
    return memory_raw_prepare_private_subdir(home, name, out_path, out_path_size, true);
}

static bool memory_raw_component_name(const char *path, char out[CBM_MEMORY_RAW_PATH_MAX]) {
    char normalized[CBM_MEMORY_RAW_PATH_MAX];
    if (!path || snprintf(normalized, sizeof(normalized), "%s", path) >= (int)sizeof(normalized)) {
        return false;
    }
    memory_raw_normalize_native_path(normalized);
    const char *name = strrchr(normalized, '/');
    name = name ? name + 1 : normalized;
    int n = snprintf(out, CBM_MEMORY_RAW_PATH_MAX, "%s", name);
    return name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && n >= 0 &&
           n < CBM_MEMORY_RAW_PATH_MAX;
}

static int memory_raw_validate_object_scope(const char *home, const char *target,
                                            const char *target_parent, bool create_parent,
                                            bool *out_created, char *out_target,
                                            size_t out_target_size, char *out_parent,
                                            size_t out_parent_size) {
    if (out_created) {
        *out_created = false;
    }
    if (!home || !target || !target_parent || !out_target || out_target_size == 0 || !out_parent ||
        out_parent_size == 0) {
        return -1;
    }
    char canonical_home[CBM_MEMORY_RAW_PATH_MAX];
    char raw_root[CBM_MEMORY_RAW_PATH_MAX];
    char canonical_root[CBM_MEMORY_RAW_PATH_MAX];
    char target_dir[CBM_MEMORY_RAW_PATH_MAX];
    char parent_of_target_parent[CBM_MEMORY_RAW_PATH_MAX];
    char canonical_parent_base[CBM_MEMORY_RAW_PATH_MAX];
    char parent_name[CBM_MEMORY_RAW_PATH_MAX];
    char target_name[CBM_MEMORY_RAW_PATH_MAX];
    if (!memory_raw_canonical_home(home, canonical_home) ||
        !memory_raw_path_join(raw_root, sizeof(raw_root), home, "raw/objects") ||
        !memory_raw_plain_directory_exists(raw_root) ||
        cbm_memory_raw_resolve_directory(raw_root, canonical_root, sizeof(canonical_root)) != 0 ||
        !memory_raw_parent_dir(target, target_dir, sizeof(target_dir)) ||
        !memory_raw_parent_dir(target_parent, parent_of_target_parent,
                               sizeof(parent_of_target_parent)) ||
        cbm_memory_raw_resolve_directory(parent_of_target_parent, canonical_parent_base,
                                         sizeof(canonical_parent_base)) != 0 ||
        !memory_raw_component_name(target_parent, parent_name) ||
        !memory_raw_component_name(target, target_name)) {
        return -1;
    }
    memory_raw_normalize_native_path(canonical_root);
    memory_raw_normalize_native_path(canonical_parent_base);
    memory_raw_normalize_native_path(target_dir);
    char normalized_target_parent[CBM_MEMORY_RAW_PATH_MAX];
    if (snprintf(normalized_target_parent, sizeof(normalized_target_parent), "%s", target_parent) >=
        (int)sizeof(normalized_target_parent)) {
        return -1;
    }
    memory_raw_normalize_native_path(normalized_target_parent);
    if (!memory_raw_canonical_within_root(canonical_home, canonical_root) ||
        !memory_raw_path_equal(canonical_root, canonical_parent_base) ||
        !memory_raw_path_equal(target_dir, normalized_target_parent)) {
        return -1;
    }

    char canonical_parent[CBM_MEMORY_RAW_PATH_MAX];
    char canonical_target[CBM_MEMORY_RAW_PATH_MAX];
    if (!memory_raw_path_join(canonical_parent, sizeof(canonical_parent), canonical_root,
                              parent_name) ||
        !memory_raw_path_join(canonical_target, sizeof(canonical_target), canonical_parent,
                              target_name)) {
        return -1;
    }

    bool original_exists = memory_raw_path_exists_nofollow(target_parent);
    if (original_exists) {
        char resolved_original[CBM_MEMORY_RAW_PATH_MAX];
        if (!memory_raw_plain_directory_exists(target_parent) ||
            cbm_memory_raw_resolve_directory(target_parent, resolved_original,
                                             sizeof(resolved_original)) != 0) {
            return -1;
        }
        memory_raw_normalize_native_path(resolved_original);
        if (!memory_raw_path_equal(resolved_original, canonical_parent)) {
            return -1;
        }
    }

    int directory_rc =
        original_exists
            ? 1
            : (create_parent ? memory_raw_create_directory_no_replace(canonical_parent) : 0);
    bool created = create_parent && !original_exists && directory_rc == 0;
    if (create_parent || original_exists) {
        char resolved_parent[CBM_MEMORY_RAW_PATH_MAX];
        bool valid = directory_rc >= 0 && memory_raw_plain_directory_exists(canonical_parent) &&
                     cbm_memory_raw_resolve_directory(canonical_parent, resolved_parent,
                                                      sizeof(resolved_parent)) == 0;
        if (valid) {
            memory_raw_normalize_native_path(resolved_parent);
            valid = memory_raw_path_equal(resolved_parent, canonical_parent) &&
                    memory_raw_canonical_within_root(canonical_home, resolved_parent) &&
                    memory_raw_canonical_within_root(canonical_root, resolved_parent) &&
                    !memory_raw_path_equal(canonical_root, resolved_parent);
        }
#ifndef _WIN32
        if (valid && chmod(canonical_parent, 0700) != 0) {
            valid = false;
        }
#endif
        if (!valid) {
            if (created) {
                (void)cbm_rmdir(canonical_parent);
            }
            return -1;
        }
    }
    if (snprintf(out_target, out_target_size, "%s", canonical_target) >= (int)out_target_size ||
        snprintf(out_parent, out_parent_size, "%s", canonical_parent) >= (int)out_parent_size) {
        if (created) {
            (void)cbm_rmdir(canonical_parent);
        }
        return -1;
    }
    if (out_created) {
        *out_created = created;
    }
    return 0;
}

int cbm_memory_raw_ensure_object_parent(const char *home, const char *target,
                                        const char *target_parent, bool *out_created,
                                        char *out_target, size_t out_target_size, char *out_parent,
                                        size_t out_parent_size) {
    return memory_raw_validate_object_scope(home, target, target_parent, true, out_created,
                                            out_target, out_target_size, out_parent,
                                            out_parent_size);
}

int cbm_memory_raw_validate_object_parent(const char *home, const char *target,
                                          const char *target_parent, char *out_target,
                                          size_t out_target_size, char *out_parent,
                                          size_t out_parent_size) {
    return memory_raw_validate_object_scope(home, target, target_parent, false, NULL, out_target,
                                            out_target_size, out_parent, out_parent_size);
}

/* Authorize an already-resolved path. */
#ifdef _WIN32
static cbm_memory_raw_read_result_t memory_raw_authorize_canonical_path(const char *canonical) {
    if (memory_raw_env_enabled("CBM_MEMORY_ALLOW_UNSAFE_PATH_INGEST")) {
        return CBM_MEMORY_RAW_READ_OK;
    }

    char roots[CBM_MEMORY_RAW_INGEST_ROOTS_MAX] = "";
    if (!cbm_safe_getenv("CBM_MEMORY_INGEST_ROOTS", roots, sizeof(roots), NULL) || !roots[0]) {
        return CBM_MEMORY_RAW_READ_AUTHORITY_REQUIRED;
    }
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const char *cursor = roots;
    while (*cursor) {
        const char *end = strchr(cursor, separator);
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length > 0 && length < CBM_MEMORY_RAW_PATH_MAX) {
            char root[CBM_MEMORY_RAW_PATH_MAX];
            memcpy(root, cursor, length);
            root[length] = '\0';
            if (memory_raw_path_is_absolute(root)) {
                char canonical_root[CBM_MEMORY_RAW_PATH_MAX];
                if (cbm_memory_raw_resolve_directory(root, canonical_root,
                                                     sizeof(canonical_root)) == 0) {
                    memory_raw_normalize_native_path(canonical_root);
                    if (memory_raw_canonical_within_root(canonical_root, canonical)) {
                        return CBM_MEMORY_RAW_READ_OK;
                    }
                }
            }
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return CBM_MEMORY_RAW_READ_DENIED;
}
#endif

#ifdef _WIN32
static bool memory_raw_final_path_from_handle(HANDLE handle, char out[CBM_MEMORY_RAW_PATH_MAX]) {
    wchar_t wide[CBM_MEMORY_RAW_PATH_MAX];
    DWORD length =
        GetFinalPathNameByHandleW(handle, wide, CBM_MEMORY_RAW_PATH_MAX, FILE_NAME_NORMALIZED);
    if (length == 0 || length >= CBM_MEMORY_RAW_PATH_MAX) {
        return false;
    }
    const wchar_t *source = wide;
    wchar_t normalized[CBM_MEMORY_RAW_PATH_MAX];
    if (wcsncmp(wide, L"\\\\?\\UNC\\", 8) == 0) {
        int n = swprintf(normalized, CBM_MEMORY_RAW_PATH_MAX, L"\\\\%ls", wide + 8);
        if (n < 0 || n >= CBM_MEMORY_RAW_PATH_MAX) {
            return false;
        }
        source = normalized;
    } else if (wcsncmp(wide, L"\\\\?\\", 4) == 0) {
        source = wide + 4;
    }
    char *utf8 = cbm_wide_to_utf8(source);
    if (!utf8) {
        return false;
    }
    int n = snprintf(out, CBM_MEMORY_RAW_PATH_MAX, "%s", utf8);
    free(utf8);
    if (n < 0 || n >= CBM_MEMORY_RAW_PATH_MAX) {
        return false;
    }
    memory_raw_normalize_native_path(out);
    return true;
}

static bool memory_raw_windows_at_eof(HANDLE handle) {
    unsigned char extra = 0;
    DWORD extra_read = 0;
    BOOL read_ok = ReadFile(handle, &extra, 1, &extra_read, NULL);
    return (read_ok || GetLastError() == ERROR_HANDLE_EOF) && extra_read == 0;
}
#endif

#ifndef _WIN32
static bool memory_raw_stat_snapshot_matches(const struct stat *before, const struct stat *after) {
    if (!before || !after || before->st_dev != after->st_dev || before->st_ino != after->st_ino ||
        before->st_size != after->st_size) {
        return false;
    }
#ifdef __APPLE__
    return before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
           before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
#else
    return before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
#endif
}

static int memory_raw_posix_directory_flags(void) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

static int memory_raw_open_directory_path(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    int fd = open(path, memory_raw_posix_directory_flags());
    if (fd < 0) {
        return -1;
    }
    struct stat opened;
    struct stat current;
    if (fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode) || lstat(path, &current) != 0 ||
        !S_ISDIR(current.st_mode) || opened.st_dev != current.st_dev ||
        opened.st_ino != current.st_ino) {
        close(fd);
        return -1;
    }
    return fd;
}

static int memory_raw_open_directory_at(int parent_fd, const char *name) {
    if (parent_fd < 0 || !name || !name[0] || strchr(name, '/')) {
        return -1;
    }
    int fd = openat(parent_fd, name, memory_raw_posix_directory_flags());
    if (fd < 0) {
        return -1;
    }
    struct stat opened;
    struct stat current;
    if (fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode) ||
        fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(current.st_mode) ||
        opened.st_dev != current.st_dev || opened.st_ino != current.st_ino) {
        close(fd);
        return -1;
    }
    return fd;
}

static int memory_raw_open_regular_at(int directory_fd, const char *name) {
    if (directory_fd < 0 || !name || !name[0] || strchr(name, '/')) {
        return -1;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    return openat(directory_fd, name, flags);
}

static bool memory_raw_fd_matches_regular_entry(int fd, int directory_fd, const char *name,
                                                struct stat *out_stat) {
    struct stat opened;
    struct stat entry;
    if (fd < 0 || directory_fd < 0 || !name || fstat(fd, &opened) != 0 ||
        !S_ISREG(opened.st_mode) || fstatat(directory_fd, name, &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(entry.st_mode) || opened.st_dev != entry.st_dev || opened.st_ino != entry.st_ino) {
        return false;
    }
    if (out_stat) {
        *out_stat = opened;
    }
    return true;
}

static int memory_raw_lock_regular_at(int directory_fd, const char *name, int operation,
                                      int *out_fd) {
    if (!out_fd) {
        return -1;
    }
    *out_fd = -1;
    int fd = memory_raw_open_regular_at(directory_fd, name);
    if (fd < 0 || !memory_raw_fd_matches_regular_entry(fd, directory_fd, name, NULL) ||
        flock(fd, operation | LOCK_NB) != 0 ||
        !memory_raw_fd_matches_regular_entry(fd, directory_fd, name, NULL)) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    *out_fd = fd;
    return 0;
}

int cbm_memory_raw_lock_regular_at(int directory_fd, const char *name, int *out_fd) {
    return memory_raw_lock_regular_at(directory_fd, name, LOCK_SH, out_fd);
}

int cbm_memory_raw_read_regular_at(int directory_fd, const char *name, size_t max_len,
                                   unsigned char **out_bytes, size_t *out_len) {
    if (!out_bytes || !out_len) {
        return -1;
    }
    *out_bytes = NULL;
    *out_len = 0;
    int fd = memory_raw_open_regular_at(directory_fd, name);
    if (fd < 0) {
        return -1;
    }
    struct stat before;
    struct stat path_before;
    bool valid = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
                 (uint64_t)before.st_size <= (uint64_t)max_len &&
                 (uint64_t)before.st_size < (uint64_t)SIZE_MAX &&
                 fstatat(directory_fd, name, &path_before, AT_SYMLINK_NOFOLLOW) == 0 &&
                 S_ISREG(path_before.st_mode) &&
                 memory_raw_stat_snapshot_matches(&before, &path_before);
    size_t len = valid ? (size_t)before.st_size : 0;
    unsigned char *bytes = valid ? malloc(len + 1U) : NULL;
    if (!bytes) {
        valid = false;
    }
    size_t offset = 0;
    while (valid && offset < len) {
        ssize_t got = read(fd, bytes + offset, len - offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            valid = false;
            break;
        }
        offset += (size_t)got;
    }
    unsigned char extra = 0;
    ssize_t extra_read = -1;
    if (valid) {
        do {
            extra_read = read(fd, &extra, 1);
        } while (extra_read < 0 && errno == EINTR);
    }
    struct stat after;
    struct stat path_after;
    if (valid &&
        (extra_read != 0 || fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
         !memory_raw_stat_snapshot_matches(&before, &after) ||
         fstatat(directory_fd, name, &path_after, AT_SYMLINK_NOFOLLOW) != 0 ||
         !S_ISREG(path_after.st_mode) || !memory_raw_stat_snapshot_matches(&before, &path_after))) {
        valid = false;
    }
    if (close(fd) != 0) {
        valid = false;
    }
    if (!valid) {
        free(bytes);
        return -1;
    }
    bytes[len] = 0;
    *out_bytes = bytes;
    *out_len = len;
    return 0;
}

typedef struct {
    int file_fd;
    int parent_fd;
    char name[CBM_MEMORY_RAW_PATH_MAX];
} memory_raw_authorized_open_t;

static void memory_raw_authorized_open_close(memory_raw_authorized_open_t *opened) {
    if (!opened) {
        return;
    }
    if (opened->file_fd >= 0) {
        close(opened->file_fd);
    }
    if (opened->parent_fd >= 0) {
        close(opened->parent_fd);
    }
    opened->file_fd = -1;
    opened->parent_fd = -1;
}

static cbm_memory_raw_read_result_t memory_raw_open_authorized_posix(
    const char *path, memory_raw_authorized_open_t *opened) {
    if (!path || !opened) {
        return CBM_MEMORY_RAW_READ_DENIED;
    }
    memset(opened, 0, sizeof(*opened));
    opened->file_fd = -1;
    opened->parent_fd = -1;
    char canonical_path[CBM_MEMORY_RAW_PATH_MAX];
    if (!cbm_canonical_path(path, canonical_path, sizeof(canonical_path))) {
        return CBM_MEMORY_RAW_READ_DENIED;
    }

    bool unsafe = memory_raw_env_enabled("CBM_MEMORY_ALLOW_UNSAFE_PATH_INGEST");
    char roots[CBM_MEMORY_RAW_INGEST_ROOTS_MAX] = "";
    if (!unsafe &&
        (!cbm_safe_getenv("CBM_MEMORY_INGEST_ROOTS", roots, sizeof(roots), NULL) || !roots[0])) {
        return CBM_MEMORY_RAW_READ_AUTHORITY_REQUIRED;
    }
    const char *cursor = unsafe ? "/" : roots;
    while (*cursor) {
        const char *end = unsafe ? NULL : strchr(cursor, ':');
        size_t root_length = unsafe ? 1U : (end ? (size_t)(end - cursor) : strlen(cursor));
        if (root_length > 0 && root_length < CBM_MEMORY_RAW_PATH_MAX) {
            char configured_root[CBM_MEMORY_RAW_PATH_MAX];
            memcpy(configured_root, cursor, root_length);
            configured_root[root_length] = '\0';
            char canonical_root[CBM_MEMORY_RAW_PATH_MAX];
            if (memory_raw_path_is_absolute(configured_root) &&
                cbm_memory_raw_resolve_directory(configured_root, canonical_root,
                                                 sizeof(canonical_root)) == 0 &&
                memory_raw_canonical_within_root(canonical_root, canonical_path)) {
                size_t canonical_root_length = strlen(canonical_root);
                const char *relative = canonical_path + canonical_root_length;
                while (*relative == '/') {
                    relative++;
                }
                if (*relative) {
                    char components[CBM_MEMORY_RAW_PATH_MAX];
                    int copied = snprintf(components, sizeof(components), "%s", relative);
                    int current_fd = memory_raw_open_directory_path(canonical_root);
                    if (copied >= 0 && copied < (int)sizeof(components) && current_fd >= 0) {
                        char *save = NULL;
                        char *component = strtok_r(components, "/", &save);
                        while (component) {
                            char *next = strtok_r(NULL, "/", &save);
                            if (!component[0] || strcmp(component, ".") == 0 ||
                                strcmp(component, "..") == 0) {
                                close(current_fd);
                                current_fd = -1;
                                break;
                            }
                            if (next) {
                                int child_fd = memory_raw_open_directory_at(current_fd, component);
                                close(current_fd);
                                current_fd = child_fd;
                                if (current_fd < 0) {
                                    break;
                                }
                            } else {
                                int file_fd = memory_raw_open_regular_at(current_fd, component);
                                if (file_fd >= 0 &&
                                    snprintf(opened->name, sizeof(opened->name), "%s", component) <
                                        (int)sizeof(opened->name)) {
                                    opened->file_fd = file_fd;
                                    opened->parent_fd = current_fd;
                                    return CBM_MEMORY_RAW_READ_OK;
                                }
                                if (file_fd >= 0) {
                                    close(file_fd);
                                }
                                close(current_fd);
                                current_fd = -1;
                            }
                            component = next;
                        }
                        if (current_fd >= 0) {
                            close(current_fd);
                        }
                    } else if (current_fd >= 0) {
                        close(current_fd);
                    }
                }
            }
        }
        if (unsafe || !end) {
            break;
        }
        cursor = end + 1;
    }
    return CBM_MEMORY_RAW_READ_DENIED;
}
#endif

cbm_memory_raw_read_result_t cbm_memory_raw_read_authorized_path(const char *path,
                                                                 unsigned char **out_bytes,
                                                                 size_t *out_len) {
    if (!out_bytes || !out_len) {
        return CBM_MEMORY_RAW_READ_FAILED;
    }
    *out_bytes = NULL;
    *out_len = 0;
    if (!path || !path[0]) {
        return CBM_MEMORY_RAW_READ_DENIED;
    }
#ifdef _WIN32
    if (!memory_raw_env_enabled("CBM_MEMORY_ALLOW_UNSAFE_PATH_INGEST")) {
        char roots[CBM_MEMORY_RAW_INGEST_ROOTS_MAX] = "";
        if (!cbm_safe_getenv("CBM_MEMORY_INGEST_ROOTS", roots, sizeof(roots), NULL) || !roots[0]) {
            return CBM_MEMORY_RAW_READ_AUTHORITY_REQUIRED;
        }
    }
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return CBM_MEMORY_RAW_READ_DENIED;
    }
    HANDLE handle =
        CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        free(wide);
        return CBM_MEMORY_RAW_READ_DENIED;
    }
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    char opened_path[CBM_MEMORY_RAW_PATH_MAX];
    bool valid = GetFileInformationByHandle(handle, &info) &&
                 !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                 !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                 GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
                 size.QuadPart <= CBM_MEMORY_RAW_MAX_SOURCE_BYTES &&
                 memory_raw_final_path_from_handle(handle, opened_path) &&
                 memory_raw_authorize_canonical_path(opened_path) == CBM_MEMORY_RAW_READ_OK;
    if (!valid) {
        CloseHandle(handle);
        free(wide);
        return CBM_MEMORY_RAW_READ_DENIED;
    }
    size_t len = (size_t)size.QuadPart;
    unsigned char *bytes = malloc(len + 1U);
    if (!bytes) {
        CloseHandle(handle);
        free(wide);
        return CBM_MEMORY_RAW_READ_FAILED;
    }
    size_t offset = 0;
    while (offset < len) {
        DWORD chunk = (DWORD)((len - offset) > UINT32_MAX ? UINT32_MAX : (len - offset));
        DWORD got = 0;
        if (!ReadFile(handle, bytes + offset, chunk, &got, NULL) || got == 0) {
            free(bytes);
            CloseHandle(handle);
            free(wide);
            return CBM_MEMORY_RAW_READ_FAILED;
        }
        offset += got;
    }
    BY_HANDLE_FILE_INFORMATION after;
    LARGE_INTEGER after_size;
    char after_path[CBM_MEMORY_RAW_PATH_MAX];
    valid = memory_raw_windows_at_eof(handle) && GetFileInformationByHandle(handle, &after) &&
            GetFileSizeEx(handle, &after_size) &&
            after.dwVolumeSerialNumber == info.dwVolumeSerialNumber &&
            after.nFileIndexHigh == info.nFileIndexHigh &&
            after.nFileIndexLow == info.nFileIndexLow && after_size.QuadPart == size.QuadPart &&
            CompareFileTime(&after.ftLastWriteTime, &info.ftLastWriteTime) == 0 &&
            memory_raw_final_path_from_handle(handle, after_path) &&
            memory_raw_authorize_canonical_path(after_path) == CBM_MEMORY_RAW_READ_OK;
    HANDLE current =
        CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION current_info;
    if (valid &&
        (current == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(current, &current_info) ||
         (current_info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
         current_info.dwVolumeSerialNumber != info.dwVolumeSerialNumber ||
         current_info.nFileIndexHigh != info.nFileIndexHigh ||
         current_info.nFileIndexLow != info.nFileIndexLow ||
         current_info.nFileSizeHigh != info.nFileSizeHigh ||
         current_info.nFileSizeLow != info.nFileSizeLow ||
         CompareFileTime(&current_info.ftLastWriteTime, &info.ftLastWriteTime) != 0)) {
        valid = false;
    }
    if (current != INVALID_HANDLE_VALUE) {
        CloseHandle(current);
    }
    CloseHandle(handle);
    free(wide);
    if (!valid) {
        free(bytes);
        return CBM_MEMORY_RAW_READ_FAILED;
    }
#else
    memory_raw_authorized_open_t opened = {.file_fd = -1, .parent_fd = -1};
    cbm_memory_raw_read_result_t authorization = memory_raw_open_authorized_posix(path, &opened);
    if (authorization != CBM_MEMORY_RAW_READ_OK) {
        return authorization;
    }
    int fd = opened.file_fd;
    struct stat opened_stat;
    struct stat path_before;
    struct stat requested_before;
    bool valid = fstat(fd, &opened_stat) == 0 && S_ISREG(opened_stat.st_mode) &&
                 opened_stat.st_size >= 0 &&
                 opened_stat.st_size <= CBM_MEMORY_RAW_MAX_SOURCE_BYTES &&
                 fstatat(opened.parent_fd, opened.name, &path_before, AT_SYMLINK_NOFOLLOW) == 0 &&
                 S_ISREG(path_before.st_mode) &&
                 memory_raw_stat_snapshot_matches(&opened_stat, &path_before) &&
                 lstat(path, &requested_before) == 0 && S_ISREG(requested_before.st_mode) &&
                 memory_raw_stat_snapshot_matches(&opened_stat, &requested_before);
    if (!valid) {
        memory_raw_authorized_open_close(&opened);
        return CBM_MEMORY_RAW_READ_DENIED;
    }
    size_t len = (size_t)opened_stat.st_size;
    unsigned char *bytes = malloc(len + 1U);
    if (!bytes) {
        memory_raw_authorized_open_close(&opened);
        return CBM_MEMORY_RAW_READ_FAILED;
    }
    size_t offset = 0;
    while (offset < len) {
        ssize_t got = read(fd, bytes + offset, len - offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            free(bytes);
            memory_raw_authorized_open_close(&opened);
            return CBM_MEMORY_RAW_READ_FAILED;
        }
        offset += (size_t)got;
    }
    unsigned char extra = 0;
    ssize_t extra_read;
    do {
        extra_read = read(fd, &extra, 1);
    } while (extra_read < 0 && errno == EINTR);
    struct stat after_stat;
    struct stat path_after;
    struct stat requested_after;
    valid = extra_read == 0 && fstat(fd, &after_stat) == 0 &&
            memory_raw_stat_snapshot_matches(&opened_stat, &after_stat) &&
            fstatat(opened.parent_fd, opened.name, &path_after, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(path_after.st_mode) &&
            memory_raw_stat_snapshot_matches(&opened_stat, &path_after) &&
            lstat(path, &requested_after) == 0 && S_ISREG(requested_after.st_mode) &&
            memory_raw_stat_snapshot_matches(&opened_stat, &requested_after);
    memory_raw_authorized_open_close(&opened);
    if (!valid) {
        free(bytes);
        return CBM_MEMORY_RAW_READ_FAILED;
    }
#endif
    bytes[len] = 0;
    *out_bytes = bytes;
    *out_len = len;
    return CBM_MEMORY_RAW_READ_OK;
}

static int memory_raw_read_regular_file_scoped(const char *path, const char *canonical_root,
                                               size_t max_len, unsigned char **out_bytes,
                                               size_t *out_len) {
    if (!path || !out_bytes || !out_len) {
        return -1;
    }
    *out_bytes = NULL;
    *out_len = 0;
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return -1;
    }
    HANDLE handle =
        CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        free(wide);
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION before;
    LARGE_INTEGER before_size;
    char canonical_file[CBM_MEMORY_RAW_PATH_MAX];
    bool valid =
        GetFileInformationByHandle(handle, &before) &&
        !(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        GetFileSizeEx(handle, &before_size) && before_size.QuadPart >= 0 &&
        (uint64_t)before_size.QuadPart <= (uint64_t)max_len &&
        (uint64_t)before_size.QuadPart < (uint64_t)SIZE_MAX &&
        (!canonical_root || (memory_raw_final_path_from_handle(handle, canonical_file) &&
                             memory_raw_canonical_within_root(canonical_root, canonical_file)));
    size_t len = valid ? (size_t)before_size.QuadPart : 0;
    unsigned char *bytes = valid ? malloc(len + 1U) : NULL;
    if (!bytes) {
        valid = false;
    }
    size_t offset = 0;
    while (valid && offset < len) {
        DWORD chunk = (DWORD)((len - offset) > UINT32_MAX ? UINT32_MAX : (len - offset));
        DWORD got = 0;
        if (!ReadFile(handle, bytes + offset, chunk, &got, NULL) || got == 0) {
            valid = false;
            break;
        }
        offset += got;
    }
    if (valid && !memory_raw_windows_at_eof(handle)) {
        valid = false;
    }
    BY_HANDLE_FILE_INFORMATION after;
    LARGE_INTEGER after_size;
    if (valid &&
        (!GetFileInformationByHandle(handle, &after) || !GetFileSizeEx(handle, &after_size) ||
         after.dwVolumeSerialNumber != before.dwVolumeSerialNumber ||
         after.nFileIndexHigh != before.nFileIndexHigh ||
         after.nFileIndexLow != before.nFileIndexLow ||
         after_size.QuadPart != before_size.QuadPart ||
         CompareFileTime(&after.ftLastWriteTime, &before.ftLastWriteTime) != 0)) {
        valid = false;
    }
    HANDLE current =
        CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION current_info;
    if (valid &&
        (current == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(current, &current_info) ||
         (current_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
         (current_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
         current_info.dwVolumeSerialNumber != before.dwVolumeSerialNumber ||
         current_info.nFileIndexHigh != before.nFileIndexHigh ||
         current_info.nFileIndexLow != before.nFileIndexLow ||
         current_info.nFileSizeHigh != before.nFileSizeHigh ||
         current_info.nFileSizeLow != before.nFileSizeLow ||
         CompareFileTime(&current_info.ftLastWriteTime, &before.ftLastWriteTime) != 0)) {
        valid = false;
    }
    if (current != INVALID_HANDLE_VALUE) {
        CloseHandle(current);
    }
    CloseHandle(handle);
    free(wide);
#else
    (void)canonical_root;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return -1;
    }
    struct stat before;
    struct stat path_before;
    bool valid = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
                 (uint64_t)before.st_size <= (uint64_t)max_len &&
                 (uint64_t)before.st_size < (uint64_t)SIZE_MAX && lstat(path, &path_before) == 0 &&
                 S_ISREG(path_before.st_mode) &&
                 memory_raw_stat_snapshot_matches(&before, &path_before);
    size_t len = valid ? (size_t)before.st_size : 0;
    unsigned char *bytes = valid ? malloc(len + 1U) : NULL;
    if (!bytes) {
        valid = false;
    }
    size_t offset = 0;
    while (valid && offset < len) {
        ssize_t got = read(fd, bytes + offset, len - offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            valid = false;
            break;
        }
        offset += (size_t)got;
    }
    unsigned char extra = 0;
    ssize_t extra_read = -1;
    if (valid) {
        do {
            extra_read = read(fd, &extra, 1);
        } while (extra_read < 0 && errno == EINTR);
    }
    struct stat after;
    struct stat path_after;
    if (valid &&
        (extra_read != 0 || fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
         !memory_raw_stat_snapshot_matches(&before, &after) || lstat(path, &path_after) != 0 ||
         !S_ISREG(path_after.st_mode) || !memory_raw_stat_snapshot_matches(&before, &path_after))) {
        valid = false;
    }
    if (close(fd) != 0) {
        valid = false;
    }
#endif
    if (!valid) {
        free(bytes);
        return -1;
    }
    bytes[len] = 0;
    *out_bytes = bytes;
    *out_len = len;
    return 0;
}

int cbm_memory_raw_read_regular_file(const char *path, size_t max_len, unsigned char **out_bytes,
                                     size_t *out_len) {
    return memory_raw_read_regular_file_scoped(path, NULL, max_len, out_bytes, out_len);
}

int cbm_memory_raw_read_regular_object(const char *home, const char *path, size_t max_len,
                                       unsigned char **out_bytes, size_t *out_len) {
    if (!out_bytes || !out_len) {
        return -1;
    }
    *out_bytes = NULL;
    *out_len = 0;
#ifdef _WIN32
    if (!home || !path || max_len == 0) {
        return -1;
    }
    char normalized_home[CBM_MEMORY_RAW_PATH_MAX];
    char normalized_path[CBM_MEMORY_RAW_PATH_MAX];
    int home_n = snprintf(normalized_home, sizeof(normalized_home), "%s", home);
    int path_n = snprintf(normalized_path, sizeof(normalized_path), "%s", path);
    if (home_n < 0 || home_n >= (int)sizeof(normalized_home) || path_n < 0 ||
        path_n >= (int)sizeof(normalized_path)) {
        return -1;
    }
    memory_raw_normalize_native_path(normalized_home);
    memory_raw_normalize_native_path(normalized_path);
    size_t home_len = strlen(normalized_home);
    while (home_len > 3U &&
           (normalized_home[home_len - 1U] == '/' || normalized_home[home_len - 1U] == '\\')) {
        normalized_home[--home_len] = '\0';
    }
    if (_strnicmp(normalized_home, normalized_path, home_len) != 0 ||
        (normalized_path[home_len] != '/' && normalized_path[home_len] != '\\')) {
        return -1;
    }
    const char *relative = normalized_path + home_len + 1U;
    static const char raw_prefix[] = "raw/objects/";
    if (_strnicmp(relative, raw_prefix, sizeof(raw_prefix) - 1U) != 0) {
        return -1;
    }
    cbm_rooted_file_t file;
    cbm_rooted_file_status_t status =
        cbm_rooted_file_read(normalized_home, relative, max_len, &file);
    if (status != CBM_ROOTED_FILE_OK) {
        const char *diagnostics = getenv("CBM_WINDOWS_FILE_DIAGNOSTICS");
        if (diagnostics && strcmp(diagnostics, "1") == 0) {
            fprintf(stderr, "memory_raw.windows stage=rooted_read status=%d path=%s\n", (int)status,
                    relative);
        }
        cbm_rooted_file_free(&file);
        return -1;
    }
    *out_bytes = (unsigned char *)file.data;
    *out_len = file.len;
    file.data = NULL;
    cbm_rooted_file_free(&file);
    return 0;
#else
    (void)home;
    return memory_raw_read_regular_file_scoped(path, NULL, max_len, out_bytes, out_len);
#endif
}

#ifdef _WIN32
static bool memory_raw_object_matches(const char *path, const unsigned char *bytes, size_t len,
                                      const char *expected_hash) {
    unsigned char *existing = NULL;
    size_t existing_len = 0;
    if (!expected_hash ||
        cbm_memory_raw_read_regular_file(path, len, &existing, &existing_len) != 0 ||
        existing_len != len) {
        free(existing);
        return false;
    }
    char hash[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(existing, len, hash);
    bool matches = strcmp(hash, expected_hash) == 0 &&
                   (len == 0 || (bytes && memcmp(existing, bytes, len) == 0));
    free(existing);
    return matches;
}
#endif

#ifndef _WIN32
static bool memory_raw_fd_identity_matches(int left_fd, int right_fd) {
    struct stat left;
    struct stat right;
    return left_fd >= 0 && right_fd >= 0 && fstat(left_fd, &left) == 0 &&
           fstat(right_fd, &right) == 0 && left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino;
}

static bool memory_raw_object_matches_at(int directory_fd, const char *name,
                                         const unsigned char *bytes, size_t len,
                                         const char *expected_hash) {
    unsigned char *existing = NULL;
    size_t existing_len = 0;
    if (!expected_hash ||
        cbm_memory_raw_read_regular_at(directory_fd, name, len, &existing, &existing_len) != 0 ||
        existing_len != len) {
        free(existing);
        return false;
    }
    char hash[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(existing, existing_len, hash);
    bool matches = strcmp(hash, expected_hash) == 0 &&
                   (len == 0 || (bytes && memcmp(existing, bytes, len) == 0));
    free(existing);
    return matches;
}

static int memory_raw_stage_open_anchors(cbm_memory_raw_stage_t *stage) {
    if (!stage || !stage->home[0] || !stage->target[0] || !stage->target_parent[0]) {
        return -1;
    }
    stage->home_fd = memory_raw_open_directory_path(stage->home);
    if (stage->home_fd < 0) {
        return -1;
    }
    int raw_fd = memory_raw_open_directory_at(stage->home_fd, "raw");
    if (raw_fd < 0) {
        return -1;
    }
    stage->raw_root_fd = memory_raw_open_directory_at(raw_fd, "objects");
    close(raw_fd);
    if (stage->raw_root_fd < 0) {
        return -1;
    }

    char canonical_root[CBM_MEMORY_RAW_PATH_MAX];
    if (!memory_raw_parent_dir(stage->target_parent, canonical_root, sizeof(canonical_root))) {
        return -1;
    }
    int canonical_root_fd = memory_raw_open_directory_path(canonical_root);
    bool root_matches = canonical_root_fd >= 0 &&
                        memory_raw_fd_identity_matches(stage->raw_root_fd, canonical_root_fd);
    if (canonical_root_fd >= 0) {
        close(canonical_root_fd);
    }
    if (!root_matches ||
        !memory_raw_component_name(stage->target_parent, stage->target_parent_name) ||
        !memory_raw_component_name(stage->target, stage->target_name)) {
        return -1;
    }
    return 0;
}

static int memory_raw_stage_ensure_staging(cbm_memory_raw_stage_t *stage) {
    if (!stage || stage->home_fd < 0) {
        return -1;
    }
    if (stage->staging_fd >= 0) {
        return 0;
    }
    if (mkdirat(stage->home_fd, ".ingest-staging", 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    stage->staging_fd = memory_raw_open_directory_at(stage->home_fd, ".ingest-staging");
    if (stage->staging_fd < 0 || fchmod(stage->staging_fd, 0700) != 0) {
        return -1;
    }
    int n = snprintf(stage->root, sizeof(stage->root), "%s/.ingest-staging", stage->home);
    return n >= 0 && n < (int)sizeof(stage->root) ? 0 : -1;
}

static int memory_raw_stage_open_target_parent(cbm_memory_raw_stage_t *stage, bool create) {
    if (!stage || stage->raw_root_fd < 0 || !stage->target_parent_name[0]) {
        return -1;
    }
    if (stage->target_parent_fd >= 0) {
        return 0;
    }
    bool created = false;
    if (create) {
        if (mkdirat(stage->raw_root_fd, stage->target_parent_name, 0700) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            return -1;
        }
    }
    stage->target_parent_fd =
        memory_raw_open_directory_at(stage->raw_root_fd, stage->target_parent_name);
    if (stage->target_parent_fd < 0 || fchmod(stage->target_parent_fd, 0700) != 0) {
        if (created) {
            (void)unlinkat(stage->raw_root_fd, stage->target_parent_name, AT_REMOVEDIR);
        }
        return -1;
    }
    stage->target_parent_created = stage->target_parent_created || created;
    return 0;
}
#endif

static int memory_raw_durable_flush(FILE *fp) {
    if (fflush(fp) != 0) {
        return -1;
    }
#ifdef _WIN32
    return _commit(_fileno(fp));
#else
    return fsync(fileno(fp));
#endif
}

#ifdef _WIN32
static int memory_raw_link_no_replace(const char *staged, const char *target) {
    wchar_t *wide_staged = cbm_utf8_to_wide(staged);
    wchar_t *wide_target = cbm_utf8_to_wide(target);
    if (!wide_staged || !wide_target) {
        free(wide_staged);
        free(wide_target);
        return -1;
    }
    /* Keep immutable-object publication available on ReFS/Dev Drive volumes:
     * a same-volume move is atomic and fails if the target already exists. */
    BOOL installed = MoveFileExW(wide_staged, wide_target, MOVEFILE_WRITE_THROUGH);
    DWORD error = installed ? ERROR_SUCCESS : GetLastError();
    free(wide_staged);
    free(wide_target);
    if (installed) {
        return 0;
    }
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? 1 : -1;
}
#endif

void cbm_memory_raw_stage_dispose(cbm_memory_raw_stage_t *stage, bool rollback_installed) {
    if (!stage) {
        return;
    }
    (void)rollback_installed;
#ifndef _WIN32
    if (stage->staged_exists && stage->staging_fd >= 0 && stage->staged_name[0]) {
        (void)unlinkat(stage->staging_fd, stage->staged_name, 0);
    }
    if (stage->lease_exists && stage->staging_fd >= 0 && stage->lease_name[0]) {
        (void)unlinkat(stage->staging_fd, stage->lease_name, 0);
    }
    if (stage->staged_fd >= 0) {
        close(stage->staged_fd);
    }
    if (stage->target_parent_fd >= 0) {
        close(stage->target_parent_fd);
    }
    if (stage->target_lease_fd >= 0) {
        close(stage->target_lease_fd);
    }
    if (stage->target_parent_created && stage->raw_root_fd >= 0 && stage->target_parent_name[0]) {
        (void)unlinkat(stage->raw_root_fd, stage->target_parent_name, AT_REMOVEDIR);
    }
    if (stage->staging_fd >= 0) {
        close(stage->staging_fd);
    }
    if (stage->home_fd >= 0 && stage->root[0]) {
        (void)unlinkat(stage->home_fd, ".ingest-staging", AT_REMOVEDIR);
    }
    if (stage->raw_root_fd >= 0) {
        close(stage->raw_root_fd);
    }
    if (stage->home_fd >= 0) {
        close(stage->home_fd);
    }
#else
    if (stage->staged_exists) {
        (void)cbm_unlink(stage->staged);
    }
    if (stage->root[0]) {
        (void)cbm_rmdir(stage->root);
    }
    if (stage->target_parent_created && stage->target_parent[0]) {
        (void)cbm_rmdir(stage->target_parent);
    }
#endif
    free(stage);
}

static int memory_raw_create_directory_no_replace(const char *path) {
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return -1;
    }
    BOOL created = CreateDirectoryW(wide, NULL);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    free(wide);
    if (created) {
        return 0;
    }
    return error == ERROR_ALREADY_EXISTS ? 1 : -1;
#else
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    return errno == EEXIST ? 1 : -1;
#endif
}

static int memory_raw_stage_create_internal(const char *home, const char *target,
                                            const char *target_parent, const unsigned char *bytes,
                                            size_t len, const char *expected_hash,
                                            bool allow_invalid_target,
                                            cbm_memory_raw_stage_t **out_stage) {
    if (!out_stage) {
        return -1;
    }
    *out_stage = NULL;
    if (!home || !target || !target_parent || !expected_hash || (len > 0 && !bytes)) {
        return -1;
    }
    cbm_memory_raw_stage_t *stage = calloc(1, sizeof(*stage));
    if (!stage) {
        return -1;
    }
#ifndef _WIN32
    stage->home_fd = -1;
    stage->raw_root_fd = -1;
    stage->staging_fd = -1;
    stage->staged_fd = -1;
    stage->target_parent_fd = -1;
    stage->target_lease_fd = -1;
#endif
    if (!memory_raw_canonical_home(home, stage->home) ||
        memory_raw_validate_object_scope(home, target, target_parent, false, NULL, stage->target,
                                         sizeof(stage->target), stage->target_parent,
                                         sizeof(stage->target_parent)) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    static atomic_uint_fast64_t sequence = ATOMIC_VAR_INIT(1);
#ifndef _WIN32
    if (memory_raw_stage_open_anchors(stage) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    stage->target_parent_fd =
        memory_raw_open_directory_at(stage->raw_root_fd, stage->target_parent_name);
    if (stage->target_parent_fd >= 0) {
        struct stat target_entry;
        int target_stat_rc = fstatat(stage->target_parent_fd, stage->target_name, &target_entry,
                                     AT_SYMLINK_NOFOLLOW);
        int target_stat_error = target_stat_rc == 0 ? 0 : errno;
        bool target_exists = target_stat_rc == 0;
        bool target_matches =
            target_exists &&
            cbm_memory_raw_lock_regular_at(stage->target_parent_fd, stage->target_name,
                                           &stage->target_lease_fd) == 0 &&
            memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name, bytes, len,
                                         expected_hash) &&
            memory_raw_fd_matches_regular_entry(stage->target_lease_fd, stage->target_parent_fd,
                                                stage->target_name, NULL);
        if (target_matches) {
            if (memory_raw_stage_ensure_staging(stage) != 0) {
                cbm_memory_raw_stage_dispose(stage, false);
                return -1;
            }
            bool linked = false;
            for (unsigned int attempt = 0; attempt < 32 && !linked; attempt++) {
                uint64_t seq = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed);
                int n = snprintf(stage->lease_name, sizeof(stage->lease_name),
                                 "ingest-%ld-%" PRIu64 "-lease-%u", (long)getpid(), seq, attempt);
                if (n < 0 || n >= (int)sizeof(stage->lease_name)) {
                    cbm_memory_raw_stage_dispose(stage, false);
                    return -1;
                }
                if (linkat(stage->target_parent_fd, stage->target_name, stage->staging_fd,
                           stage->lease_name, 0) == 0) {
                    linked = true;
                } else if (errno != EEXIST) {
                    cbm_memory_raw_stage_dispose(stage, false);
                    return -1;
                }
            }
            stage->lease_exists = linked;
            struct stat lease_entry;
            if (!linked ||
                fstatat(stage->staging_fd, stage->lease_name, &lease_entry, AT_SYMLINK_NOFOLLOW) !=
                    0 ||
                !S_ISREG(lease_entry.st_mode) ||
                !memory_raw_fd_matches_regular_entry(
                    stage->target_lease_fd, stage->target_parent_fd, stage->target_name, NULL)) {
                cbm_memory_raw_stage_dispose(stage, false);
                return -1;
            }
            struct stat leased_fd;
            if (fstat(stage->target_lease_fd, &leased_fd) != 0 ||
                leased_fd.st_dev != lease_entry.st_dev || leased_fd.st_ino != lease_entry.st_ino) {
                cbm_memory_raw_stage_dispose(stage, false);
                return -1;
            }
            *out_stage = stage;
            return 0;
        }
        if (stage->target_lease_fd >= 0) {
            close(stage->target_lease_fd);
            stage->target_lease_fd = -1;
        }
        if (target_exists) {
            if (!allow_invalid_target) {
                cbm_memory_raw_stage_dispose(stage, false);
                return -1;
            }
        } else if (target_stat_error != ENOENT) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
    }
    if (memory_raw_stage_ensure_staging(stage) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    int staged_fd = -1;
    for (unsigned int attempt = 0; attempt < 32 && staged_fd < 0; attempt++) {
        uint64_t seq = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed);
        int n = snprintf(stage->staged_name, sizeof(stage->staged_name),
                         "ingest-%ld-%" PRIu64 "-%u", (long)getpid(), seq, attempt);
        if (n < 0 || n >= (int)sizeof(stage->staged_name)) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
        int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        staged_fd = openat(stage->staging_fd, stage->staged_name, flags, 0600);
        if (staged_fd < 0 && errno != EEXIST) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
    }
    if (staged_fd < 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    stage->staged_fd = staged_fd;
    stage->staged_exists = true;
    if (flock(stage->staged_fd, LOCK_SH | LOCK_NB) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    int path_length =
        snprintf(stage->staged, sizeof(stage->staged), "%s/%s", stage->root, stage->staged_name);
    int writer_fd = dup(stage->staged_fd);
    FILE *fp = writer_fd >= 0 ? fdopen(writer_fd, "wb") : NULL;
    if (path_length < 0 || path_length >= (int)sizeof(stage->staged) || !fp) {
        if (writer_fd >= 0 && !fp) {
            close(writer_fd);
        }
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    size_t written = len ? fwrite(bytes, 1, len, fp) : 0;
    int flush_rc = written == len ? memory_raw_durable_flush(fp) : -1;
    int close_rc = fclose(fp);
    if (flush_rc != 0 || close_rc != 0 || fchmod(stage->staged_fd, 0600) != 0 ||
        !memory_raw_object_matches_at(stage->staging_fd, stage->staged_name, bytes, len,
                                      expected_hash) ||
        fsync(stage->staging_fd) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
#else
    if (memory_raw_path_exists_nofollow(stage->target)) {
        if (!memory_raw_object_matches(stage->target, bytes, len, expected_hash)) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
        *out_stage = stage;
        return 0;
    }
    if (cbm_memory_raw_ensure_private_subdir(stage->home, ".ingest-staging", stage->root,
                                             sizeof(stage->root)) != 0) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    FILE *fp = NULL;
    for (unsigned int attempt = 0; attempt < 32 && !fp; attempt++) {
        uint64_t seq = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed);
        int n = snprintf(stage->staged, sizeof(stage->staged), "%s/ingest-%ld-%" PRIu64 "-%u",
                         stage->root, (long)getpid(), seq, attempt);
        if (n < 0 || n >= (int)sizeof(stage->staged)) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
        fp = cbm_fopen(stage->staged, "wbx");
        if (!fp && errno != EEXIST) {
            cbm_memory_raw_stage_dispose(stage, false);
            return -1;
        }
    }
    if (!fp) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
    stage->staged_exists = true;
    size_t written = len ? fwrite(bytes, 1, len, fp) : 0;
    int flush_rc = written == len ? memory_raw_durable_flush(fp) : -1;
    int close_rc = fclose(fp);
    if (flush_rc != 0 || close_rc != 0 ||
        !memory_raw_object_matches(stage->staged, bytes, len, expected_hash)) {
        cbm_memory_raw_stage_dispose(stage, false);
        return -1;
    }
#endif
    *out_stage = stage;
    return 0;
}

int cbm_memory_raw_stage_create(const char *home, const char *target, const char *target_parent,
                                const unsigned char *bytes, size_t len, const char *expected_hash,
                                cbm_memory_raw_stage_t **out_stage) {
    return memory_raw_stage_create_internal(home, target, target_parent, bytes, len, expected_hash,
                                            false, out_stage);
}

int cbm_memory_raw_stage_create_repair(const char *home, const char *target,
                                       const char *target_parent, const unsigned char *bytes,
                                       size_t len, const char *expected_hash,
                                       cbm_memory_raw_stage_t **out_stage) {
    return memory_raw_stage_create_internal(home, target, target_parent, bytes, len, expected_hash,
                                            true, out_stage);
}

int cbm_memory_raw_stage_promote(cbm_memory_raw_stage_t *stage, const unsigned char *bytes,
                                 size_t len, const char *expected_hash) {
    if (!stage || !expected_hash || (len > 0 && !bytes)) {
        return -1;
    }
#ifndef _WIN32
    if (!stage->staged_exists) {
        return stage->target_lease_fd >= 0 && stage->target_parent_fd >= 0 &&
                       memory_raw_fd_matches_regular_entry(stage->target_lease_fd,
                                                           stage->target_parent_fd,
                                                           stage->target_name, NULL) &&
                       memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name,
                                                    bytes, len, expected_hash)
                   ? 0
                   : -1;
    }
    if (memory_raw_stage_open_target_parent(stage, true) != 0) {
        return -1;
    }
    struct stat staged_handle;
    struct stat staged_entry;
    if (stage->staged_fd < 0 || fstat(stage->staged_fd, &staged_handle) != 0 ||
        fstatat(stage->staging_fd, stage->staged_name, &staged_entry, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(staged_entry.st_mode) || staged_handle.st_dev != staged_entry.st_dev ||
        staged_handle.st_ino != staged_entry.st_ino) {
        return -1;
    }
    int install_rc = linkat(stage->staging_fd, stage->staged_name, stage->target_parent_fd,
                            stage->target_name, 0);
    if (install_rc == 0) {
        return memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name, bytes, len,
                                            expected_hash) &&
                       fsync(stage->target_parent_fd) == 0
                   ? 0
                   : -1;
    }
    return errno == EEXIST &&
                   memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name, bytes,
                                                len, expected_hash)
               ? 0
               : -1;
#else
    char canonical_target[CBM_MEMORY_RAW_PATH_MAX];
    char canonical_parent[CBM_MEMORY_RAW_PATH_MAX];
    if (!stage->staged_exists) {
        return memory_raw_validate_object_scope(
                   stage->home, stage->target, stage->target_parent, false, NULL, canonical_target,
                   sizeof(canonical_target), canonical_parent, sizeof(canonical_parent)) == 0 &&
                       memory_raw_object_matches(canonical_target, bytes, len, expected_hash)
                   ? 0
                   : -1;
    }
    bool parent_created = false;
    if (memory_raw_validate_object_scope(stage->home, stage->target, stage->target_parent, true,
                                         &parent_created, canonical_target,
                                         sizeof(canonical_target), canonical_parent,
                                         sizeof(canonical_parent)) != 0) {
        return -1;
    }
    (void)snprintf(stage->target, sizeof(stage->target), "%s", canonical_target);
    (void)snprintf(stage->target_parent, sizeof(stage->target_parent), "%s", canonical_parent);
    stage->target_parent_created = parent_created;
    int install_rc = memory_raw_link_no_replace(stage->staged, stage->target);
    if (install_rc == 0) {
        return memory_raw_object_matches(stage->target, bytes, len, expected_hash) ? 0 : -1;
    }
    return install_rc == 1 && memory_raw_object_matches(stage->target, bytes, len, expected_hash)
               ? 0
               : -1;
#endif
}

int cbm_memory_raw_stage_repair_promote(cbm_memory_raw_stage_t *stage, const unsigned char *bytes,
                                        size_t len, const char *expected_hash) {
    if (!stage || !expected_hash || (len > 0 && !bytes)) {
        return -1;
    }
#ifdef _WIN32
    return cbm_memory_raw_stage_promote(stage, bytes, len, expected_hash);
#else
    if (memory_raw_stage_open_target_parent(stage, true) != 0) {
        return -1;
    }
    if (memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name, bytes, len,
                                     expected_hash)) {
        return 0;
    }
    if (!stage->staged_exists || stage->staged_fd < 0) {
        return -1;
    }
    struct stat staged_handle;
    struct stat staged_entry;
    if (fstat(stage->staged_fd, &staged_handle) != 0 ||
        fstatat(stage->staging_fd, stage->staged_name, &staged_entry, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(staged_entry.st_mode) || staged_handle.st_dev != staged_entry.st_dev ||
        staged_handle.st_ino != staged_entry.st_ino) {
        return -1;
    }

    struct stat target_entry;
    bool target_exists = fstatat(stage->target_parent_fd, stage->target_name, &target_entry,
                                 AT_SYMLINK_NOFOLLOW) == 0;
    if (target_exists && S_ISDIR(target_entry.st_mode)) {
        return -1;
    }
    static atomic_uint_fast64_t repair_sequence = ATOMIC_VAR_INIT(1);
    char quarantine[CBM_MEMORY_RAW_PATH_MAX] = "";
    bool quarantined = false;
    if (target_exists) {
        for (unsigned int attempt = 0; attempt < 32 && !quarantined; attempt++) {
            uint64_t seq = atomic_fetch_add_explicit(&repair_sequence, 1, memory_order_relaxed);
            int n = snprintf(quarantine, sizeof(quarantine), ".repair-%ld-%" PRIu64 "-%u",
                             (long)getpid(), seq, attempt);
            if (n < 0 || n >= (int)sizeof(quarantine)) {
                return -1;
            }
            if (renameat(stage->target_parent_fd, stage->target_name, stage->target_parent_fd,
                         quarantine) == 0) {
                quarantined = true;
            } else if (errno == ENOENT) {
                target_exists = false;
                break;
            } else if (errno != EEXIST) {
                return -1;
            }
        }
        if (target_exists && !quarantined) {
            return -1;
        }
    }

    bool installed = linkat(stage->staging_fd, stage->staged_name, stage->target_parent_fd,
                            stage->target_name, 0) == 0;
    bool verified =
        installed && memory_raw_object_matches_at(stage->target_parent_fd, stage->target_name,
                                                  bytes, len, expected_hash);
    if (!verified) {
        if (quarantined) {
            if (linkat(stage->target_parent_fd, quarantine, stage->target_parent_fd,
                       stage->target_name, 0) == 0) {
                (void)unlinkat(stage->target_parent_fd, quarantine, 0);
            }
        }
        return -1;
    }
    if (quarantined) {
        (void)unlinkat(stage->target_parent_fd, quarantine, 0);
    }
    return fsync(stage->target_parent_fd) == 0 ? 0 : -1;
#endif
}

#ifndef _WIN32
static bool memory_raw_gc_old_enough(const struct stat *st, time_t cutoff) {
    return st && st->st_mtime <= cutoff;
}

static bool memory_raw_gc_name_prefix(const char *name, const char *prefix) {
    return name && prefix && strncmp(name, prefix, strlen(prefix)) == 0;
}

static bool memory_raw_gc_owner_alive(const char *name, const char *prefix) {
    if (!memory_raw_gc_name_prefix(name, prefix)) {
        return false;
    }
    const char *cursor = name + strlen(prefix);
    errno = 0;
    char *end = NULL;
    long long value = strtoll(cursor, &end, 10);
    pid_t owner = (pid_t)value;
    if (errno != 0 || end == cursor || !end || *end != '-' || value <= 0 ||
        (long long)owner != value) {
        return false;
    }
    return kill(owner, 0) == 0 || errno == EPERM;
}

static int memory_raw_gc_flat_staging(int home_fd, const char *directory_name, const char *prefix,
                                      time_t cutoff, size_t *removed) {
    int directory_fd = memory_raw_open_directory_at(home_fd, directory_name);
    if (directory_fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    int scan_fd = dup(directory_fd);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory) {
        if (scan_fd >= 0) {
            close(scan_fd);
        }
        close(directory_fd);
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !memory_raw_gc_name_prefix(entry->d_name, prefix) ||
            memory_raw_gc_owner_alive(entry->d_name, prefix)) {
            continue;
        }
        struct stat st;
        if (fstatat(directory_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            !S_ISDIR(st.st_mode) && memory_raw_gc_old_enough(&st, cutoff) &&
            unlinkat(directory_fd, entry->d_name, 0) == 0) {
            (*removed)++;
        }
    }
    closedir(directory);
    (void)fsync(directory_fd);
    close(directory_fd);
    (void)unlinkat(home_fd, directory_name, AT_REMOVEDIR);
    return 0;
}

static int memory_raw_gc_import_staging(int home_fd, time_t cutoff, size_t *removed) {
    static const char root_name[] = ".import-staging";
    int root_fd = memory_raw_open_directory_at(home_fd, root_name);
    if (root_fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    int scan_fd = dup(root_fd);
    DIR *root = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!root) {
        if (scan_fd >= 0) {
            close(scan_fd);
        }
        close(root_fd);
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !memory_raw_gc_name_prefix(entry->d_name, "import-") ||
            memory_raw_gc_owner_alive(entry->d_name, "import-")) {
            continue;
        }
        struct stat directory_stat;
        if (fstatat(root_fd, entry->d_name, &directory_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(directory_stat.st_mode) ||
            !memory_raw_gc_old_enough(&directory_stat, cutoff)) {
            continue;
        }
        int operation_fd = memory_raw_open_directory_at(root_fd, entry->d_name);
        if (operation_fd < 0) {
            continue;
        }
        int operation_scan_fd = dup(operation_fd);
        DIR *operation = operation_scan_fd >= 0 ? fdopendir(operation_scan_fd) : NULL;
        if (!operation) {
            if (operation_scan_fd >= 0) {
                close(operation_scan_fd);
            }
            close(operation_fd);
            continue;
        }
        struct dirent *child;
        while ((child = readdir(operation)) != NULL) {
            if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
                continue;
            }
            struct stat child_stat;
            if (fstatat(operation_fd, child->d_name, &child_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
                !S_ISDIR(child_stat.st_mode) && unlinkat(operation_fd, child->d_name, 0) == 0) {
                (*removed)++;
            }
        }
        closedir(operation);
        (void)fsync(operation_fd);
        close(operation_fd);
        (void)unlinkat(root_fd, entry->d_name, AT_REMOVEDIR);
    }
    closedir(root);
    (void)fsync(root_fd);
    close(root_fd);
    (void)unlinkat(home_fd, root_name, AT_REMOVEDIR);
    return 0;
}

static bool memory_raw_gc_lower_hex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

static bool memory_raw_gc_object_name(const char *prefix, const char *name) {
    if (!prefix || strlen(prefix) != 2 || !name || strlen(name) < CBM_SHA256_HEX_LEN ||
        name[0] != prefix[0] || name[1] != prefix[1]) {
        return false;
    }
    for (int i = 0; i < CBM_SHA256_HEX_LEN; i++) {
        if (!memory_raw_gc_lower_hex(name[i])) {
            return false;
        }
    }
    if (name[CBM_SHA256_HEX_LEN] == '\0') {
        return true;
    }
    if (name[CBM_SHA256_HEX_LEN] != '.' || !name[CBM_SHA256_HEX_LEN + 1]) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)name + CBM_SHA256_HEX_LEN + 1;
         *cursor; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '_' || *cursor == '-' || *cursor == '.')) {
            return false;
        }
    }
    return true;
}

static int memory_raw_gc_orphans(int home_fd, time_t cutoff,
                                 cbm_memory_raw_reference_fn is_referenced, void *opaque,
                                 size_t *removed) {
    if (!is_referenced) {
        return 0;
    }
    int raw_fd = memory_raw_open_directory_at(home_fd, "raw");
    if (raw_fd < 0) {
        return -1;
    }
    int objects_fd = memory_raw_open_directory_at(raw_fd, "objects");
    close(raw_fd);
    if (objects_fd < 0) {
        return -1;
    }
    int scan_fd = dup(objects_fd);
    DIR *objects = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!objects) {
        if (scan_fd >= 0) {
            close(scan_fd);
        }
        close(objects_fd);
        return -1;
    }
    struct dirent *prefix;
    while ((prefix = readdir(objects)) != NULL) {
        if (strlen(prefix->d_name) != 2 || !memory_raw_gc_lower_hex(prefix->d_name[0]) ||
            !memory_raw_gc_lower_hex(prefix->d_name[1])) {
            continue;
        }
        int prefix_fd = memory_raw_open_directory_at(objects_fd, prefix->d_name);
        if (prefix_fd < 0) {
            continue;
        }
        int prefix_scan_fd = dup(prefix_fd);
        DIR *files = prefix_scan_fd >= 0 ? fdopendir(prefix_scan_fd) : NULL;
        if (!files) {
            if (prefix_scan_fd >= 0) {
                close(prefix_scan_fd);
            }
            close(prefix_fd);
            continue;
        }
        struct dirent *file;
        while ((file = readdir(files)) != NULL) {
            if (!memory_raw_gc_object_name(prefix->d_name, file->d_name)) {
                continue;
            }
            int object_fd = -1;
            struct stat st;
            if (memory_raw_lock_regular_at(prefix_fd, file->d_name, LOCK_EX, &object_fd) != 0 ||
                !memory_raw_fd_matches_regular_entry(object_fd, prefix_fd, file->d_name, &st) ||
                st.st_nlink > 1 || !memory_raw_gc_old_enough(&st, cutoff)) {
                if (object_fd >= 0) {
                    close(object_fd);
                }
                continue;
            }
            char relative[CBM_MEMORY_RAW_PATH_MAX];
            int n = snprintf(relative, sizeof(relative), "raw/objects/%s/%s", prefix->d_name,
                             file->d_name);
            if (n >= 0 && n < (int)sizeof(relative) && !is_referenced(relative, opaque) &&
                unlinkat(prefix_fd, file->d_name, 0) == 0) {
                (*removed)++;
            }
            close(object_fd);
        }
        closedir(files);
        (void)fsync(prefix_fd);
        close(prefix_fd);
        (void)unlinkat(objects_fd, prefix->d_name, AT_REMOVEDIR);
    }
    closedir(objects);
    (void)fsync(objects_fd);
    close(objects_fd);
    return 0;
}
#endif

int cbm_memory_raw_gc(const char *home, long long grace_seconds,
                      cbm_memory_raw_reference_fn is_referenced, void *opaque,
                      size_t *out_staging_removed, size_t *out_orphans_removed) {
    if (!home || grace_seconds < 0) {
        return -1;
    }
    size_t staging_removed = 0;
    size_t orphans_removed = 0;
#ifdef _WIN32
    (void)is_referenced;
    (void)opaque;
#else
    char canonical_home[CBM_MEMORY_RAW_PATH_MAX];
    time_t now = time(NULL);
    time_t grace = grace_seconds > (long long)now ? now : (time_t)grace_seconds;
    time_t cutoff = now - grace;
    if (cbm_memory_raw_resolve_directory(home, canonical_home, sizeof(canonical_home)) != 0) {
        return -1;
    }
    int home_fd = memory_raw_open_directory_path(canonical_home);
    if (home_fd < 0 ||
        memory_raw_gc_flat_staging(home_fd, ".ingest-staging", "ingest-", cutoff,
                                   &staging_removed) != 0 ||
        memory_raw_gc_import_staging(home_fd, cutoff, &staging_removed) != 0 ||
        memory_raw_gc_orphans(home_fd, cutoff, is_referenced, opaque, &orphans_removed) != 0) {
        if (home_fd >= 0) {
            close(home_fd);
        }
        return -1;
    }
    close(home_fd);
#endif
    if (out_staging_removed) {
        *out_staging_removed = staging_removed;
    }
    if (out_orphans_removed) {
        *out_orphans_removed = orphans_removed;
    }
    return 0;
}
