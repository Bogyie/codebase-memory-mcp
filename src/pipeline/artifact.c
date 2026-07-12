/* Persistent, generation-addressed artifact export/import. */
#include "pipeline/artifact.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/rooted_file.h"
#include "foundation/sha256.h"
#include "foundation/subprocess.h"
#include "git/git_context.h"
#include "store/store.h"
#include "zstd_store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include "foundation/win_utf8.h"
#include <io.h>
#include <wchar.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

enum {
    ART_DIR_PERMS = 0755,
    ART_PRIVATE_DIR_PERMS = 0700,
    ART_FILE_PERMS = 0644,
    ART_PRIVATE_FILE_PERMS = 0600,
    ART_ZSTD_FAST = 3,
    ART_ZSTD_BEST = 9,
    ART_META_MAX_BYTES = 1024 * 1024,
    ART_IO_BUFFER = 128 * 1024,
    ART_GIT_OUTPUT_MAX = 256,
};

#define ART_GIB (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define ART_MAX_DECOMPRESSED_BYTES (UINT64_C(64) * ART_GIB)
#define ART_MAX_COMPRESSED_BYTES (UINT64_C(64) * ART_GIB)
#define ART_PAYLOAD_PREFIX "graph.db."
#define ART_PAYLOAD_SUFFIX ".zst"

static _Thread_local char g_export_error[CBM_SZ_512];
static _Thread_local cbm_artifact_test_hook_fn g_test_hook;
static _Thread_local void *g_test_hook_context;

void cbm_artifact_set_test_hook(cbm_artifact_test_hook_fn hook, void *context) {
    g_test_hook = hook;
    g_test_hook_context = context;
}

static bool run_test_hook(const char *stage) {
    return !g_test_hook || g_test_hook(stage, g_test_hook_context);
}

typedef struct {
    char *path;
#ifdef _WIN32
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION identity;
#else
    int fd;
#endif
} artifact_dir_t;

typedef struct {
    char *payload;
    char *commit;
    uint64_t original_size;
    uint64_t compressed_size;
    char compressed_sha256[CBM_SHA256_HEX_LEN + 1];
    char metadata_sha256[CBM_SHA256_HEX_LEN + 1];
} artifact_metadata_t;

typedef struct {
    FILE *fp;
    artifact_dir_t dir;
#ifdef _WIN32
    struct stat initial;
    BY_HANDLE_FILE_INFORMATION initial_info;
    char *path;
#else
    struct stat initial;
#endif
} artifact_payload_t;

static int export_fail(const char *stage, const char *path, const char *detail, int error) {
    const char *s = stage ? stage : "unknown";
    const char *d = detail ? detail : "unknown";
    if (path && error) {
        (void)snprintf(g_export_error, sizeof(g_export_error), "%s: %s errno=%d path=%s", s, d,
                       error, path);
        cbm_log_error("artifact.export", "stage", s, "err", d, "path", path);
    } else if (path) {
        (void)snprintf(g_export_error, sizeof(g_export_error), "%s: %s path=%s", s, d, path);
        cbm_log_error("artifact.export", "stage", s, "err", d, "path", path);
    } else {
        (void)snprintf(g_export_error, sizeof(g_export_error), "%s: %s", s, d);
        cbm_log_error("artifact.export", "stage", s, "err", d);
    }
    return CBM_NOT_FOUND;
}

const char *cbm_artifact_export_last_error(void) {
    return g_export_error[0] ? g_export_error : NULL;
}

static bool size_add(size_t a, size_t b, size_t *out) {
    if (!out || b > SIZE_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

static char *path_join2(const char *a, const char *b) {
    if (!a || !a[0] || !b || !b[0]) {
        return NULL;
    }
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    bool slash = a[alen - 1] != '/' && a[alen - 1] != '\\';
    size_t total = 0;
    if (!size_add(alen, slash ? 1U : 0U, &total) || !size_add(total, blen, &total) ||
        !size_add(total, 1U, &total)) {
        return NULL;
    }
    char *path = malloc(total);
    if (!path) {
        return NULL;
    }
    memcpy(path, a, alen);
    size_t pos = alen;
    if (slash) {
        path[pos++] = '/';
    }
    memcpy(path + pos, b, blen + 1U);
    return path;
}

static char *path_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) {
        return NULL;
    }
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    size_t total = 0;
    if (!size_add(plen, slen, &total) || !size_add(total, 1U, &total)) {
        return NULL;
    }
    char *result = malloc(total);
    if (result) {
        memcpy(result, path, plen);
        memcpy(result + plen, suffix, slen + 1U);
    }
    return result;
}

static void unlink_temp_db(const char *path) {
    if (!path) {
        return;
    }
    (void)cbm_unlink(path);
    char *wal = path_suffix(path, "-wal");
    char *shm = path_suffix(path, "-shm");
    if (wal) {
        (void)cbm_unlink(wal);
    }
    if (shm) {
        (void)cbm_unlink(shm);
    }
    free(wal);
    free(shm);
}

static bool sync_file(FILE *fp) {
    if (!fp || fflush(fp) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(cbm_fileno(fp)) == 0;
#else
    return fsync(cbm_fileno(fp)) == 0;
#endif
}

static bool sync_directory(const artifact_dir_t *dir) {
#ifdef _WIN32
    (void)dir;
    return true;
#else
    return dir && dir->fd >= 0 && fsync(dir->fd) == 0;
#endif
}

static void artifact_dir_close(artifact_dir_t *dir) {
    if (!dir) {
        return;
    }
#ifdef _WIN32
    if (dir->handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(dir->handle);
    }
    dir->handle = INVALID_HANDLE_VALUE;
    memset(&dir->identity, 0, sizeof(dir->identity));
#else
    if (dir->fd >= 0) {
        (void)close(dir->fd);
    }
    dir->fd = -1;
#endif
    free(dir->path);
    dir->path = NULL;
}

#ifdef _WIN32
static bool canonical_child_of(const char *root, const char *child) {
    enum { CANON_CAP = 32768 };
    char *r = malloc(CANON_CAP);
    char *c = malloc(CANON_CAP);
    bool ok = false;
    if (r && c && cbm_canonical_path(root, r, CANON_CAP) &&
        cbm_canonical_path(child, c, CANON_CAP)) {
        size_t n = strlen(r);
        ok = _strnicmp(r, c, n) == 0 &&
             (c[n] == '\0' || c[n] == '/' || c[n] == '\\' || r[n - 1] == '/' || r[n - 1] == '\\');
    }
    free(r);
    free(c);
    return ok;
}

static char *windows_handle_path(HANDLE handle) {
    DWORD needed =
        GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0 || needed > 32768U) {
        return NULL;
    }
    wchar_t *wide = malloc(((size_t)needed + 1U) * sizeof(*wide));
    if (!wide) {
        return NULL;
    }
    DWORD written = GetFinalPathNameByHandleW(handle, wide, needed + 1U,
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written > needed) {
        free(wide);
        return NULL;
    }
    const wchar_t *normalized = wide;
    wchar_t *unc = NULL;
    if (wcsncmp(wide, L"\\\\?\\UNC\\", 8) == 0) {
        size_t tail = wcslen(wide + 8);
        unc = malloc((tail + 3U) * sizeof(*unc));
        if (!unc) {
            free(wide);
            return NULL;
        }
        unc[0] = L'\\';
        unc[1] = L'\\';
        memcpy(unc + 2, wide + 8, (tail + 1U) * sizeof(*unc));
        normalized = unc;
    } else if (wcsncmp(wide, L"\\\\?\\", 4) == 0) {
        normalized = wide + 4;
    }
    char *result = cbm_wide_to_utf8(normalized);
    free(unc);
    free(wide);
    return result;
}

static bool windows_file_info_same(const BY_HANDLE_FILE_INFORMATION *a,
                                   const BY_HANDLE_FILE_INFORMATION *b) {
    return a && b && a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow &&
           a->nFileSizeHigh == b->nFileSizeHigh && a->nFileSizeLow == b->nFileSizeLow &&
           a->ftLastWriteTime.dwHighDateTime == b->ftLastWriteTime.dwHighDateTime &&
           a->ftLastWriteTime.dwLowDateTime == b->ftLastWriteTime.dwLowDateTime;
}

static bool windows_file_identity_same(const BY_HANDLE_FILE_INFORMATION *a,
                                       const BY_HANDLE_FILE_INFORMATION *b) {
    return a && b && a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}

static HANDLE windows_open_artifact_directory(const char *path,
                                              BY_HANDLE_FILE_INFORMATION *out_info) {
    if (!path || !path[0] || !out_info) {
        return INVALID_HANDLE_VALUE;
    }
    wchar_t *wide = cbm_utf8_to_wide(path);
    /* Deliberately omit FILE_SHARE_DELETE: the pinned handle prevents the
     * directory entry itself from being renamed or replaced while in use. */
    HANDLE handle =
        wide ? CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           NULL)
             : INVALID_HANDLE_VALUE;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, out_info) ||
        !(out_info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (out_info->dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle);
        }
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

/* Windows lacks the openat-style operations used below on POSIX. Keep the
 * artifact directory itself pinned and fail closed whenever its pathname no
 * longer resolves to the pinned object before a pathname-based operation. */
static bool artifact_dir_validate(const artifact_dir_t *dir) {
    if (!dir || !dir->path || dir->handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION pinned;
    if (GetFileType(dir->handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(dir->handle, &pinned) ||
        !(pinned.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (pinned.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
        !windows_file_identity_same(&dir->identity, &pinned)) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION live_info;
    HANDLE live = windows_open_artifact_directory(dir->path, &live_info);
    bool ok = live != INVALID_HANDLE_VALUE && windows_file_identity_same(&pinned, &live_info);
    if (live != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(live);
    }
    return ok;
}
#else
static bool artifact_dir_validate(const artifact_dir_t *dir) {
    return dir && dir->path && dir->fd >= 0;
}
#endif

static bool artifact_dir_open(const char *repo_path, bool create, artifact_dir_t *out) {
    if (!repo_path || !repo_path[0] || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
#ifndef _WIN32
    out->fd = -1;
    int root_flags = O_RDONLY;
#ifdef O_DIRECTORY
    root_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = open(repo_path, root_flags);
    if (root_fd < 0) {
        return false;
    }
    struct stat root_st;
    if (fstat(root_fd, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        (void)close(root_fd);
        return false;
    }
    struct stat lst;
    if (fstatat(root_fd, CBM_ARTIFACT_DIR, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
        int lookup_error = errno;
        if (!create || lookup_error != ENOENT ||
            (mkdirat(root_fd, CBM_ARTIFACT_DIR, ART_DIR_PERMS) != 0 && errno != EEXIST)) {
            (void)close(root_fd);
            return false;
        }
        if (fstatat(root_fd, CBM_ARTIFACT_DIR, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
            (void)close(root_fd);
            return false;
        }
    }
    if (!S_ISDIR(lst.st_mode)) {
        (void)close(root_fd);
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = openat(root_fd, CBM_ARTIFACT_DIR, flags);
    (void)close(root_fd);
    if (fd < 0) {
        return false;
    }
    struct stat opened;
    if (fstat(fd, &opened) != 0 || !S_ISDIR(opened.st_mode) || opened.st_dev != lst.st_dev ||
        opened.st_ino != lst.st_ino) {
        (void)close(fd);
        return false;
    }
    out->fd = fd;
#else
    out->handle = INVALID_HANDLE_VALUE;
    char *candidate = path_join2(repo_path, CBM_ARTIFACT_DIR);
    if (!candidate) {
        return false;
    }
    if (create && !cbm_mkdir_p(candidate, ART_DIR_PERMS)) {
        free(candidate);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION identity;
    HANDLE handle = windows_open_artifact_directory(candidate, &identity);
    char *opened_path = handle != INVALID_HANDLE_VALUE ? windows_handle_path(handle) : NULL;
    if (handle == INVALID_HANDLE_VALUE || !opened_path ||
        !canonical_child_of(repo_path, opened_path)) {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle);
        }
        free(opened_path);
        free(candidate);
        return false;
    }
    free(opened_path);
    out->path = candidate;
    out->handle = handle;
    out->identity = identity;
#endif
#ifndef _WIN32
    out->path = path_join2(repo_path, CBM_ARTIFACT_DIR);
#endif
    if (!out->path) {
        artifact_dir_close(out);
        return false;
    }
    return true;
}

static bool make_private_temp_dir(const artifact_dir_t *dir, char **out_path) {
    if (!dir || !dir->path || !out_path) {
        return false;
    }
    *out_path = NULL;
    for (unsigned int attempt = 0; attempt < 256U; attempt++) {
        char name[96];
#ifdef _WIN32
        int n = snprintf(name, sizeof(name), ".artifact-%lu-%u-XXXXXX",
                         (unsigned long)GetCurrentProcessId(), attempt);
#else
        int n = snprintf(name, sizeof(name), ".artifact-%ld-%" PRIu64 "-%u", (long)getpid(),
                         cbm_now_ns(), attempt);
#endif
        if (n <= 0 || (size_t)n >= sizeof(name)) {
            return false;
        }
        char *path = path_join2(dir->path, name);
        if (!path) {
            return false;
        }
#ifdef _WIN32
        if (!artifact_dir_validate(dir)) {
            free(path);
            return false;
        }
        if (cbm_mkdtemp(path)) {
            if (!artifact_dir_validate(dir)) {
                free(path);
                return false;
            }
            *out_path = path;
            return true;
        }
#else
        if (mkdirat(dir->fd, name, ART_PRIVATE_DIR_PERMS) == 0) {
            *out_path = path;
            return true;
        }
        if (errno != EEXIST) {
            free(path);
            return false;
        }
#endif
        free(path);
    }
    return false;
}

static void cleanup_private_temp_dir(const artifact_dir_t *dir, char *temp_dir,
                                     const char *snapshot, const char *compressed) {
    bool safe = artifact_dir_validate(dir);
    if (safe) {
        unlink_temp_db(snapshot);
    }
    safe = safe && artifact_dir_validate(dir);
    if (safe && compressed) {
        (void)cbm_unlink(compressed);
    }
    safe = safe && artifact_dir_validate(dir);
    if (safe && temp_dir) {
        (void)cbm_rmdir(temp_dir);
    }
    free(temp_dir);
}

static int open_exclusive_file(const char *path, int permissions) {
#ifdef _WIN32
    (void)permissions;
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return -1;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0) {
        (void)CloseHandle(handle);
    }
    return fd;
#else
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return open(path, flags, (mode_t)permissions);
#endif
}

static bool sqlite_backup_snapshot(const char *source_path, const char *dest_path) {
    sqlite3 *source = NULL;
    sqlite3 *dest = NULL;
    int rc =
        sqlite3_open_v2(source_path, &source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(source);
        return false;
    }
    rc = sqlite3_open_v2(dest_path, &dest,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(dest);
        sqlite3_close(source);
        return false;
    }
    (void)sqlite3_busy_timeout(source, 5000);
    (void)sqlite3_busy_timeout(dest, 5000);
    sqlite3_backup *backup = sqlite3_backup_init(dest, "main", source, "main");
    if (!backup) {
        sqlite3_close(dest);
        sqlite3_close(source);
        return false;
    }
    for (int attempt = 0; attempt < 50; attempt++) {
        rc = sqlite3_backup_step(backup, -1);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            break;
        }
        cbm_usleep(10000);
    }
    int finish_rc = sqlite3_backup_finish(backup);
    bool ok = rc == SQLITE_DONE && finish_rc == SQLITE_OK;
    if (sqlite3_close(dest) != SQLITE_OK) {
        ok = false;
    }
    if (sqlite3_close(source) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

static const char DROP_INDEXES_SQL[] = "BEGIN IMMEDIATE;"
                                       "DROP INDEX IF EXISTS idx_nodes_label;"
                                       "DROP INDEX IF EXISTS idx_nodes_name;"
                                       "DROP INDEX IF EXISTS idx_nodes_file;"
                                       "DROP INDEX IF EXISTS idx_edges_source;"
                                       "DROP INDEX IF EXISTS idx_edges_target;"
                                       "DROP INDEX IF EXISTS idx_edges_type;"
                                       "DROP INDEX IF EXISTS idx_edges_target_type;"
                                       "DROP INDEX IF EXISTS idx_edges_source_type;"
                                       "DROP INDEX IF EXISTS idx_edges_url_path;"
                                       "COMMIT;";

static bool strip_snapshot_indexes(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char *error = NULL;
    bool ok = sqlite3_exec(db, DROP_INDEXES_SQL, NULL, NULL, &error) == SQLITE_OK;
    sqlite3_free(error);
    error = NULL;
    if (ok) {
        ok = sqlite3_exec(db, "VACUUM;", NULL, NULL, &error) == SQLITE_OK;
    }
    sqlite3_free(error);
    if (sqlite3_close(db) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

static bool snapshot_integrity_and_counts(const char *path, const char *project, int *nodes,
                                          int *edges) {
    if (!nodes || !edges) {
        return false;
    }
    *nodes = 0;
    *edges = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(db, "PRAGMA quick_check(1);", -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        ok = sqlite3_step(stmt) == SQLITE_ROW;
        const char *answer = ok ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
        ok = answer && strcmp(answer, "ok") == 0;
    }
    sqlite3_finalize(stmt);
    static const char *queries[] = {"SELECT count(*) FROM nodes WHERE project=?1;",
                                    "SELECT count(*) FROM edges WHERE project=?1;"};
    int *outputs[] = {nodes, edges};
    for (size_t i = 0; ok && i < 2; i++) {
        stmt = NULL;
        ok = sqlite3_prepare_v2(db, queries[i], -1, &stmt, NULL) == SQLITE_OK &&
             sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
             sqlite3_step(stmt) == SQLITE_ROW;
        if (ok) {
            sqlite3_int64 count = sqlite3_column_int64(stmt, 0);
            ok = count >= 0 && count <= INT_MAX;
            if (ok) {
                *outputs[i] = (int)count;
            }
        }
        sqlite3_finalize(stmt);
    }
    if (sqlite3_close(db) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

static bool hex_sha256(const char *value) {
    if (!value || strlen(value) != CBM_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < CBM_SHA256_HEX_LEN; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool payload_name_valid(const char *name, const char *sha) {
    size_t prefix = sizeof(ART_PAYLOAD_PREFIX) - 1U;
    size_t suffix = sizeof(ART_PAYLOAD_SUFFIX) - 1U;
    size_t expected = prefix + CBM_SHA256_HEX_LEN + suffix;
    return name && sha && strlen(name) == expected &&
           memcmp(name, ART_PAYLOAD_PREFIX, prefix) == 0 &&
           memcmp(name + prefix, sha, CBM_SHA256_HEX_LEN) == 0 &&
           memcmp(name + prefix + CBM_SHA256_HEX_LEN, ART_PAYLOAD_SUFFIX, suffix + 1U) == 0;
}

static void metadata_free(artifact_metadata_t *meta) {
    if (!meta) {
        return;
    }
    free(meta->payload);
    free(meta->commit);
    memset(meta, 0, sizeof(*meta));
}

static bool metadata_read(const char *repo_path, artifact_metadata_t *out) {
    if (!repo_path || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    char relative[sizeof(CBM_ARTIFACT_DIR) + sizeof(CBM_ARTIFACT_META) + 2U];
    int rn = snprintf(relative, sizeof(relative), "%s/%s", CBM_ARTIFACT_DIR, CBM_ARTIFACT_META);
    if (rn <= 0 || (size_t)rn >= sizeof(relative)) {
        return false;
    }
    cbm_rooted_file_t file = {0};
    if (cbm_rooted_file_read(repo_path, relative, ART_META_MAX_BYTES, &file) !=
        CBM_ROOTED_FILE_OK) {
        cbm_rooted_file_free(&file);
        return false;
    }
    yyjson_doc *doc = yyjson_read(file.data, file.len, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *version =
        root && yyjson_is_obj(root) ? yyjson_obj_get(root, "schema_version") : NULL;
    yyjson_val *payload = root ? yyjson_obj_get(root, "payload") : NULL;
    yyjson_val *generation = root ? yyjson_obj_get(root, "generation") : NULL;
    yyjson_val *hash = root ? yyjson_obj_get(root, "compressed_sha256") : NULL;
    yyjson_val *original = root ? yyjson_obj_get(root, "original_size") : NULL;
    yyjson_val *compressed = root ? yyjson_obj_get(root, "compressed_size") : NULL;
    yyjson_val *commit = root ? yyjson_obj_get(root, "commit") : NULL;
    bool ok = version && yyjson_is_uint(version) &&
              yyjson_get_uint(version) == CBM_ARTIFACT_SCHEMA_VERSION && payload &&
              yyjson_is_str(payload) && generation && yyjson_is_str(generation) && hash &&
              yyjson_is_str(hash) && original && yyjson_is_uint(original) && compressed &&
              yyjson_is_uint(compressed);
    const char *payload_s = ok ? yyjson_get_str(payload) : NULL;
    const char *generation_s = ok ? yyjson_get_str(generation) : NULL;
    const char *hash_s = ok ? yyjson_get_str(hash) : NULL;
    uint64_t original_n = ok ? yyjson_get_uint(original) : 0;
    uint64_t compressed_n = ok ? yyjson_get_uint(compressed) : 0;
    ok = ok && hex_sha256(hash_s) && hex_sha256(generation_s) &&
         strcmp(hash_s, generation_s) == 0 && payload_name_valid(payload_s, hash_s) &&
         original_n > 0 && original_n <= ART_MAX_DECOMPRESSED_BYTES && compressed_n > 0 &&
         compressed_n <= ART_MAX_COMPRESSED_BYTES;
    if (commit && !yyjson_is_str(commit)) {
        ok = false;
    }
    const char *commit_s = ok && commit ? yyjson_get_str(commit) : NULL;
    if (commit_s && commit_s[0] && !hex_sha256(commit_s) && strlen(commit_s) != 40U) {
        ok = false;
    }
    if (ok && commit_s && commit_s[0]) {
        size_t n = strlen(commit_s);
        for (size_t i = 0; i < n; i++) {
            if (!isxdigit((unsigned char)commit_s[i])) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        out->payload = cbm_strdup(payload_s);
        out->commit = commit_s && commit_s[0] ? cbm_strdup(commit_s) : NULL;
        ok = out->payload && (!(commit_s && commit_s[0]) || out->commit);
    }
    if (ok) {
        out->original_size = original_n;
        out->compressed_size = compressed_n;
        memcpy(out->compressed_sha256, hash_s, CBM_SHA256_HEX_LEN + 1U);
        memcpy(out->metadata_sha256, file.sha256, CBM_SHA256_HEX_LEN + 1U);
    }
    yyjson_doc_free(doc);
    cbm_rooted_file_free(&file);
    if (!ok) {
        metadata_free(out);
    }
    return ok;
}

static bool metadata_revalidate(const char *repo_path, const artifact_metadata_t *meta) {
    if (!repo_path || !meta) {
        return false;
    }
    char relative[sizeof(CBM_ARTIFACT_DIR) + sizeof(CBM_ARTIFACT_META) + 2U];
    (void)snprintf(relative, sizeof(relative), "%s/%s", CBM_ARTIFACT_DIR, CBM_ARTIFACT_META);
    cbm_rooted_file_t file = {0};
    bool ok = cbm_rooted_file_read(repo_path, relative, ART_META_MAX_BYTES, &file) ==
                  CBM_ROOTED_FILE_OK &&
              strcmp(file.sha256, meta->metadata_sha256) == 0;
    cbm_rooted_file_free(&file);
    return ok;
}

static void payload_close(artifact_payload_t *payload) {
    if (!payload) {
        return;
    }
    if (payload->fp) {
        (void)fclose(payload->fp);
    }
    payload->fp = NULL;
#ifdef _WIN32
    free(payload->path);
    payload->path = NULL;
#endif
    artifact_dir_close(&payload->dir);
}

static bool payload_open(const char *repo_path, const artifact_metadata_t *meta,
                         artifact_payload_t *out) {
    if (!repo_path || !meta || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
#ifndef _WIN32
    out->dir.fd = -1;
#endif
    if (!artifact_dir_open(repo_path, false, &out->dir)) {
        return false;
    }
#ifdef _WIN32
    if (!artifact_dir_validate(&out->dir)) {
        payload_close(out);
        return false;
    }
    out->path = path_join2(out->dir.path, meta->payload);
    if (!out->path || cbm_path_is_reparse_point(out->path) ||
        !canonical_child_of(out->dir.path, out->path)) {
        payload_close(out);
        return false;
    }
    wchar_t *wide = cbm_utf8_to_wide(out->path);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                           FILE_FLAG_OPEN_REPARSE_POINT,
                                       NULL)
                         : INVALID_HANDLE_VALUE;
    free(wide);
    BY_HANDLE_FILE_INFORMATION info;
    char *opened_path = handle != INVALID_HANDLE_VALUE ? windows_handle_path(handle) : NULL;
    bool safe_handle =
        handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
        opened_path && canonical_child_of(out->dir.path, opened_path) &&
        artifact_dir_validate(&out->dir);
    free(opened_path);
    int fd = safe_handle ? _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY) : -1;
    if (!safe_handle || fd < 0) {
        if (handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle);
        }
        payload_close(out);
        return false;
    }
    out->fp = _fdopen(fd, "rb");
    if (!out->fp || fstat(cbm_fileno(out->fp), &out->initial) != 0 ||
        !S_ISREG(out->initial.st_mode)) {
        if (!out->fp) {
            (void)_close(fd);
        }
        payload_close(out);
        return false;
    }
    out->initial_info = info;
#else
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
    int fd = openat(out->dir.fd, meta->payload, flags);
    if (fd < 0 || fstat(fd, &out->initial) != 0 || !S_ISREG(out->initial.st_mode)) {
        if (fd >= 0) {
            (void)close(fd);
        }
        payload_close(out);
        return false;
    }
    out->fp = fdopen(fd, "rb");
    if (!out->fp) {
        (void)close(fd);
        payload_close(out);
        return false;
    }
#endif
    if (out->initial.st_size < 0 || (uint64_t)out->initial.st_size != meta->compressed_size ||
        (uint64_t)out->initial.st_size > ART_MAX_COMPRESSED_BYTES ||
        !run_test_hook("payload_opened")) {
        payload_close(out);
        return false;
    }
    return true;
}

static bool same_stat_generation(const struct stat *a, const struct stat *b) {
    if (!a || !b || a->st_size != b->st_size || a->st_mode != b->st_mode) {
        return false;
    }
#ifdef _WIN32
    return a->st_mtime == b->st_mtime;
#else
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino) {
        return false;
    }
#if defined(__APPLE__)
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
#endif
}

static bool payload_generation_stable(artifact_payload_t *payload,
                                      const artifact_metadata_t *meta) {
    if (!payload || !payload->fp || !meta) {
        return false;
    }
    struct stat after;
    if (fstat(cbm_fileno(payload->fp), &after) != 0 ||
        !same_stat_generation(&payload->initial, &after)) {
        return false;
    }
#ifdef _WIN32
    if (!artifact_dir_validate(&payload->dir)) {
        return false;
    }
    HANDLE opened = (HANDLE)_get_osfhandle(cbm_fileno(payload->fp));
    BY_HANDLE_FILE_INFORMATION after_info;
    char *opened_path = opened != INVALID_HANDLE_VALUE ? windows_handle_path(opened) : NULL;
    bool opened_stable = opened != INVALID_HANDLE_VALUE &&
                         GetFileInformationByHandle(opened, &after_info) &&
                         windows_file_info_same(&payload->initial_info, &after_info) &&
                         opened_path && canonical_child_of(payload->dir.path, opened_path);
    free(opened_path);
    if (!opened_stable || !payload->path) {
        return false;
    }
    wchar_t *wide = cbm_utf8_to_wide(payload->path);
    HANDLE live = wide ? CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    BY_HANDLE_FILE_INFORMATION live_info;
    char *live_path = live != INVALID_HANDLE_VALUE ? windows_handle_path(live) : NULL;
    bool ok =
        live != INVALID_HANDLE_VALUE && GetFileType(live) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(live, &live_info) &&
        !(live_info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
        windows_file_info_same(&after_info, &live_info) && live_path &&
        canonical_child_of(payload->dir.path, live_path) && artifact_dir_validate(&payload->dir);
    free(live_path);
    if (live != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(live);
    }
    return ok;
#else
    struct stat live;
    return fstatat(payload->dir.fd, meta->payload, &live, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(live.st_mode) && same_stat_generation(&after, &live);
#endif
}

static bool hash_payload(artifact_payload_t *payload, const artifact_metadata_t *meta,
                         char out_hash[CBM_SHA256_HEX_LEN + 1], uint64_t *out_size) {
    unsigned char *buffer = malloc(ART_IO_BUFFER);
    if (!buffer) {
        return false;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        size_t n = fread(buffer, 1, ART_IO_BUFFER, payload->fp);
        if (n > UINT64_MAX - total || total + (uint64_t)n > ART_MAX_COMPRESSED_BYTES) {
            ok = false;
            break;
        }
        total += (uint64_t)n;
        cbm_sha256_update(&hash, buffer, n);
        if (n < ART_IO_BUFFER) {
            if (ferror(payload->fp) || !feof(payload->fp)) {
                ok = false;
            }
            break;
        }
    }
    free(buffer);
    if (!ok || total != meta->compressed_size || !payload_generation_stable(payload, meta)) {
        return false;
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out_hash[i * 2U] = hex[digest[i] >> 4U];
        out_hash[i * 2U + 1U] = hex[digest[i] & 15U];
    }
    out_hash[CBM_SHA256_HEX_LEN] = '\0';
    if (out_size) {
        *out_size = total;
    }
    return true;
}

static bool validate_artifact_pair(const char *repo_path, artifact_metadata_t *meta) {
    artifact_payload_t payload;
    if (!payload_open(repo_path, meta, &payload)) {
        return false;
    }
    char hash[CBM_SHA256_HEX_LEN + 1];
    uint64_t size = 0;
    bool ok = hash_payload(&payload, meta, hash, &size) && size == meta->compressed_size &&
              strcmp(hash, meta->compressed_sha256) == 0;
    payload_close(&payload);
    return ok && metadata_revalidate(repo_path, meta);
}

bool cbm_artifact_repo_path_is_shell_safe(const char *repo_path) {
    return repo_path && repo_path[0] != '\0';
}

static bool git_head_hash(const char *repo_path, char out[CBM_SHA256_HEX_LEN + 1]) {
    out[0] = '\0';
    char git_bin[CBM_SZ_4K];
    if (!cbm_git_resolve_binary(git_bin, sizeof(git_bin))) {
        return false;
    }
    const char *argv[] = {git_bin, "-C", repo_path, "rev-parse", "--verify", "HEAD", NULL};
    cbm_proc_opts_t opts = {.bin = git_bin, .argv = argv, .discard_stderr = true};
    char *data = NULL;
    size_t len = 0;
    char ignored_hash[CBM_SHA256_HEX_LEN + 1];
    cbm_proc_result_t result;
    if (cbm_subprocess_capture(&opts, ART_GIT_OUTPUT_MAX, &data, &len, ignored_hash, &result) !=
        0) {
        free(data);
        return false;
    }
    while (len && (data[len - 1U] == '\n' || data[len - 1U] == '\r')) {
        data[--len] = '\0';
    }
    bool ok = len == 40U || len == 64U;
    for (size_t i = 0; ok && i < len; i++) {
        ok = isxdigit((unsigned char)data[i]) != 0;
    }
    if (ok) {
        memcpy(out, data, len + 1U);
    }
    free(data);
    return ok;
}

static void configure_merge_driver(const char *repo_path) {
    char git_bin[CBM_SZ_4K];
    if (!cbm_git_resolve_binary(git_bin, sizeof(git_bin))) {
        return;
    }
    const char *argv[] = {git_bin, "-C", repo_path, "config", "merge.ours.driver", "true", NULL};
    cbm_proc_opts_t opts = {.bin = git_bin, .argv = argv, .discard_stderr = true};
    cbm_proc_result_t result;
    (void)cbm_subprocess_run(&opts, &result);
}

static bool attributes_has_line(const char *data, size_t len, const char *line) {
    size_t wanted = strlen(line);
    const char *cursor = data;
    const char *end = data + len;
    while (cursor < end) {
        const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        const char *line_end = newline ? newline : end;
        if (line_end > cursor && line_end[-1] == '\r')
            line_end--;
        if ((size_t)(line_end - cursor) == wanted && memcmp(cursor, line, wanted) == 0)
            return true;
        cursor = newline ? newline + 1 : end;
    }
    return false;
}

static bool ensure_gitattributes_fd(int fd) {
    static const char header[] =
        "# Auto-generated by codebase-memory-mcp\n"
        "# Prevent merge conflicts on the manifest and compressed generations\n";
    static const char meta_rule[] = CBM_ARTIFACT_META " -diff merge=ours";
    static const char payload_rule[] = CBM_ARTIFACT_PATTERN " binary merge=ours";
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > ART_META_MAX_BYTES)
        return false;
#ifndef _WIN32
    if (st.st_nlink != 1)
        return false;
#endif
    FILE *fp = fdopen(fd, "r+b");
    if (!fp) {
#ifdef _WIN32
        (void)_close(fd);
#else
        (void)close(fd);
#endif
        return false;
    }
    size_t len = (size_t)st.st_size;
    char *existing = malloc(len + 1U);
    bool ok = existing && (len == 0 || fread(existing, 1, len, fp) == len);
    if (ok)
        existing[len] = '\0';
    bool need_meta = ok && !attributes_has_line(existing, len, meta_rule);
    bool need_payload = ok && !attributes_has_line(existing, len, payload_rule);
    if (ok && (need_meta || need_payload)) {
        ok = fseek(fp, 0, SEEK_END) == 0;
        if (ok && len == 0)
            ok = fwrite(header, 1, sizeof(header) - 1U, fp) == sizeof(header) - 1U;
        else if (ok && existing[len - 1U] != '\n')
            ok = fputc('\n', fp) != EOF;
        if (ok && need_meta)
            ok = fprintf(fp, "%s\n", meta_rule) > 0;
        if (ok && need_payload)
            ok = fprintf(fp, "%s\n", payload_rule) > 0;
        if (ok)
            ok = sync_file(fp);
    }
    free(existing);
    if (fclose(fp) != 0)
        ok = false;
    return ok;
}

static void ensure_gitattributes(const artifact_dir_t *dir, const char *repo_path) {
#ifdef _WIN32
    char *path = path_join2(dir->path, ".gitattributes");
    if (path) {
        int fd = open_exclusive_file(path, ART_FILE_PERMS);
        if (fd < 0 && !cbm_path_is_reparse_point(path)) {
            wchar_t *wide = cbm_utf8_to_wide(path);
            HANDLE handle =
                wide ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                     : INVALID_HANDLE_VALUE;
            free(wide);
            BY_HANDLE_FILE_INFORMATION info;
            char *opened = handle != INVALID_HANDLE_VALUE ? windows_handle_path(handle) : NULL;
            bool safe =
                handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(handle, &info) &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                           FILE_ATTRIBUTE_DEVICE)) &&
                info.nNumberOfLinks == 1 && opened && canonical_child_of(dir->path, opened);
            free(opened);
            if (safe)
                fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
            if (!safe || fd < 0) {
                if (handle != INVALID_HANDLE_VALUE)
                    CloseHandle(handle);
                fd = -1;
            }
        }
        if (fd >= 0)
            (void)ensure_gitattributes_fd(fd); /* consumes fd */
        free(path);
    }
#else
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = openat(dir->fd, ".gitattributes", flags, ART_FILE_PERMS);
    if (fd < 0 && errno == EEXIST) {
        int existing_flags = O_RDWR;
#ifdef O_CLOEXEC
        existing_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        existing_flags |= O_NOFOLLOW;
#endif
        fd = openat(dir->fd, ".gitattributes", existing_flags);
    }
    if (fd >= 0 && ensure_gitattributes_fd(fd))
        (void)sync_directory(dir);
#endif
    configure_merge_driver(repo_path);
}

static bool publish_payload(const artifact_dir_t *dir, const char *temp_path,
                            const char *payload_name, uint64_t size, const char *sha) {
#ifdef _WIN32
    if (!artifact_dir_validate(dir)) {
        return false;
    }
    char *dest = path_join2(dir->path, payload_name);
    if (!dest) {
        return false;
    }
    wchar_t *wtemp = cbm_utf8_to_wide(temp_path);
    wchar_t *wdest = cbm_utf8_to_wide(dest);
    bool created = wtemp && wdest && MoveFileExW(wtemp, wdest, MOVEFILE_WRITE_THROUGH) != 0;
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    free(wtemp);
    free(wdest);
    if (!artifact_dir_validate(dir) ||
        (!created && error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)) {
        free(dest);
        return false;
    }
    char hash[CBM_SHA256_HEX_LEN + 1];
    int64_t published_size = cbm_file_size(dest);
    bool ok = !cbm_path_is_reparse_point(dest) && published_size >= 0 &&
              (uint64_t)published_size == size &&
              cbm_sha256_file_hex_limited(dest, (size_t)(size <= SIZE_MAX ? size : SIZE_MAX),
                                          hash) == CBM_SHA256_FILE_HASHED &&
              strcmp(hash, sha) == 0 && artifact_dir_validate(dir);
    free(dest);
    return ok;
#else
    if (linkat(AT_FDCWD, temp_path, dir->fd, payload_name, 0) != 0 && errno != EEXIST) {
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = openat(dir->fd, payload_name, flags);
    if (fd < 0) {
        return false;
    }
    struct stat before;
    bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
              (uint64_t)before.st_size == size;
    FILE *fp = ok ? fdopen(fd, "rb") : NULL;
    if (!fp) {
        (void)close(fd);
        return false;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    unsigned char buffer[ART_IO_BUFFER];
    uint64_t total = 0;
    while (ok) {
        size_t n = fread(buffer, 1, sizeof(buffer), fp);
        if (n > UINT64_MAX - total) {
            ok = false;
            break;
        }
        total += (uint64_t)n;
        cbm_sha256_update(&hash, buffer, n);
        if (n < sizeof(buffer)) {
            ok = feof(fp) && !ferror(fp);
            break;
        }
    }
    struct stat after;
    struct stat live;
    ok = ok && fstat(cbm_fileno(fp), &after) == 0 && same_stat_generation(&before, &after) &&
         fstatat(dir->fd, payload_name, &live, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(live.st_mode) &&
         same_stat_generation(&after, &live);
    (void)fclose(fp);
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    char actual[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        actual[i * 2U] = hex[digest[i] >> 4U];
        actual[i * 2U + 1U] = hex[digest[i] & 15U];
    }
    actual[CBM_SHA256_HEX_LEN] = '\0';
    return ok && total == size && strcmp(actual, sha) == 0 && sync_directory(dir);
#endif
}

static void iso_timestamp(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_value;
    if (!cbm_gmtime_r(&now, &tm_value) || strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_value) == 0) {
        out[0] = '\0';
    }
}

static bool write_metadata_atomic(const artifact_dir_t *dir, const char *repo_path,
                                  const char *project, const char *payload, const char *sha,
                                  uint64_t original_size, uint64_t compressed_size, int level,
                                  int nodes, int edges) {
    char commit[CBM_SHA256_HEX_LEN + 1] = "";
    (void)git_head_hash(repo_path, commit);
    char timestamp[32];
    iso_timestamp(timestamp);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return false;
    }
    yyjson_mut_doc_set_root(doc, root);
    bool encoded =
        yyjson_mut_obj_add_uint(doc, root, "schema_version", CBM_ARTIFACT_SCHEMA_VERSION) &&
        yyjson_mut_obj_add_str(doc, root, "payload", payload) &&
        yyjson_mut_obj_add_str(doc, root, "generation", sha) &&
        yyjson_mut_obj_add_str(doc, root, "compressed_sha256", sha) &&
        yyjson_mut_obj_add_uint(doc, root, "original_size", original_size) &&
        yyjson_mut_obj_add_uint(doc, root, "compressed_size", compressed_size) &&
        yyjson_mut_obj_add_int(doc, root, "compression_level", level) &&
        yyjson_mut_obj_add_str(doc, root, "commit", commit) &&
        yyjson_mut_obj_add_str(doc, root, "indexed_at", timestamp) &&
        yyjson_mut_obj_add_str(doc, root, "project", project) &&
        yyjson_mut_obj_add_int(doc, root, "nodes", nodes) &&
        yyjson_mut_obj_add_int(doc, root, "edges", edges);
    size_t json_len = 0;
    char *json = encoded ? yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &json_len) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json || json_len == 0 || json_len > ART_META_MAX_BYTES) {
        free(json);
        return false;
    }

    bool ok = false;
#ifdef _WIN32
    char *template_path = path_join2(dir->path, ".artifact.json-XXXXXX");
    char *dest = path_join2(dir->path, CBM_ARTIFACT_META);
    int fd = artifact_dir_validate(dir) && template_path ? cbm_mkstemp(template_path) : -1;
    FILE *fp = fd >= 0 ? fdopen(fd, "w+b") : NULL;
    ok = fp && fwrite(json, 1, json_len, fp) == json_len && sync_file(fp);
    if (fp) {
        ok = fclose(fp) == 0 && ok;
    } else if (fd >= 0) {
        (void)_close(fd);
    }
    if (ok) {
        ok = artifact_dir_validate(dir) && cbm_rename_replace(template_path, dest) == 0 &&
             artifact_dir_validate(dir);
    }
    if (!ok && template_path && artifact_dir_validate(dir)) {
        (void)cbm_unlink(template_path);
    }
    free(template_path);
    free(dest);
#else
    char temp_name[96] = "";
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 256U && fd < 0; attempt++) {
        int n = snprintf(temp_name, sizeof(temp_name), ".artifact.json-%ld-%" PRIu64 "-%u",
                         (long)getpid(), cbm_now_ns(), attempt);
        if (n <= 0 || (size_t)n >= sizeof(temp_name)) {
            break;
        }
        int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd = openat(dir->fd, temp_name, flags, ART_PRIVATE_FILE_PERMS);
        if (fd < 0 && errno != EEXIST) {
            break;
        }
    }
    FILE *fp = fd >= 0 ? fdopen(fd, "w+b") : NULL;
    ok = fp && fwrite(json, 1, json_len, fp) == json_len && sync_file(fp) &&
         fchmod(fd, ART_FILE_PERMS) == 0;
    if (fp) {
        ok = fclose(fp) == 0 && ok;
    } else if (fd >= 0) {
        (void)close(fd);
    }
    if (ok) {
        ok = renameat(dir->fd, temp_name, dir->fd, CBM_ARTIFACT_META) == 0 && sync_directory(dir);
    }
    if (!ok && temp_name[0]) {
        (void)unlinkat(dir->fd, temp_name, 0);
    }
#endif
    free(json);
    return ok;
}

int cbm_artifact_export(const char *db_path, const char *repo_path, const char *project_name,
                        int quality) {
    g_export_error[0] = '\0';
    if (!db_path || !db_path[0] || !repo_path || !repo_path[0] || !project_name ||
        !project_name[0] || (quality != CBM_ARTIFACT_FAST && quality != CBM_ARTIFACT_BEST)) {
        return export_fail("validate_args", NULL, "invalid_argument", 0);
    }
    artifact_dir_t dir;
    if (!artifact_dir_open(repo_path, true, &dir)) {
        return export_fail("prepare_artifact_dir", repo_path, "unsafe_or_unavailable", errno);
    }
    char *temp_dir = NULL;
    if (!make_private_temp_dir(&dir, &temp_dir)) {
        artifact_dir_close(&dir);
        return export_fail("prepare_temp_dir", repo_path, "exclusive_create_failed", errno);
    }
    char *snapshot = path_join2(temp_dir, "snapshot.db");
    char *compressed = path_join2(temp_dir, "payload.zst");
    if (!snapshot || !compressed) {
        cleanup_private_temp_dir(&dir, temp_dir, snapshot, compressed);
        free(snapshot);
        free(compressed);
        artifact_dir_close(&dir);
        return export_fail("prepare_paths", repo_path, "path_overflow_or_oom", 0);
    }
    int result = CBM_NOT_FOUND;
    if (!sqlite_backup_snapshot(db_path, snapshot)) {
        export_fail("snapshot", db_path, "sqlite_backup_failed", 0);
        goto done;
    }
    if (quality == CBM_ARTIFACT_BEST && !strip_snapshot_indexes(snapshot)) {
        export_fail("strip_indexes", snapshot, "checked_strip_or_vacuum_failed", 0);
        goto done;
    }
    int nodes = 0;
    int edges = 0;
    if (!snapshot_integrity_and_counts(snapshot, project_name, &nodes, &edges)) {
        export_fail("validate_snapshot", snapshot, "integrity_or_count_failed", 0);
        goto done;
    }
    FILE *source = cbm_fopen(snapshot, "rb");
    int output_fd = open_exclusive_file(compressed, ART_PRIVATE_FILE_PERMS);
    FILE *output = output_fd >= 0 ? fdopen(output_fd, "w+b") : NULL;
    if (!source || !output) {
        if (source) {
            (void)fclose(source);
        }
        if (output) {
            (void)fclose(output);
        } else if (output_fd >= 0) {
#ifdef _WIN32
            (void)_close(output_fd);
#else
            (void)close(output_fd);
#endif
        }
        export_fail("compress", compressed, "exclusive_open_failed", errno);
        goto done;
    }
    uint64_t original_size = 0;
    uint64_t compressed_size = 0;
    char compressed_sha[CBM_SHA256_HEX_LEN + 1];
    int level = quality == CBM_ARTIFACT_BEST ? ART_ZSTD_BEST : ART_ZSTD_FAST;
    bool compressed_ok = cbm_zstd_compress_file(source, output, level, ART_MAX_DECOMPRESSED_BYTES,
                                                ART_MAX_COMPRESSED_BYTES, &original_size,
                                                &compressed_size, compressed_sha) == 0 &&
                         sync_file(output);
    int source_close_rc = fclose(source);
    int output_close_rc = fclose(output);
    if (source_close_rc != 0 || output_close_rc != 0) {
        compressed_ok = false;
    }
    if (!compressed_ok || original_size == 0 || compressed_size == 0) {
        export_fail("compress", snapshot, "stream_or_limit_failed", 0);
        goto done;
    }
#ifndef _WIN32
    if (chmod(compressed, ART_FILE_PERMS) != 0) {
        export_fail("compress", compressed, "chmod_failed", errno);
        goto done;
    }
#endif
    char payload_name[sizeof(ART_PAYLOAD_PREFIX) + CBM_SHA256_HEX_LEN + sizeof(ART_PAYLOAD_SUFFIX)];
    int pn = snprintf(payload_name, sizeof(payload_name), "%s%s%s", ART_PAYLOAD_PREFIX,
                      compressed_sha, ART_PAYLOAD_SUFFIX);
    if (pn <= 0 || (size_t)pn >= sizeof(payload_name) ||
        !publish_payload(&dir, compressed, payload_name, compressed_size, compressed_sha)) {
        export_fail("publish_payload", dir.path, "create_or_verify_failed", errno);
        goto done;
    }
    if (!run_test_hook("before_metadata_publish")) {
        export_fail("write_metadata", dir.path, "injected_before_publish", 0);
        goto done;
    }
    if (!write_metadata_atomic(&dir, repo_path, project_name, payload_name, compressed_sha,
                               original_size, compressed_size, level, nodes, edges)) {
        /* The previous metadata and its immutable payload remain untouched.
         * A newly-published unreferenced payload is a harmless collectible orphan. */
        export_fail("write_metadata", dir.path, "atomic_publish_failed", errno);
        goto done;
    }
    if (artifact_dir_validate(&dir)) {
        ensure_gitattributes(&dir, repo_path);
    }
    cbm_log_info("artifact.export", "quality", quality == CBM_ARTIFACT_BEST ? "best" : "fast",
                 "payload", payload_name);
    result = 0;

done:
    cleanup_private_temp_dir(&dir, temp_dir, snapshot, compressed);
    free(snapshot);
    free(compressed);
    artifact_dir_close(&dir);
    return result;
}

static bool ensure_parent_directory(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    char *copy = cbm_strdup(path);
    if (!copy) {
        return false;
    }
    char *slash = strrchr(copy, '/');
#ifdef _WIN32
    char *backslash = strrchr(copy, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif
    bool ok = true;
    if (slash) {
        if (slash == copy) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        ok = cbm_mkdir_p(copy, ART_DIR_PERMS) && cbm_is_dir(copy);
    }
    free(copy);
    return ok;
}

static char *make_import_temp(const char *dest, int *out_fd) {
    *out_fd = -1;
    char *path = path_suffix(dest, ".import-XXXXXX");
    if (!path) {
        return NULL;
    }
    int fd = cbm_mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
#ifndef _WIN32
    if (fchmod(fd, ART_PRIVATE_FILE_PERMS) != 0) {
        (void)close(fd);
        (void)cbm_unlink(path);
        free(path);
        return NULL;
    }
#endif
    *out_fd = fd;
    return path;
}

int cbm_artifact_import(const char *repo_path, const char *cache_db_path) {
    if (!repo_path || !repo_path[0] || !cache_db_path || !cache_db_path[0]) {
        return CBM_NOT_FOUND;
    }
    artifact_metadata_t meta;
    if (!metadata_read(repo_path, &meta)) {
        cbm_log_info("artifact.import", "skip", "invalid_or_legacy_metadata");
        return CBM_NOT_FOUND;
    }
    artifact_payload_t payload;
    if (!payload_open(repo_path, &meta, &payload) || !ensure_parent_directory(cache_db_path)) {
        payload_close(&payload);
        metadata_free(&meta);
        return CBM_NOT_FOUND;
    }
    int temp_fd = -1;
    char *temp_path = make_import_temp(cache_db_path, &temp_fd);
    FILE *dest = temp_fd >= 0 ? fdopen(temp_fd, "w+b") : NULL;
    if (!temp_path || !dest) {
        if (dest) {
            (void)fclose(dest);
        } else if (temp_fd >= 0) {
#ifdef _WIN32
            (void)_close(temp_fd);
#else
            (void)close(temp_fd);
#endif
        }
        free(temp_path);
        payload_close(&payload);
        metadata_free(&meta);
        return CBM_NOT_FOUND;
    }
    uint64_t compressed_size = 0;
    uint64_t decompressed_size = 0;
    char compressed_sha[CBM_SHA256_HEX_LEN + 1];
    bool ok = cbm_zstd_decompress_file(payload.fp, dest, ART_MAX_COMPRESSED_BYTES,
                                       ART_MAX_DECOMPRESSED_BYTES, &compressed_size,
                                       &decompressed_size, compressed_sha) == 0 &&
              compressed_size == meta.compressed_size && decompressed_size == meta.original_size &&
              strcmp(compressed_sha, meta.compressed_sha256) == 0 && sync_file(dest) &&
              payload_generation_stable(&payload, &meta) && metadata_revalidate(repo_path, &meta);
    payload_close(&payload);
    if (fclose(dest) != 0) {
        ok = false;
    }
    if (ok) {
        cbm_store_t *store = cbm_store_open_path(temp_path);
        ok = store && cbm_store_check_integrity_deep(store);
        cbm_store_close(store);
    }
    if (ok) {
        /* Never unlink/rename cache_db_path or delete its WAL/SHM. SQLite's
         * backup transaction publishes coherently to live readers. */
        ok = cbm_store_install_snapshot_file(temp_path, cache_db_path) == CBM_STORE_OK;
    }
    unlink_temp_db(temp_path);
    free(temp_path);
    metadata_free(&meta);
    if (!ok) {
        cbm_log_error("artifact.import", "err", "verification_or_install_failed");
        return CBM_NOT_FOUND;
    }
    cbm_log_info("artifact.import", "db", cache_db_path);
    return 0;
}

bool cbm_artifact_exists(const char *repo_path) {
    if (!repo_path || !repo_path[0]) {
        return false;
    }
    artifact_metadata_t meta;
    if (!metadata_read(repo_path, &meta)) {
        return false;
    }
    bool ok = validate_artifact_pair(repo_path, &meta);
    metadata_free(&meta);
    return ok;
}

char *cbm_artifact_commit(const char *repo_path) {
    if (!repo_path || !repo_path[0]) {
        return NULL;
    }
    artifact_metadata_t meta;
    if (!metadata_read(repo_path, &meta)) {
        return NULL;
    }
    char *result = NULL;
    if (meta.commit && validate_artifact_pair(repo_path, &meta)) {
        result = cbm_strdup(meta.commit);
    }
    metadata_free(&meta);
    return result;
}
