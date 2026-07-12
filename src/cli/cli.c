/*
 * cli.c — CLI subcommand handlers for install, uninstall, update, version.
 *
 * Port of Go cmd/codebase-memory-mcp/ install/update logic.
 * All functions accept explicit paths for testability.
 */
#include "cli/cli.h"
#include "foundation/compat.h"
#include "foundation/platform.h"
#include "foundation/constants.h"
#include "foundation/product.h"
#include "foundation/sha256.h"
#include "foundation/subprocess.h"
#include "mcp/mcp.h" // cbm_mcp_tool_input_schema — CLI flag parser + per-tool --help

/* CLI buffer size constants. */
enum {
    CLI_BUF_1K = 1024,
    CLI_BUF_512 = 512,
    CLI_BUF_256 = 256,
    CLI_BUF_128 = 128,
    CLI_BUF_4K = 4096,
    CLI_BUF_16 = 16,
    CLI_BUF_8 = 8,
    CLI_BUF_24 = 24,
    CLI_SKIP_ONE = 1,
    CLI_PAIR_LEN = 2,
    CLI_OCTAL_PERM = 0755,
    CLI_JSON_INDENT = 3,
    CLI_MAX_SCAN = 10,
    CLI_ERR = -1,
    CLI_OK = 0,
    CLI_TRUE = 1,
    CLI_ELEM_SIZE = 1,    /* fread/fwrite element size */
    CLI_IDX_1 = 1,        /* array index 1 */
    CLI_IDX_2 = 2,        /* array index 2 */
    CLI_STRTOL_BASE = 10, /* decimal base for strtol */
    CLI_STRTOL_HEX = 16,  /* hex base for strtol */
    CLI_BUF_2K = 2048,
    CLI_BUF_8K = 8192,
    CLI_BUF_32 = 32,
    CLI_INDENT_24 = 24,
    CLI_FIELD_1040 = 1040,
    CLI_MB_10 = 10,
    BYTE_SHIFT = 8,    /* bits per byte for multi-byte reads */
    SQL_NUL_TERM = -1, /* sqlite3 length = -1 means NUL-terminated */
    SQL_PARAM_1 = 1,   /* sqlite3_bind parameter index 1 */
    SQL_PARAM_2 = 2,
    SEMVER_PARTS = 3, /* major.minor.patch */
    DB_EXT_LEN = 3,   /* strlen(".db") */
    MIN_ARGC_CMD = 3,
    /* minimum argc for subcommand with arg */ /* sqlite3_bind parameter index 2 */ /* 10 MB cap
                                                                                       factor */
    CLI_MB_FACTOR = CLI_BUF_1K * CLI_BUF_1K,
    NUM_RETRIES = 5,
    NUM_DIRS = 4,
    DECOMP_FACTOR = 10,
    GROWTH_FACTOR = 2,
    MIN_ARGC_GET = 2,
    AUTO_YES = 1,
    AUTO_NO = -1,
    VARIANT_A = 1,
    VARIANT_B = 2,
    OCTAL_BASE = 8,
};

/* String length helper for strncmp. */
#define SLEN(s) (sizeof(s) - SKIP_ONE)

// the correct standard headers are included below but clang-tidy doesn't map them.
#include <ctype.h>
#ifndef _WIN32
#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#else
#include "foundation/win_utf8.h"
#include <io.h>
#include <tlhelp32.h>
#include <windows.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#include "foundation/compat_fs.h"

#ifndef CBM_VERSION
#define CBM_VERSION "dev"
#endif
#include <errno.h>  // EEXIST
#include <fcntl.h>  // open, O_WRONLY, O_CREAT, O_TRUNC
#include <limits.h> // UINT_MAX
#include <stdint.h> // uintptr_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   // strtok_r
#include <sys/stat.h> // mode_t, S_IXUSR
#include <zlib.h>     // MAX_WBITS

/* yyjson for JSON read-modify-write */
#include "yyjson/yyjson.h"

/* SQLITE_TRANSIENT equivalent as a typed function pointer (avoids int-to-ptr cast).
 * sqlite3.h defines SQLITE_TRANSIENT as ((sqlite3_destructor_type)-1).
 * We replicate the same bit pattern via memcpy to satisfy performance-no-int-to-ptr. */
static void (*cbm_sqlite_transient_fn(void))(void *) {
    uintptr_t bits = (uintptr_t)CLI_ERR;
    void (*fp)(void *) = NULL;
    memcpy(&fp, &bits, sizeof(fp));
    return fp;
}
#define cbm_sqlite_transient (cbm_sqlite_transient_fn())

/* ── Constants ────────────────────────────────────────────────── */

/* Directory permissions: rwxr-x--- */
#define DIR_PERMS 0750

/* Decompression buffer cap (500 MB) */
#define DECOMPRESS_MAX_BYTES ((size_t)500 * CLI_BUF_1K * CBM_SZ_1K)

/* Tar header field offsets */
#define TAR_NAME_LEN 101    /* filename field: bytes 0-99 + NUL */
#define TAR_SIZE_OFFSET 124 /* octal size field offset */
#define TAR_SIZE_LEN 13     /* octal size field: bytes 124-135 + NUL */
#define TAR_TYPE_OFFSET 156 /* type flag byte */
#define TAR_BINARY_NAME "codebase-memory-mcp"
#define TAR_BINARY_NAME_LEN 19
#define TAR_BLOCK_SIZE CBM_SZ_512 /* tar record alignment */
#define TAR_BLOCK_MASK 511        /* TAR_BLOCK_SIZE - 1 */

/* ── Version ──────────────────────────────────────────────────── */

static const char *cli_version = "dev";

void cbm_cli_set_version(const char *ver) {
    if (ver) {
        cli_version = ver;
    }
}

const char *cbm_cli_get_version(void) {
    return cli_version;
}

/* ── Version comparison ───────────────────────────────────────── */

/* Parse semver major.minor.patch into array. Returns number of parts parsed. */
static int parse_semver(const char *v, int out[SEMVER_PARTS]) {
    out[0] = out[CLI_IDX_1] = out[CLI_IDX_2] = 0;
    /* Skip v prefix */
    if (*v == 'v' || *v == 'V') {
        v++;
    }

    int count = 0;
    while (*v && count < SEMVER_PARTS) {
        if (*v == '-') {
            break; /* stop at pre-release suffix */
        }
        char *endptr;
        long val = strtol(v, &endptr, CLI_STRTOL_BASE);
        out[count++] = (int)val;
        if (*endptr == '.') {
            v = endptr + CLI_SKIP_ONE;
        } else {
            break;
        }
    }
    return count;
}

static const char *prerelease_suffix(const char *v) {
    const char *dash = strchr(v, '-');
    const char *build = strchr(v, '+');
    if (!dash || (build && dash > build)) {
        return NULL;
    }
    return dash + CLI_SKIP_ONE;
}

static bool identifier_is_numeric(const char *s, size_t len) {
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* Compare SemVer prerelease identifiers without allocating or overflowing.
 * Numeric identifiers compare numerically, numeric identifiers sort before
 * text identifiers, and a longer equal prefix has higher precedence. */
static int compare_prerelease(const char *a, const char *b) {
    const char *ap = prerelease_suffix(a);
    const char *bp = prerelease_suffix(b);
    if (!ap && !bp) {
        return 0;
    }
    if (!ap) {
        return CLI_TRUE; /* a stable release beats a prerelease */
    }
    if (!bp) {
        return CLI_ERR;
    }

    for (;;) {
        const char *ae = ap;
        const char *be = bp;
        while (*ae && *ae != '.' && *ae != '+') {
            ae++;
        }
        while (*be && *be != '.' && *be != '+') {
            be++;
        }
        size_t alen = (size_t)(ae - ap);
        size_t blen = (size_t)(be - bp);
        bool anum = identifier_is_numeric(ap, alen);
        bool bnum = identifier_is_numeric(bp, blen);
        int cmp = 0;
        if (anum && bnum) {
            while (alen > CLI_SKIP_ONE && *ap == '0') {
                ap++;
                alen--;
            }
            while (blen > CLI_SKIP_ONE && *bp == '0') {
                bp++;
                blen--;
            }
            if (alen != blen) {
                cmp = alen < blen ? CLI_ERR : CLI_TRUE;
            } else {
                cmp = memcmp(ap, bp, alen);
            }
        } else if (anum != bnum) {
            cmp = anum ? CLI_ERR : CLI_TRUE;
        } else {
            size_t min_len = alen < blen ? alen : blen;
            cmp = memcmp(ap, bp, min_len);
            if (cmp == 0 && alen != blen) {
                cmp = alen < blen ? CLI_ERR : CLI_TRUE;
            }
        }
        if (cmp != 0) {
            return cmp;
        }

        bool a_done = (*ae == '\0' || *ae == '+');
        bool b_done = (*be == '\0' || *be == '+');
        if (a_done || b_done) {
            if (a_done && b_done) {
                return 0;
            }
            return a_done ? CLI_ERR : CLI_TRUE;
        }
        ap = ae + CLI_SKIP_ONE;
        bp = be + CLI_SKIP_ONE;
    }
}

int cbm_compare_versions(const char *a, const char *b) {
    int pa[SEMVER_PARTS];
    int pb[SEMVER_PARTS];
    parse_semver(a, pa);
    parse_semver(b, pb);

    for (int i = 0; i < SEMVER_PARTS; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] - pb[i];
        }
    }

    return compare_prerelease(a, b);
}

/* ── Shell RC detection ───────────────────────────────────────── */

const char *cbm_detect_shell_rc(const char *home_dir) {
    static char buf[CLI_BUF_512];
    if (!home_dir || !home_dir[0]) {
        return "";
    }

    char shell_buf[CLI_BUF_256];
    const char *shell = cbm_safe_getenv("SHELL", shell_buf, sizeof(shell_buf), "");
    if (!shell) {
        shell = "";
    }

    if (strstr(shell, "/zsh")) {
        snprintf(buf, sizeof(buf), "%s/.zshrc", home_dir);
        return buf;
    }
    if (strstr(shell, "/bash")) {
        /* Prefer .bashrc, fall back to .bash_profile */
        snprintf(buf, sizeof(buf), "%s/.bashrc", home_dir);
        struct stat st;
        if (stat(buf, &st) == 0) {
            return buf;
        }
        snprintf(buf, sizeof(buf), "%s/.bash_profile", home_dir);
        return buf;
    }
    if (strstr(shell, "/fish")) {
        snprintf(buf, sizeof(buf), "%s/.config/fish/config.fish", home_dir);
        return buf;
    }

    /* Default to .profile */
    snprintf(buf, sizeof(buf), "%s/.profile", home_dir);
    return buf;
}

/* ── CLI binary detection ─────────────────────────────────────── */

/* PATH delimiter: `;` on Windows, `:` on POSIX. */
#ifdef _WIN32
#define PATH_DELIM ";"
#else
#define PATH_DELIM ":"
#endif

/* Check if a path exists and is executable.
 * On Windows, stat() doesn't set S_IXUSR — just check existence. */
static bool is_executable(const char *path) {
    struct stat st;
#ifdef _WIN32
    return stat(path, &st) == 0;
#else
    return stat(path, &st) == 0 && (st.st_mode & S_IXUSR);
#endif
}

/* Search for an executable named `name` in the PATH environment variable.
 * Returns the full path in `out` (max out_sz) if found, else empty string. */
static bool find_in_path(const char *name, char *out, size_t out_sz) {
    char path_copy[CLI_BUF_4K];
    if (!cbm_safe_getenv("PATH", path_copy, sizeof(path_copy), NULL)) {
        return false;
    }
    char *saveptr;
    char *dir = strtok_r(path_copy, PATH_DELIM, &saveptr);
    while (dir) {
        snprintf(out, out_sz, "%s/%s", dir, name);
        if (is_executable(out)) {
            return true;
        }
#ifdef _WIN32
        /* On Windows executables carry an extension (PATHEXT). A CLI like
         * opencode is often installed as a .cmd / .ps1 / .exe shim (e.g. via
         * mise or npm), so the bare-name probe above misses it (#221). Try the
         * common executable extensions before moving to the next PATH entry. */
        static const char *const win_exts[] = {".exe", ".cmd", ".bat", ".ps1", NULL};
        for (int i = 0; win_exts[i]; i++) {
            snprintf(out, out_sz, "%s/%s%s", dir, name, win_exts[i]);
            if (is_executable(out)) {
                return true;
            }
        }
#endif
        dir = strtok_r(NULL, PATH_DELIM, &saveptr);
    }
    return false;
}

const char *cbm_find_cli(const char *name, const char *home_dir) {
    static char buf[CLI_BUF_512];
    if (!name || !name[0]) {
        return "";
    }
    if (find_in_path(name, buf, sizeof(buf))) {
        return buf;
    }
    if (!home_dir || !home_dir[0]) {
        return "";
    }
    enum { NUM_PATHS = 5 };
    char paths[NUM_PATHS][CLI_BUF_512];
    snprintf(paths[0], sizeof(paths[0]), "/usr/local/bin/%s", name);
    snprintf(paths[1], sizeof(paths[1]), "%s/.npm/bin/%s", home_dir, name);
    snprintf(paths[2], sizeof(paths[2]), "%s/.local/bin/%s", home_dir, name);
    snprintf(paths[3], sizeof(paths[3]), "%s/.cargo/bin/%s", home_dir, name);
#ifdef __APPLE__
    snprintf(paths[4], sizeof(paths[4]), "/opt/homebrew/bin/%s", name);
#else
    paths[4][0] = '\0';
#endif
    for (int i = 0; i < NUM_RETRIES; i++) {
        if (paths[i][0] && is_executable(paths[i])) {
            snprintf(buf, sizeof(buf), "%s", paths[i]);
            return buf;
        }
    }
    return "";
}

/* ── File utilities ───────────────────────────────────────────── */

int cbm_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return CLI_ERR;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        (void)fclose(in);
        return CLI_ERR;
    }

    char buf[CLI_BUF_8K];
    int err = 0;
    while (!feof(in) && !ferror(in)) {
        size_t n = fread(buf, CLI_ELEM_SIZE, sizeof(buf), in);
        if (n == 0) {
            break;
        }
        if (fwrite(buf, CLI_ELEM_SIZE, n, out) != n) {
            err = CLI_TRUE;
            break;
        }
    }

    if (err || ferror(in)) {
        (void)fclose(in);
        (void)fclose(out);
        return CLI_ERR;
    }

    (void)fclose(in);
    int rc = fclose(out);
    return rc == 0 ? 0 : CLI_ERR;
}

/* Return true if two paths refer to the same on-disk file. Used to avoid
 * copying the running binary onto itself during install (cbm_copy_file would
 * truncate it, since it opens the destination "wb" before reading the source). */
static bool cbm_same_file(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
#ifdef _WIN32
    wchar_t *wide_a = cbm_utf8_to_wide(a);
    wchar_t *wide_b = cbm_utf8_to_wide(b);
    if (!wide_a || !wide_b) {
        free(wide_a);
        free(wide_b);
        return false;
    }
    DWORD dst_attrs = GetFileAttributesW(wide_b);
    if (dst_attrs == INVALID_FILE_ATTRIBUTES || (dst_attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wide_a);
        free(wide_b);
        return false; /* replacing a symlink/reparse point must replace the link itself */
    }
    HANDLE handle_a = CreateFileW(wide_a, FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE handle_b = CreateFileW(wide_b, FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide_a);
    free(wide_b);
    BY_HANDLE_FILE_INFORMATION info_a;
    BY_HANDLE_FILE_INFORMATION info_b;
    bool same = handle_a != INVALID_HANDLE_VALUE && handle_b != INVALID_HANDLE_VALUE &&
                GetFileType(handle_a) == FILE_TYPE_DISK &&
                GetFileType(handle_b) == FILE_TYPE_DISK &&
                GetFileInformationByHandle(handle_a, &info_a) &&
                GetFileInformationByHandle(handle_b, &info_b) &&
                !(info_a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(info_b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                info_a.dwVolumeSerialNumber == info_b.dwVolumeSerialNumber &&
                info_a.nFileIndexHigh == info_b.nFileIndexHigh &&
                info_a.nFileIndexLow == info_b.nFileIndexLow;
    if (handle_a != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_a);
    }
    if (handle_b != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_b);
    }
    return same;
#else
    struct stat sa;
    struct stat sb_link;
    struct stat sb;
    if (stat(a, &sa) != 0 || lstat(b, &sb_link) != 0 || S_ISLNK(sb_link.st_mode) ||
        stat(b, &sb) != 0) {
        return false;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

/* Copy the running binary into the canonical install target, preserving the
 * executable bit. When src and dst are the same on-disk file the copy is
 * skipped: cbm_copy_file opens dst "wb" before reading src, so copying a file
 * onto itself would truncate it to zero. Returns 0 on success or skip,
 * CLI_ERR on failure. Exposed (non-static) as the regression surface for the
 * `install --force` binary-swap bug (#472). */
int cbm_copy_binary_to_target(const char *src, const char *dst) {
    if (cbm_same_file(src, dst)) {
        return 0; /* already in place — nothing to copy */
    }
    char *data = NULL;
    size_t data_len = 0;
    char sha256[CBM_SHA256_HEX_LEN + 1];
    int read_rc = cbm_sha256_file_read_hex(src, DECOMPRESS_MAX_BYTES, &data, &data_len, sha256);
    if (read_rc != CBM_SHA256_FILE_HASHED || data_len == 0 || data_len > INT_MAX) {
        free(data);
        return CLI_ERR;
    }
    int rc = cbm_replace_binary(dst, (const unsigned char *)data, (int)data_len, CLI_OCTAL_PERM);
    free(data);
    return rc;
}

static char *cli_temp_template_for(const char *path, const char *suffix) {
    if (!path || !suffix) {
        return NULL;
    }
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > SIZE_MAX - CLI_SKIP_ONE || path_len > SIZE_MAX - suffix_len - CLI_SKIP_ONE) {
        return NULL;
    }
    char *result = malloc(path_len + suffix_len + CLI_SKIP_ONE);
    if (!result) {
        return NULL;
    }
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len + CLI_SKIP_ONE);
    return result;
}

static int cli_sync_fd(int fd) {
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

static int cli_close_fd(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

#ifndef _WIN32
static int cli_publish_binary_posix(const char *temporary, const char *path,
                                    const struct stat *expected_temporary) {
    if (!temporary || !path || !expected_temporary) {
        return CLI_ERR;
    }
    char *copy = strdup(path);
    if (!copy) {
        return CLI_ERR;
    }
    char *slash = strrchr(copy, '/');
    const char *parent = ".";
    const char *destination_name = copy;
    if (slash) {
        destination_name = slash + CLI_SKIP_ONE;
        if (slash == copy) {
            parent = "/";
        } else {
            *slash = '\0';
            parent = copy;
        }
    }
    const char *temporary_name = strrchr(temporary, '/');
    temporary_name = temporary_name ? temporary_name + CLI_SKIP_ONE : temporary;
    if (!destination_name[0] || !temporary_name[0]) {
        free(copy);
        return CLI_ERR;
    }
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat live_temporary;
    bool same_generation =
        fd >= 0 && fstatat(fd, temporary_name, &live_temporary, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(live_temporary.st_mode) && live_temporary.st_dev == expected_temporary->st_dev &&
        live_temporary.st_ino == expected_temporary->st_ino &&
        live_temporary.st_size == expected_temporary->st_size &&
        live_temporary.st_mtime == expected_temporary->st_mtime &&
        live_temporary.st_ctime == expected_temporary->st_ctime;
#ifdef __APPLE__
    same_generation =
        same_generation &&
        live_temporary.st_mtimespec.tv_nsec == expected_temporary->st_mtimespec.tv_nsec &&
        live_temporary.st_ctimespec.tv_nsec == expected_temporary->st_ctimespec.tv_nsec;
#else
    same_generation = same_generation &&
                      live_temporary.st_mtim.tv_nsec == expected_temporary->st_mtim.tv_nsec &&
                      live_temporary.st_ctim.tv_nsec == expected_temporary->st_ctim.tv_nsec;
#endif
    int rc = same_generation ? renameat(fd, temporary_name, fd, destination_name) : CLI_ERR;
    if (rc == 0) {
        (void)fsync(fd);
    }
    if (fd >= 0) {
        (void)close(fd);
    }
    free(copy);
    return rc == 0 ? CLI_OK : CLI_ERR;
}
#endif

/* Replace a binary only after a complete private temp file has been written
 * and synced. The previous implementation unlinked the working binary before
 * opening its replacement, so allocation/I/O/process interruption left the
 * installation with no executable; it also followed a symlink destination. */
int cbm_replace_binary(const char *path, const unsigned char *data, int len, int mode) {
    if (!path || !data || len <= 0) {
        return CLI_ERR;
    }
    char *temporary = cli_temp_template_for(path, ".new.XXXXXX");
    if (!temporary) {
        return CLI_ERR;
    }
    int fd = cbm_mkstemp(temporary);
    if (fd < 0) {
        free(temporary);
        return CLI_ERR;
    }
#ifdef _WIN32
    (void)mode;
#endif
#ifdef _WIN32
    FILE *f = _fdopen(fd, "wb");
#else
    FILE *f = fdopen(fd, "wb");
#endif
    if (!f) {
        (void)cli_close_fd(fd);
        (void)cbm_unlink(temporary);
        free(temporary);
        return CLI_ERR;
    }
    bool write_ok = fwrite(data, CLI_ELEM_SIZE, (size_t)len, f) == (size_t)len;
    bool flush_ok = write_ok && fflush(f) == 0;
#ifdef _WIN32
    bool mode_ok = flush_ok;
#else
    /* Keep mkstemp's private mode until every byte has been written. */
    bool mode_ok = flush_ok && fchmod(fileno(f), (mode_t)mode) == 0;
#endif
    bool sync_ok = mode_ok && cli_sync_fd(fileno(f)) == 0;
    int close_rc = fclose(f);
    if (!write_ok || !flush_ok || !mode_ok || !sync_ok || close_rc != 0) {
        (void)cbm_unlink(temporary);
        free(temporary);
        return CLI_ERR;
    }

    /* mkstemp makes substitution impractical for other users, but the name is
     * still addressable after close. Reject a symlink/non-regular replacement
     * and verify that the generation about to be published has the bytes the
     * caller supplied. */
    char expected_sha256[CBM_SHA256_HEX_LEN + 1];
    char actual_sha256[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(data, (size_t)len, expected_sha256);
#ifdef _WIN32
    wchar_t *wide_temporary = cbm_utf8_to_wide(temporary);
    DWORD temporary_attrs =
        wide_temporary ? GetFileAttributesW(wide_temporary) : INVALID_FILE_ATTRIBUTES;
    free(wide_temporary);
    bool regular_temporary =
        temporary_attrs != INVALID_FILE_ATTRIBUTES &&
        !(temporary_attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat temporary_st;
    bool regular_temporary = lstat(temporary, &temporary_st) == 0 &&
                             S_ISREG(temporary_st.st_mode) &&
                             (uint64_t)temporary_st.st_size == (uint64_t)(size_t)len;
#endif
    if (!regular_temporary ||
        cbm_sha256_file_hex_limited(temporary, (size_t)len, actual_sha256) != 0 ||
        strcmp(expected_sha256, actual_sha256) != 0) {
        (void)cbm_unlink(temporary);
        free(temporary);
        return CLI_ERR;
    }

#ifdef _WIN32
    int publish_rc = cbm_rename_replace(temporary, path);
#else
    int publish_rc = cli_publish_binary_posix(temporary, path, &temporary_st);
#endif
    if (publish_rc == 0) {
        free(temporary);
        return CLI_OK;
    }

#ifdef _WIN32
    /* A running executable may reject replacement but still permit a rename.
     * Move the old generation to a unique path only after the new generation
     * is durable, then restore it if publication fails. */
    char *old_path = cli_temp_template_for(path, ".old.XXXXXX");
    int old_fd = old_path ? cbm_mkstemp(old_path) : CLI_ERR;
    bool old_closed = old_fd >= 0 && _close(old_fd) == 0;
    bool moved_old = old_closed && cbm_rename_replace(path, old_path) == 0;
    if (moved_old && cbm_rename_replace(temporary, path) == 0) {
        (void)cbm_unlink(old_path);
        free(old_path);
        free(temporary);
        return CLI_OK;
    }
    if (moved_old) {
        if (cbm_rename_replace(old_path, path) == 0) {
            (void)cbm_unlink(temporary);
            free(old_path);
            free(temporary);
            return CLI_ERR;
        }
        /* A rare restore failure must not discard both durable generations.
         * Try once more to publish the new one; if that also fails, leave the
         * unique .old and .new files in place for recovery. */
        if (cbm_rename_replace(temporary, path) == 0) {
            free(old_path);
            free(temporary);
            return CLI_OK;
        }
        (void)fprintf(stderr,
                      "error: binary replacement and rollback both failed; recovery files: %s, "
                      "%s\n",
                      old_path, temporary);
        free(old_path);
        free(temporary);
        return CLI_ERR;
    }
    if (old_fd >= 0) {
        (void)cbm_unlink(old_path);
    }
    free(old_path);
#endif
    (void)cbm_unlink(temporary);
    free(temporary);
    return CLI_ERR;
}

typedef int (*cli_binary_step_fn)(const char *path, void *ctx);

typedef struct {
    unsigned char *data;
    int length;
    int mode;
    bool existed;
    char *recovery_path;
} cli_binary_backup_t;

static void cli_binary_backup_cleanup(cli_binary_backup_t *backup, bool remove_recovery) {
    if (!backup) {
        return;
    }
    if (remove_recovery && backup->recovery_path) {
        (void)cbm_unlink(backup->recovery_path);
    }
    free(backup->recovery_path);
    free(backup->data);
    memset(backup, 0, sizeof(*backup));
}

static int cli_binary_backup_persist(const char *path, cli_binary_backup_t *backup) {
    backup->recovery_path = cli_temp_template_for(path, ".rollback.XXXXXX");
    if (!backup->recovery_path) {
        return CLI_ERR;
    }
    int fd = cbm_mkstemp(backup->recovery_path);
    if (fd < 0) {
        return CLI_ERR;
    }
#ifdef _WIN32
    FILE *file = _fdopen(fd, "wb");
#else
    FILE *file = fdopen(fd, "wb");
#endif
    if (!file) {
        (void)cli_close_fd(fd);
        (void)cbm_unlink(backup->recovery_path);
        return CLI_ERR;
    }
    bool write_ok =
        fwrite(backup->data, CLI_ELEM_SIZE, (size_t)backup->length, file) == (size_t)backup->length;
    bool flush_ok = write_ok && fflush(file) == 0;
#ifdef _WIN32
    bool mode_ok = flush_ok;
#else
    bool mode_ok = flush_ok && fchmod(fileno(file), (mode_t)backup->mode) == 0;
#endif
    bool sync_ok = mode_ok && cli_sync_fd(fileno(file)) == 0;
    int close_rc = fclose(file);
    if (!write_ok || !flush_ok || !mode_ok || !sync_ok || close_rc != 0) {
        (void)cbm_unlink(backup->recovery_path);
        return CLI_ERR;
    }
    return CLI_OK;
}

static int cli_binary_backup_capture(const char *path, cli_binary_backup_t *backup) {
    if (!path || !path[0] || !backup) {
        return CLI_ERR;
    }
    memset(backup, 0, sizeof(*backup));
    int probe = cbm_path_probe(path);
    if (probe == 0) {
        backup->mode = CLI_OCTAL_PERM;
        return CLI_OK;
    }
    if (probe < 0) {
        return CLI_ERR;
    }
    char *data = NULL;
    size_t length = 0;
    char sha256[CBM_SHA256_HEX_LEN + CLI_SKIP_ONE];
    if (cbm_sha256_file_read_hex(path, DECOMPRESS_MAX_BYTES, &data, &length, sha256) !=
            CBM_SHA256_FILE_HASHED ||
        length == 0 || length > INT_MAX) {
        free(data);
        return CLI_ERR;
    }
    backup->data = (unsigned char *)data;
    backup->length = (int)length;
    backup->mode = CLI_OCTAL_PERM;
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cli_binary_backup_cleanup(backup, true);
        return CLI_ERR;
    }
    backup->mode = (int)(st.st_mode & 0777);
#endif
    backup->existed = true;
    if (cli_binary_backup_persist(path, backup) != CLI_OK) {
        cli_binary_backup_cleanup(backup, true);
        return CLI_ERR;
    }
    return CLI_OK;
}

static int cli_binary_backup_restore(const char *path, cli_binary_backup_t *backup) {
    if (!path || !backup) {
        return CLI_ERR;
    }
    if (backup->existed) {
        if (backup->recovery_path && cbm_rename_replace(backup->recovery_path, path) == CLI_OK) {
            return CLI_OK;
        }
        return cbm_replace_binary(path, backup->data, backup->length, backup->mode);
    }
    int probe = cbm_path_probe(path);
    if (probe == 0) {
        return CLI_OK;
    }
    return probe > 0 && cbm_unlink(path) == 0 ? CLI_OK : CLI_ERR;
}

static int cli_publish_verified_binary(const char *path, const unsigned char *data, int length,
                                       int mode, cli_binary_step_fn before_publish,
                                       void *before_ctx, cli_binary_step_fn verify,
                                       void *verify_ctx) {
    if (!path || !data || length <= 0 || !verify) {
        return CLI_ERR;
    }
    cli_binary_backup_t backup;
    if (cli_binary_backup_capture(path, &backup) != CLI_OK) {
        return CLI_ERR;
    }
    if (before_publish && before_publish(path, before_ctx) != CLI_OK) {
        cli_binary_backup_cleanup(&backup, true);
        return CLI_ERR;
    }
    if (cbm_replace_binary(path, data, length, mode) != CLI_OK) {
        cli_binary_backup_cleanup(&backup, true);
        return CLI_ERR;
    }
    if (verify(path, verify_ctx) == CLI_OK) {
        cli_binary_backup_cleanup(&backup, true);
        return CLI_OK;
    }
    int rollback_rc = cli_binary_backup_restore(path, &backup);
    if (rollback_rc != CLI_OK) {
        (void)fprintf(stderr,
                      "error: installed binary verification and rollback both failed: %s"
                      " (recovery: %s)\n",
                      path, backup.recovery_path ? backup.recovery_path : "unavailable");
    }
    cli_binary_backup_cleanup(&backup, rollback_rc == CLI_OK);
    return CLI_ERR;
}

// cppcheck-suppress constParameterCallback
static int cli_test_binary_verifier(const char *path, void *ctx) {
    (void)path;
    return ctx ? CLI_OK : CLI_ERR;
}

/* Internal regression seam for the post-publication rollback transaction. */
int cbm_cli_publish_verified_for_test(const char *path, const unsigned char *data, int length,
                                      bool verification_ok) {
    return cli_publish_verified_binary(path, data, length, CLI_OCTAL_PERM, NULL, NULL,
                                       cli_test_binary_verifier,
                                       verification_ok ? (void *)(uintptr_t)CLI_TRUE : NULL);
}

/* ── Skill file content (embedded) ────────────────────────────── */

/* Consolidated from 4 separate skills into 1 with progressive disclosure.
 * This embedded version is the single source of truth for the CLI installer.
 * Based on PR #81 by @gdilla — factual corrections applied. */
static const char skill_content[] =
    "---\n"
    "name: codebase-memory\n"
    "description: Use the codebase knowledge graph for structural code queries and Global "
    "Memory for durable cross-project knowledge. "
    "Triggers on: explore the codebase, understand the architecture, what functions exist, "
    "show me the structure, who calls this function, what does X call, trace the call chain, "
    "find callers of, show dependencies, impact analysis, dead code, unused functions, "
    "high fan-out, refactor candidates, code quality audit, graph query syntax, "
    "Cypher query examples, edge types, how to use search_graph, global memory, prior "
    "cross-project decisions, reusable experience, memory_query.\n"
    "---\n"
    "\n"
    "# Codebase Memory — Knowledge Graph Tools\n"
    "\n"
    "Graph tools return precise structural results in ~500 tokens vs ~80K for grep.\n"
    "\n"
    "## Quick Decision Matrix\n"
    "\n"
    "| Question | Tool call |\n"
    "|----------|----------|\n"
    "| Who calls X? | `trace_path(direction=\"inbound\")` |\n"
    "| What does X call? | `trace_path(direction=\"outbound\")` |\n"
    "| Full call context | `trace_path(direction=\"both\")` |\n"
    "| Find by name pattern | `search_graph(name_pattern=\"...\")` |\n"
    "| Dead code | `search_graph(max_degree=0, exclude_entry_points=true)` |\n"
    "| Cross-service edges | `query_graph` with Cypher |\n"
    "| Impact of local changes | `detect_changes()` |\n"
    "| Risk-classified trace | `trace_path(risk_labels=true)` |\n"
    "| Text search | `search_code` or Grep |\n"
    "| Repository design context | `get_design_context` |\n"
    "| Reuse cross-project knowledge | `memory_query` |\n"
    "\n"
    "## Exploration Workflow\n"
    "1. `list_projects` — check if project is indexed\n"
    "2. `get_graph_schema` — understand node/edge types\n"
    "3. `search_graph(label=\"Function\", name_pattern=\".*Pattern.*\")` — find code\n"
    "4. `get_code_snippet(qualified_name=\"project.path.FuncName\")` — read source\n"
    "   Trust the graph-derived range as fresh only when `source_state=\"current\"`; handle "
    "`stale_worktree`, `missing_worktree`, `range_unavailable`, `metadata_match`, or `unknown` "
    "explicitly.\n"
    "\n"
    "## Tracing Workflow\n"
    "1. `search_graph(name_pattern=\".*FuncName.*\")` — discover exact name\n"
    "2. `trace_path(function_name=\"FuncName\", direction=\"both\", depth=3)` — trace\n"
    "3. `detect_changes()` — map git diff to affected symbols\n"
    "\n"
    "## Design Context Workflow\n"
    "1. `get_design_context(project=\"...\")` — inspect systems, tokens, components, modes, "
    "and usages\n"
    "2. Filter by `scope`, `token`, or `component` before broader graph traversal\n"
    "3. Keep DESIGN.md/token authoring and CSS generation in external design tooling; this "
    "server indexes those repository artifacts read-only\n"
    "4. Design Context remains project-local. Promote durable cross-project decisions to Global "
    "Memory only when explicitly authorized\n"
    "\n"
    "## Quality Analysis\n"
    "- Dead code: `search_graph(max_degree=0, exclude_entry_points=true)`\n"
    "- High fan-out: `search_graph(min_degree=10, relationship=\"CALLS\", "
    "direction=\"outbound\")`\n"
    "- High fan-in: `search_graph(min_degree=10, relationship=\"CALLS\", "
    "direction=\"inbound\")`\n"
    "\n"
    "## Global Memory Retrieval\n"
    "- Use `memory_query` when prior cross-project knowledge is relevant; memory is not "
    "implicitly injected.\n"
    "- Compare `applicability`, freshness, evidence lineage, and the returned route before "
    "reusing a result.\n"
    "- Do not search for an opposing view by default. Follow `verify`, `experiment`, or "
    "`deliberate` only when the returned uncertainty and impact justify it.\n"
    "- Repetition, retrieval frequency, and graph centrality are audit-priority signals, "
    "never evidence that a claim is true.\n"
    "- Persist only durable, scoped knowledge when the current task explicitly authorizes a "
    "Global Memory write. Keep repository ADRs local unless explicitly promoted.\n"
    "- `memory_commit` requires `user_approved=true`. External export/import paths require "
    "both `allow_external_path=true` and `user_approved=true`; replacing an export also "
    "requires `overwrite=true`.\n"
    "- Use `memory_export`, `memory_import`, or `memory_sync` only on explicit request; bundles "
    "include raw sources and may be sensitive.\n"
    "\n"
    "## 24 MCP Tools\n"
    "`index_repository`, `index_status`, `list_projects`, `delete_project`,\n"
    "`search_graph`, `search_code`, `trace_path`, `detect_changes`,\n"
    "`query_graph`, `get_graph_schema`, `get_code_snippet`, `get_architecture`, "
    "`get_design_context`,\n"
    "`manage_adr`, `ingest_traces`, `memory_ingest`, `memory_query`, `memory_status`,\n"
    "`memory_propose`, `memory_commit`, `memory_lint`, `memory_export`,\n"
    "`memory_import`, `memory_sync`\n"
    "\n"
    "## Edge Types\n"
    "CALLS, HTTP_CALLS, ASYNC_CALLS, IMPORTS, DEFINES, DEFINES_METHOD, DEFINES_TOKEN,\n"
    "HANDLES, IMPLEMENTS, OVERRIDES, USAGE, FILE_CHANGES_WITH,\n"
    "CONTAINS_FILE, CONTAINS_FOLDER, CONTAINS_PACKAGE,\n"
    "PROVIDES, ALIASES_TO, OVERRIDES, USES_TOKEN, DOCUMENTED_BY, GUIDED_BY, GENERATED_AS\n"
    "\n"
    "## Cypher Examples (for query_graph)\n"
    "```\n"
    "MATCH (a)-[r:HTTP_CALLS]->(b) RETURN a.name, b.name, r.url_path, "
    "r.confidence LIMIT 20\n"
    "MATCH (f:Function) WHERE f.name =~ '.*Handler.*' RETURN f.name, f.file_path\n"
    "MATCH (a)-[r:CALLS]->(b) WHERE a.name = 'main' RETURN b.name\n"
    "```\n"
    "\n"
    "## Gotchas\n"
    "1. `search_graph(relationship=\"HTTP_CALLS\")` filters nodes by degree — "
    "use `query_graph` with Cypher to see actual edges.\n"
    "2. `query_graph` has a 200-row cap — use `search_graph` with degree filters "
    "for counting.\n"
    "3. `trace_path` needs exact names — use `search_graph(name_pattern=...)` first.\n"
    "4. `direction=\"outbound\"` misses cross-service callers — use "
    "`direction=\"both\"`.\n"
    "5. Results default to 10 per page — check `has_more` and use `offset`.\n";

/* Old skill names — cleaned up during install to remove stale directories. */
static const char *old_skill_names[] = {
    "codebase-memory-exploring",
    "codebase-memory-tracing",
    "codebase-memory-quality",
    "codebase-memory-reference",
};
enum { OLD_SKILL_COUNT = 4 };

static const cbm_skill_t skills[CBM_SKILL_COUNT] = {
    {"codebase-memory", skill_content},
};

const cbm_skill_t *cbm_get_skills(void) {
    return skills;
}

/* ── Recursive mkdir (via compat_fs) ──────────────────────────── */

static int mkdirp(const char *path, int mode) {
    return (int)cbm_mkdir_p(path, mode) ? 0 : CLI_ERR;
}

/* ── Recursive rmdir ──────────────────────────────────────────── */

/* Skill removal is destructive, so it must never follow a symlink/junction.
 * The old breadth-first walker used stat() (which follows links), silently
 * stopped after 256 directories, and truncated every path to 1 KiB. A skill
 * directory replaced with a link could therefore erase an unrelated tree.
 *
 * POSIX walks relative to already-open directory descriptors. O_NOFOLLOW and
 * AT_SYMLINK_NOFOLLOW keep each lookup anchored even if another process swaps
 * a name while removal is in progress. Windows directory enumeration exposes
 * reparse attributes; those entries are removed as links and never entered. */
enum { RMDIR_MAX_DEPTH = 128 };

typedef enum {
    CLI_PATH_MISSING = 0,
    CLI_PATH_FILE,
    CLI_PATH_DIRECTORY,
    CLI_PATH_REPARSE,
    CLI_PATH_ERROR,
} cli_path_kind_t;

static cli_path_kind_t cli_path_kind_nofollow(const char *path) {
    if (!path || !path[0]) {
        return CLI_PATH_ERROR;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return CLI_PATH_ERROR;
    }
    DWORD attrs = GetFileAttributesW(wide);
    DWORD saved = GetLastError();
    free(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return saved == ERROR_FILE_NOT_FOUND || saved == ERROR_PATH_NOT_FOUND ? CLI_PATH_MISSING
                                                                              : CLI_PATH_ERROR;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return CLI_PATH_REPARSE;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? CLI_PATH_DIRECTORY : CLI_PATH_FILE;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? CLI_PATH_MISSING : CLI_PATH_ERROR;
    }
    if (S_ISLNK(st.st_mode)) {
        return CLI_PATH_REPARSE;
    }
    return S_ISDIR(st.st_mode) ? CLI_PATH_DIRECTORY : CLI_PATH_FILE;
#endif
}

#ifdef _WIN32
static HANDLE cli_windows_open_nofollow(const wchar_t *path, DWORD access) {
    if (!path || !path[0]) {
        return INVALID_HANDLE_VALUE;
    }
    /* Omitting FILE_SHARE_DELETE pins this name's directory generation until
     * the handle closes. OPEN_REPARSE_POINT ensures the exact entry is
     * inspected rather than following a junction or symbolic link. */
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

static bool cli_windows_handle_info(HANDLE handle, BY_HANDLE_FILE_INFORMATION *info) {
    return handle != INVALID_HANDLE_VALUE && info && GetFileType(handle) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(handle, info) != FALSE;
}

static wchar_t *cli_windows_final_path(HANDLE handle) {
    DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD needed = GetFinalPathNameByHandleW(handle, NULL, 0, flags);
    if (needed == 0 || needed > 32768U) {
        return NULL;
    }
    wchar_t *path = malloc(((size_t)needed + CLI_SKIP_ONE) * sizeof(*path));
    if (!path) {
        return NULL;
    }
    DWORD written = GetFinalPathNameByHandleW(handle, path, needed + CLI_SKIP_ONE, flags);
    if (written == 0 || written > needed) {
        free(path);
        return NULL;
    }
    return path;
}

static wchar_t *cli_windows_child_path(const wchar_t *parent, const wchar_t *name) {
    if (!parent || !name || !name[0] || wcschr(name, L'\\') || wcschr(name, L'/')) {
        return NULL;
    }
    size_t parent_len = wcslen(parent);
    size_t name_len = wcslen(name);
    bool separator = parent_len > 0 && parent[parent_len - CLI_SKIP_ONE] != L'\\' &&
                     parent[parent_len - CLI_SKIP_ONE] != L'/';
    size_t extra = separator ? CLI_PAIR_LEN : CLI_SKIP_ONE;
    if (parent_len > SIZE_MAX - name_len - extra) {
        return NULL;
    }
    wchar_t *path = malloc((parent_len + name_len + extra) * sizeof(*path));
    if (!path) {
        return NULL;
    }
    wmemcpy(path, parent, parent_len);
    size_t offset = parent_len;
    if (separator) {
        path[offset++] = L'\\';
    }
    wmemcpy(path + offset, name, name_len + CLI_SKIP_ONE);
    return path;
}

static bool cli_windows_mark_delete(HANDLE handle, BY_HANDLE_FILE_INFORMATION *info) {
    if (!cli_windows_handle_info(handle, info)) {
        return false;
    }
    if ((info->dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        FILE_BASIC_INFO basic = {0};
        if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
            return false;
        }
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
            return false;
        }
    }
    FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != FALSE;
}
#endif

static int write_skill_file(const char *skill_path, const char *content) {
    if (!skill_path || !content) {
        return CLI_ERR;
    }
#ifndef _WIN32
    int dir_fd = open(skill_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0) {
        return CLI_ERR;
    }
    int fd =
        openat(dir_fd, "SKILL.md", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0640);
    close(dir_fd);
    if (fd < 0) {
        return CLI_ERR;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return CLI_ERR;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        return CLI_ERR;
    }
#else
    wchar_t *wide_dir = cbm_utf8_to_wide(skill_path);
    if (!wide_dir) {
        return CLI_ERR;
    }
    HANDLE pinned_dir = cli_windows_open_nofollow(wide_dir, FILE_READ_ATTRIBUTES);
    free(wide_dir);
    BY_HANDLE_FILE_INFORMATION dir_info = {0};
    if (!cli_windows_handle_info(pinned_dir, &dir_info) ||
        !(dir_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (dir_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (pinned_dir != INVALID_HANDLE_VALUE) {
            CloseHandle(pinned_dir);
        }
        return CLI_ERR;
    }
    wchar_t *pinned_path = cli_windows_final_path(pinned_dir);
    wchar_t *wide_file = cli_windows_child_path(pinned_path, L"SKILL.md");
    free(pinned_path);
    if (!wide_file) {
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
    HANDLE handle =
        CreateFileW(wide_file, GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide_file);
    if (handle == INVALID_HANDLE_VALUE) {
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
    FILE_ATTRIBUTE_TAG_INFO tag = {0};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(handle);
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
    LARGE_INTEGER zero = {0};
    if (!SetFilePointerEx(handle, zero, NULL, FILE_BEGIN) || !SetEndOfFile(handle)) {
        CloseHandle(handle);
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
    FILE *file = _fdopen(fd, "wb");
    if (!file) {
        _close(fd);
        CloseHandle(pinned_dir);
        return CLI_ERR;
    }
#endif
    size_t length = strlen(content);
    bool ok = fwrite(content, CLI_ELEM_SIZE, length, file) == length;
    if (fclose(file) != 0) {
        ok = false;
    }
#ifdef _WIN32
    CloseHandle(pinned_dir);
#endif
    return ok ? CLI_OK : CLI_ERR;
}

#ifndef _WIN32
static int rmdir_fd_contents(int dir_fd, int depth) {
    if (dir_fd < 0 || depth > RMDIR_MAX_DEPTH) {
        return CLI_ERR;
    }
    int scan_fd = dup(dir_fd);
    if (scan_fd < 0) {
        return CLI_ERR;
    }
    DIR *dir = fdopendir(scan_fd);
    if (!dir) {
        close(scan_fd);
        return CLI_ERR;
    }

    int rc = CLI_OK;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        struct stat st;
        if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = CLI_ERR;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            int child_fd = openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child_fd < 0) {
                rc = CLI_ERR;
                continue;
            }
            int child_rc = rmdir_fd_contents(child_fd, depth + CLI_SKIP_ONE);
            close(child_fd);
            if (child_rc != CLI_OK || unlinkat(dir_fd, name, AT_REMOVEDIR) != 0) {
                rc = CLI_ERR;
            }
        } else if (unlinkat(dir_fd, name, 0) != 0) {
            rc = CLI_ERR;
        }
        errno = 0;
    }
    if (errno != 0) {
        rc = CLI_ERR;
    }
    closedir(dir);
    return rc;
}

static int rmdir_recursive(const char *path) {
    if (!path || !path[0]) {
        return CLI_ERR;
    }
    int root_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        return CLI_ERR;
    }
    int rc = rmdir_fd_contents(root_fd, 0);
    close(root_fd);
    if (rc != CLI_OK || cbm_rmdir(path) != 0) {
        return CLI_ERR;
    }
    return CLI_OK;
}
#else
static int rmdir_windows_handle_contents(HANDLE directory, int depth) {
    if (directory == INVALID_HANDLE_VALUE || depth > RMDIR_MAX_DEPTH) {
        return CLI_ERR;
    }
    BY_HANDLE_FILE_INFORMATION directory_info = {0};
    if (!cli_windows_handle_info(directory, &directory_info) ||
        !(directory_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (directory_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return CLI_ERR;
    }
    wchar_t *base = cli_windows_final_path(directory);
    wchar_t *pattern = cli_windows_child_path(base, L"*");
    if (!base || !pattern) {
        free(pattern);
        free(base);
        return CLI_ERR;
    }

    WIN32_FIND_DATAW find_data = {0};
    HANDLE find = FindFirstFileW(pattern, &find_data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        free(base);
        return error == ERROR_FILE_NOT_FOUND ? CLI_OK : CLI_ERR;
    }
    int rc = CLI_OK;
    bool more = true;
    while (more) {
        const wchar_t *name = find_data.cFileName;
        bool dot = name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
        if (!dot) {
            wchar_t *child_path = cli_windows_child_path(base, name);
            HANDLE child =
                child_path ? cli_windows_open_nofollow(child_path, DELETE | FILE_READ_ATTRIBUTES |
                                                                       FILE_WRITE_ATTRIBUTES)
                           : INVALID_HANDLE_VALUE;
            free(child_path);
            BY_HANDLE_FILE_INFORMATION child_info = {0};
            bool child_ok = cli_windows_handle_info(child, &child_info);
            bool is_directory =
                child_ok && (child_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool is_reparse =
                child_ok && (child_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (!child_ok ||
                (is_directory && !is_reparse &&
                 rmdir_windows_handle_contents(child, depth + CLI_SKIP_ONE) != CLI_OK) ||
                !cli_windows_mark_delete(child, &child_info)) {
                rc = CLI_ERR;
            }
            if (child != INVALID_HANDLE_VALUE) {
                CloseHandle(child);
            }
        }
        if (!FindNextFileW(find, &find_data)) {
            DWORD error = GetLastError();
            more = false;
            if (error != ERROR_NO_MORE_FILES) {
                rc = CLI_ERR;
            }
        }
    }
    FindClose(find);
    free(base);
    return rc;
}

static int rmdir_recursive(const char *path) {
    if (!path || !path[0]) {
        return CLI_ERR;
    }
    wchar_t *wide = cbm_utf8_to_wide(path);
    HANDLE root =
        wide
            ? cli_windows_open_nofollow(wide, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)
            : INVALID_HANDLE_VALUE;
    free(wide);
    BY_HANDLE_FILE_INFORMATION root_info = {0};
    int rc = cli_windows_handle_info(root, &root_info) &&
                     (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                     (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                     rmdir_windows_handle_contents(root, 0) == CLI_OK &&
                     cli_windows_mark_delete(root, &root_info)
                 ? CLI_OK
                 : CLI_ERR;
    if (root != INVALID_HANDLE_VALUE) {
        CloseHandle(root);
    }
    return rc;
}
#endif

/* ── Skill management ─────────────────────────────────────────── */

int cbm_install_skills(const char *skills_dir, bool force, bool dry_run) {
    if (!skills_dir) {
        return 0;
    }
    int count = 0;

    /* Clean up old 4-skill directories (consolidated into 1). */
    for (int i = 0; i < OLD_SKILL_COUNT; i++) {
        char old_path[CLI_BUF_1K];
        int old_n = snprintf(old_path, sizeof(old_path), "%s/%s", skills_dir, old_skill_names[i]);
        if (old_n > 0 && old_n < (int)sizeof(old_path) &&
            cli_path_kind_nofollow(old_path) == CLI_PATH_DIRECTORY && !dry_run) {
            (void)rmdir_recursive(old_path);
        }
    }

    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char skill_path[CLI_BUF_1K];
        int skill_n = snprintf(skill_path, sizeof(skill_path), "%s/%s", skills_dir, skills[i].name);
        if (skill_n <= 0 || skill_n >= (int)sizeof(skill_path)) {
            continue;
        }
        char file_path[CLI_BUF_1K];
        int file_n = snprintf(file_path, sizeof(file_path), "%s/SKILL.md", skill_path);
        if (file_n <= 0 || file_n >= (int)sizeof(file_path)) {
            continue;
        }

        cli_path_kind_t skill_kind = cli_path_kind_nofollow(skill_path);
        if (skill_kind == CLI_PATH_REPARSE || skill_kind == CLI_PATH_FILE ||
            skill_kind == CLI_PATH_ERROR) {
            continue;
        }

        /* Check if already exists */
        if (!force) {
            cli_path_kind_t file_kind = cli_path_kind_nofollow(file_path);
            if (file_kind != CLI_PATH_MISSING) {
                continue;
            }
        }

        if (dry_run) {
            count++;
            continue;
        }

        if (mkdirp(skill_path, DIR_PERMS) != 0) {
            continue;
        }
        if (cli_path_kind_nofollow(skill_path) != CLI_PATH_DIRECTORY ||
            write_skill_file(skill_path, skills[i].content) != CLI_OK) {
            continue;
        }
        count++;
    }
    return count;
}

int cbm_remove_skills(const char *skills_dir, bool dry_run) {
    if (!skills_dir) {
        return 0;
    }
    int count = 0;

    for (int i = 0; i < CBM_SKILL_COUNT; i++) {
        char skill_path[CLI_BUF_1K];
        int n = snprintf(skill_path, sizeof(skill_path), "%s/%s", skills_dir, skills[i].name);
        if (n <= 0 || n >= (int)sizeof(skill_path)) {
            continue;
        }
        cli_path_kind_t kind = cli_path_kind_nofollow(skill_path);
        if (kind == CLI_PATH_MISSING || kind == CLI_PATH_ERROR || kind == CLI_PATH_REPARSE ||
            kind == CLI_PATH_FILE) {
            continue;
        }

        if (dry_run) {
            count++;
            continue;
        }

        if (rmdir_recursive(skill_path) == 0) {
            count++;
        }
    }
    return count;
}

bool cbm_remove_old_monolithic_skill(const char *skills_dir, bool dry_run) {
    if (!skills_dir) {
        return false;
    }

    char old_path[CLI_BUF_1K];
    int n = snprintf(old_path, sizeof(old_path), "%s/codebase-memory-mcp", skills_dir);
    if (n <= 0 || n >= (int)sizeof(old_path) ||
        cli_path_kind_nofollow(old_path) != CLI_PATH_DIRECTORY) {
        return false;
    }

    if (dry_run) {
        return true;
    }
    return rmdir_recursive(old_path) == 0;
}

/* ── JSON config helpers (using yyjson) ───────────────────────── */

/* Read a JSON file into a yyjson document. Returns NULL on error. */
static yyjson_doc *read_json_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }

    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (long)CLI_MB_10 * CLI_MB_FACTOR) {
        (void)fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)size + CLI_SKIP_ONE);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, CLI_ELEM_SIZE, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size) {
        nread = (size_t)size;
    }
    buf[nread] = '\0';

    /* Allow JSONC (comments + trailing commas) — Zed settings.json uses this format */
    yyjson_read_flag flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;
    yyjson_doc *doc = yyjson_read(buf, nread, flags);
    free(buf);
    return doc;
}

/* Write a mutable yyjson document to a file with pretty printing. */
static int write_json_file(const char *path, yyjson_mut_doc *doc) {
    /* Ensure parent directory exists */
    char dir[CLI_BUF_1K];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdirp(dir, DIR_PERMS);
    }

    yyjson_write_flag flags = YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_UNICODE;
    size_t len;
    char *json = yyjson_mut_write(doc, flags, &len);
    if (!json) {
        return CLI_ERR;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        free(json);
        return CLI_ERR;
    }

    size_t written = fwrite(json, CLI_ELEM_SIZE, len, f);
    /* Add trailing newline */
    (void)fputc('\n', f);
    (void)fclose(f);
    free(json);

    return written == len ? 0 : CLI_ERR;
}

/* ── Editor MCP: Cursor/Windsurf/Gemini (mcpServers key) ──────── */

int cbm_install_editor_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    /* Read existing or start fresh */
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Get or create mcpServers object */
    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "mcpServers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "mcpServers", servers);
    }

    /* Remove existing entry if present */
    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    /* Add our entry */
    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
    yyjson_mut_obj_add_val(mdoc, servers, "codebase-memory-mcp", entry);

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_remove_editor_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "mcpServers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── OpenClaw MCP (nested mcp.servers with command + args) ────── */

int cbm_install_openclaw_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *mcp = yyjson_mut_obj_get(root, "mcp");
    if (!mcp || !yyjson_mut_is_obj(mcp)) {
        mcp = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "mcp", mcp);
    }

    yyjson_mut_val *servers = yyjson_mut_obj_get(mcp, "servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, mcp, "servers", servers);
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_bool(mdoc, entry, "enabled", true);
    yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
    yyjson_mut_val *args = yyjson_mut_arr(mdoc);
    yyjson_mut_obj_add_val(mdoc, entry, "args", args);
    yyjson_mut_obj_add_val(mdoc, servers, "codebase-memory-mcp", entry);

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_remove_openclaw_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *mcp = yyjson_mut_obj_get(root, "mcp");
    if (!mcp || !yyjson_mut_is_obj(mcp)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_val *servers = yyjson_mut_obj_get(mcp, "servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── VS Code MCP (servers key with type:stdio) ────────────────── */

int cbm_install_vscode_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "servers", servers);
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, entry, "type", "stdio");
    yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
    yyjson_mut_obj_add_val(mdoc, servers, "codebase-memory-mcp", entry);

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_remove_vscode_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── Zed MCP (context_servers with command + args) ────────────── */

int cbm_install_zed_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "context_servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        servers = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "context_servers", servers);
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, entry, "command", binary_path);
    yyjson_mut_val *args = yyjson_mut_arr(mdoc);
    yyjson_mut_arr_add_str(mdoc, args, "");
    yyjson_mut_obj_add_val(mdoc, entry, "args", args);
    yyjson_mut_obj_add_val(mdoc, servers, "codebase-memory-mcp", entry);

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_remove_zed_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *servers = yyjson_mut_obj_get(root, "context_servers");
    if (!servers || !yyjson_mut_is_obj(servers)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_obj_remove_key(servers, "codebase-memory-mcp");

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── Agent detection ──────────────────────────────────────────── */

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Resolve the Claude Code config dir.
 * Honors $CLAUDE_CONFIG_DIR; falls back to "$home_dir/.claude". */
static void cbm_claude_config_dir(const char *home_dir, char *out, size_t out_sz) {
    if (out_sz == 0) {
        return;
    }
    out[0] = '\0';
    char env_buf[CLI_BUF_1K];
    const char *env = cbm_safe_getenv("CLAUDE_CONFIG_DIR", env_buf, sizeof(env_buf), NULL);
    if (env && env[0]) {
        snprintf(out, out_sz, "%s", env);
    } else if (home_dir && home_dir[0]) {
        snprintf(out, out_sz, "%s/.claude", home_dir);
    }
}

/* Resolve the parent dir containing `.claude.json` (Claude Code's user config file).
 * Honors $CLAUDE_CONFIG_DIR; falls back to "$home_dir". */
static void cbm_claude_user_root(const char *home_dir, char *out, size_t out_sz) {
    if (out_sz == 0) {
        return;
    }
    out[0] = '\0';
    char env_buf[CLI_BUF_1K];
    const char *env = cbm_safe_getenv("CLAUDE_CONFIG_DIR", env_buf, sizeof(env_buf), NULL);
    if (env && env[0]) {
        snprintf(out, out_sz, "%s", env);
    } else if (home_dir && home_dir[0]) {
        snprintf(out, out_sz, "%s", home_dir);
    }
}

/* Build the hook command string written into Claude Code's settings.json.
 * Honors $CLAUDE_CONFIG_DIR. When CLAUDE_CONFIG_DIR is unset, preserves the
 * legacy tilde-expanded form so settings.json stays portable across HOME values. */
static void cbm_resolve_hook_command(const char *script_name, char *out, size_t out_sz) {
    if (out_sz == 0) {
        return;
    }
    out[0] = '\0';
    char env_buf[CLI_BUF_1K];
    const char *env = cbm_safe_getenv("CLAUDE_CONFIG_DIR", env_buf, sizeof(env_buf), NULL);
    if (env && env[0]) {
        snprintf(out, out_sz, "%s/hooks/%s", env, script_name);
    } else {
        snprintf(out, out_sz, "~/.claude/hooks/%s", script_name);
    }
}

cbm_detected_agents_t cbm_detect_agents(const char *home_dir) {
    cbm_detected_agents_t agents;
    memset(&agents, 0, sizeof(agents));
    if (!home_dir || !home_dir[0]) {
        return agents;
    }

    char path[CLI_BUF_1K];

    cbm_claude_config_dir(home_dir, path, sizeof(path));
    agents.claude_code = path[0] != '\0' && dir_exists(path);

    snprintf(path, sizeof(path), "%s/.codex", home_dir);
    agents.codex = dir_exists(path);

    snprintf(path, sizeof(path), "%s/.gemini", home_dir);
    agents.gemini = dir_exists(path);

#ifdef __APPLE__
    snprintf(path, sizeof(path), "%s/Library/Application Support/Zed", home_dir);
#elif defined(_WIN32)
    snprintf(path, sizeof(path), "%s/AppData/Local/Zed", home_dir);
#else
    snprintf(path, sizeof(path), "%s/.config/zed", home_dir);
#endif
    agents.zed = dir_exists(path);

    agents.opencode = cbm_find_cli("opencode", home_dir)[0] != '\0';

    /* Antigravity CLI (2026 unification) installs under ~/.gemini/antigravity-cli/
     * (brain/, mcp/, settings.json), with MCP config in the shared
     * ~/.gemini/config/mcp_config.json. */
    snprintf(path, sizeof(path), "%s/.gemini/antigravity-cli", home_dir);
    if (dir_exists(path)) {
        agents.antigravity = true;
    }

    agents.aider = cbm_find_cli("aider", home_dir)[0] != '\0';

#ifdef __APPLE__
    snprintf(path, sizeof(path),
             "%s/Library/Application Support/Code/User/globalStorage/kilocode.kilo-code", home_dir);
#elif defined(_WIN32)
    snprintf(path, sizeof(path), "%s/AppData/Roaming/Code/User/globalStorage/kilocode.kilo-code",
             home_dir);
#else
    snprintf(path, sizeof(path), "%s/.config/Code/User/globalStorage/kilocode.kilo-code", home_dir);
#endif
    agents.kilocode = dir_exists(path);

#ifdef __APPLE__
    snprintf(path, sizeof(path), "%s/Library/Application Support/Code/User", home_dir);
#elif defined(_WIN32)
    snprintf(path, sizeof(path), "%s/AppData/Roaming/Code/User", home_dir);
#else
    snprintf(path, sizeof(path), "%s/.config/Code/User", home_dir);
#endif
    agents.vscode = dir_exists(path);

    /* Cursor stores its user MCP config in ~/.cursor/mcp.json on all platforms. */
    snprintf(path, sizeof(path), "%s/.cursor", home_dir);
    agents.cursor = dir_exists(path);

    snprintf(path, sizeof(path), "%s/.openclaw", home_dir);
    agents.openclaw = dir_exists(path);

    /* Kiro: ~/.kiro/ */
    snprintf(path, sizeof(path), "%s/.kiro", home_dir);
    agents.kiro = dir_exists(path);

    /* Junie (JetBrains): ~/.junie/ */
    snprintf(path, sizeof(path), "%s/.junie", home_dir);
    agents.junie = dir_exists(path);

    return agents;
}

/* ── Shared agent instructions content ────────────────────────── */

static const char agent_instructions_content[] =
    "# Codebase Knowledge Graph (codebase-memory-mcp)\n"
    "\n"
    "This project uses codebase-memory-mcp to maintain a knowledge graph of the codebase.\n"
    "ALWAYS prefer MCP graph tools over grep/glob/file-search for code discovery.\n"
    "\n"
    "## Priority Order\n"
    "1. `search_graph` — find functions, classes, routes, variables by pattern\n"
    "2. `trace_path` — trace who calls a function or what it calls\n"
    "3. `get_code_snippet` — read specific function/class source code\n"
    "4. `query_graph` — run Cypher queries for complex patterns\n"
    "5. `get_architecture` — high-level project summary\n"
    "\n"
    "## When to fall back to grep/glob\n"
    "- Searching for string literals, error messages, config values\n"
    "- Searching non-code files (Dockerfiles, shell scripts, configs)\n"
    "- When MCP tools return insufficient results\n"
    "\n"
    "## Examples\n"
    "- Find a handler: `search_graph(name_pattern=\".*OrderHandler.*\")`\n"
    "- Who calls it: `trace_path(function_name=\"OrderHandler\", direction=\"inbound\")`\n"
    "- Read source: `get_code_snippet(qualified_name=\"pkg/orders.OrderHandler\")`\n"
    "\n"
    "## Global Memory\n"
    "Global Memory is user-global and repository-independent. It is not implicitly injected.\n"
    "- Use `memory_query` only when prior cross-project facts, decisions, or experiences could "
    "materially help. Do not query it for every task.\n"
    "- Include the current repository and task constraints in `current_context`. Before reuse, "
    "inspect the returned `route`, applicability, freshness, evidence lineage, and conflicts. "
    "Do not search for an opposing view by default; follow `verify`, `experiment`, "
    "`deliberate`, or `abstain` when the route calls for it.\n"
    "- Global writes affect every project. Persist only durable, scoped knowledge when the "
    "current task explicitly authorizes it. When evidence exists, use `memory_ingest`; curate "
    "changes through `memory_propose` then `memory_commit`. On conflict, reread and repropose "
    "instead of forcing last-write-wins.\n"
    "- `memory_commit` requires `user_approved=true`. External export/import paths require "
    "both `allow_external_path=true` and `user_approved=true`; export replacement also "
    "requires `overwrite=true`.\n"
    "- Keep repository-specific details and ADRs local unless explicitly promoted. Use "
    "`memory_lint` to audit memory. Use `memory_export`, `memory_import`, or `memory_sync` only "
    "on explicit request; shared bundles include raw sources and may be sensitive.\n";

const char *cbm_get_agent_instructions(void) {
    return agent_instructions_content;
}

/* Legacy public compatibility getter. Codex now consumes the same managed
 * AGENTS.md content as every other supported instruction-file agent. */
const char *cbm_get_codex_instructions(void) {
    return cbm_get_agent_instructions();
}

/* ── Instructions file upsert ─────────────────────────────────── */

#define CMM_MARKER_START "<!-- codebase-memory-mcp:start -->"
#define CMM_MARKER_END "<!-- codebase-memory-mcp:end -->"

/* Read entire file into malloc'd buffer. Returns NULL on error. */
static char *read_file_str(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (long)CLI_MB_10 * CLI_MB_FACTOR) { /* cap at 10 MB */
        (void)fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)size + CLI_SKIP_ONE);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, CLI_ELEM_SIZE, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size) {
        nread = (size_t)size;
    }
    buf[nread] = '\0';
    if (out_len) {
        *out_len = nread;
    }
    return buf;
}

/* Write string to file, creating parent dirs if needed. */
static int write_file_str(const char *path, const char *content) {
    /* Ensure parent directory */
    char dir[CLI_BUF_1K];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdirp(dir, DIR_PERMS);
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return CLI_ERR;
    }
    size_t len = strlen(content);
    size_t written = fwrite(content, CLI_ELEM_SIZE, len, f);
    (void)fclose(f);
    return written == len ? 0 : CLI_ERR;
}

int cbm_upsert_instructions(const char *path, const char *content) {
    if (!path || !content) {
        return CLI_ERR;
    }

    size_t existing_len = 0;
    char *existing = read_file_str(path, &existing_len);

    /* Build the marker-wrapped section */
    size_t section_len = strlen(CMM_MARKER_START) + CLI_SKIP_ONE + strlen(content) +
                         strlen(CMM_MARKER_END) + CLI_SKIP_ONE;
    char *section = malloc(section_len + CLI_SKIP_ONE);
    if (!section) {
        free(existing);
        return CLI_ERR;
    }
    snprintf(section, section_len + SKIP_ONE, "%s\n%s%s\n", CMM_MARKER_START, content,
             CMM_MARKER_END);

    if (!existing) {
        /* File doesn't exist — create with just the section */
        int rc = write_file_str(path, section);
        free(section);
        return rc;
    }

    /* Check if markers already exist */
    char *start = strstr(existing, CMM_MARKER_START);
    char *end = start ? strstr(start, CMM_MARKER_END) : NULL;

    char *result;
    if (start && end) {
        /* Replace between markers (including markers themselves) */
        end += strlen(CMM_MARKER_END);
        /* Skip trailing newline after end marker */
        if (*end == '\n') {
            end++;
        }

        size_t prefix_len = (size_t)(start - existing);
        size_t suffix_len = strlen(end);
        size_t new_len = prefix_len + strlen(section) + suffix_len;
        result = malloc(new_len + CLI_SKIP_ONE);
        if (!result) {
            free(existing);
            free(section);
            return CLI_ERR;
        }
        memcpy(result, existing, prefix_len);
        memcpy(result + prefix_len, section, strlen(section));
        memcpy(result + prefix_len + strlen(section), end, suffix_len);
        result[new_len] = '\0';
    } else {
        /* Append section */
        size_t new_len = existing_len + CLI_SKIP_ONE + strlen(section);
        if (new_len > (size_t)CLI_MB_10 * CLI_MB_FACTOR) { /* 10 MB safety cap */
            free(existing);
            free(section);
            return CLI_ERR;
        }
        result = malloc(new_len + CLI_SKIP_ONE);
        if (!result) {
            free(existing);
            free(section);
            return CLI_ERR;
        }
        memcpy(result, existing, existing_len);
        result[existing_len] = '\n';
        memcpy(result + existing_len + SKIP_ONE, section, strlen(section));
        result[new_len] = '\0';
    }

    int rc = write_file_str(path, result);
    free(existing);
    free(section);
    free(result);
    return rc;
}

int cbm_remove_instructions(const char *path) {
    if (!path) {
        return CLI_ERR;
    }

    size_t len = 0;
    char *content = read_file_str(path, &len);
    if (!content) {
        return CLI_TRUE;
    }

    char *start = strstr(content, CMM_MARKER_START);
    char *end = start ? strstr(start, CMM_MARKER_END) : NULL;

    if (!start || !end) {
        free(content);
        return CLI_TRUE; /* not found */
    }

    end += strlen(CMM_MARKER_END);
    if (*end == '\n') {
        end++;
    }

    /* Also remove a leading newline before the start marker if present */
    if (start > content && *(start - CLI_SKIP_ONE) == '\n') {
        start--;
    }

    size_t prefix_len = (size_t)(start - content);
    size_t suffix_len = strlen(end);
    size_t new_len = prefix_len + suffix_len;
    char *result = malloc(new_len + CLI_SKIP_ONE);
    if (!result) {
        free(content);
        return CLI_ERR;
    }
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, end, suffix_len);
    result[new_len] = '\0';

    int rc = write_file_str(path, result);
    free(content);
    free(result);
    return rc;
}

/* ── Codex MCP config (TOML) ─────────────────────────────────── */

#define CODEX_CMM_SECTION "[mcp_servers.codebase-memory-mcp]"

int cbm_upsert_codex_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    size_t len = 0;
    char *content = read_file_str(config_path, &len);

    /* Build our TOML section */
    char section[CLI_BUF_1K];
    snprintf(section, sizeof(section), "%s\ncommand = \"%s\"\n", CODEX_CMM_SECTION, binary_path);

    if (!content) {
        /* No file — create fresh */
        return write_file_str(config_path, section);
    }

    /* Check if our section already exists */
    char *existing = strstr(content, CODEX_CMM_SECTION);
    if (existing) {
        /* Remove old section: from [mcp_servers.codebase-memory-mcp] to next [section] or EOF */
        char *section_end = existing + strlen(CODEX_CMM_SECTION);
        /* Find next [section] header */
        char *next_section = strstr(section_end, "\n[");
        if (next_section) {
            next_section++; /* keep the newline before next section */
        }

        size_t prefix_len = (size_t)(existing - content);
        const char *suffix = next_section ? next_section : "";
        size_t suffix_len = strlen(suffix);
        size_t new_len = prefix_len + strlen(section) + CLI_SKIP_ONE + suffix_len;
        char *result = malloc(new_len + CLI_SKIP_ONE);
        if (!result) {
            free(content);
            return CLI_ERR;
        }
        memcpy(result, content, prefix_len);
        memcpy(result + prefix_len, section, strlen(section));
        result[prefix_len + strlen(section)] = '\n';
        memcpy(result + prefix_len + strlen(section) + CLI_SKIP_ONE, suffix, suffix_len);
        result[new_len] = '\0';

        int rc = write_file_str(config_path, result);
        free(content);
        free(result);
        return rc;
    }

    /* Append our section */
    size_t new_len = len + CLI_SKIP_ONE + strlen(section);
    char *result = malloc(new_len + CLI_SKIP_ONE);
    if (!result) {
        free(content);
        return CLI_ERR;
    }
    memcpy(result, content, len);
    result[len] = '\n';
    memcpy(result + len + SKIP_ONE, section, strlen(section));
    result[new_len] = '\0';

    int rc = write_file_str(config_path, result);
    free(content);
    free(result);
    return rc;
}

int cbm_remove_codex_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    size_t len = 0;
    char *content = read_file_str(config_path, &len);
    if (!content) {
        return CLI_TRUE;
    }

    char *existing = strstr(content, CODEX_CMM_SECTION);
    if (!existing) {
        free(content);
        return CLI_TRUE;
    }

    char *section_end = existing + strlen(CODEX_CMM_SECTION);
    char *next_section = strstr(section_end, "\n[");
    if (next_section) {
        next_section++;
    }

    /* Remove leading newline if present */
    if (existing > content && *(existing - CLI_SKIP_ONE) == '\n') {
        existing--;
    }

    size_t prefix_len = (size_t)(existing - content);
    const char *suffix = next_section ? next_section : "";
    size_t suffix_len = strlen(suffix);
    size_t new_len = prefix_len + suffix_len;
    char *result = malloc(new_len + CLI_SKIP_ONE);
    if (!result) {
        free(content);
        return CLI_ERR;
    }
    memcpy(result, content, prefix_len);
    memcpy(result + prefix_len, suffix, suffix_len);
    result[new_len] = '\0';

    int rc = write_file_str(config_path, result);
    free(content);
    free(result);
    return rc;
}

/* ── SessionStart reminder hook (Codex / Gemini / Antigravity) ──────
 * Same methodology as the Claude Code SessionStart hook: a non-blocking
 * lifecycle hook whose stdout is injected as session context, reminding the
 * agent to use codebase-memory-mcp graph tools first. The command is written
 * so it is valid both inside a TOML single-quoted literal (Codex config.toml)
 * and a JSON string (Gemini settings.json) — i.e. it contains NO single quotes
 * and NO newlines. (issues #330 + Gemini/Antigravity parity) */
#define CMM_SESSION_REMINDER_CMD                                                    \
    "echo \"Code discovery: prefer codebase-memory-mcp (search_graph, trace_path, " \
    "get_code_snippet, query_graph, search_code) over grep/file-read; run "         \
    "index_repository first if the project is not indexed.\""

/* Sentinel-delimited block so upsert/remove are robust to the nested TOML
 * array-of-tables (which both start with '['). */
#define CODEX_HOOK_BEGIN "# >>> codebase-memory-mcp SessionStart >>>"
#define CODEX_HOOK_END "# <<< codebase-memory-mcp SessionStart <<<"

/* Splice out an existing [CODEX_HOOK_BEGIN .. CODEX_HOOK_END] block (inclusive,
 * plus a leading newline). Returns a newly-malloc'd string the caller frees, or
 * NULL if no block was present (content is left untouched). */
static char *codex_hook_strip(const char *content) {
    const char *begin = strstr(content, CODEX_HOOK_BEGIN);
    if (!begin) {
        return NULL;
    }
    const char *end = strstr(begin, CODEX_HOOK_END);
    if (!end) {
        return NULL;
    }
    end += strlen(CODEX_HOOK_END);
    if (*end == '\n') {
        end++;
    }
    /* Drop one leading newline before the block, if any. */
    const char *cut = begin;
    if (cut > content && *(cut - CLI_SKIP_ONE) == '\n') {
        cut--;
    }
    size_t prefix_len = (size_t)(cut - content);
    size_t suffix_len = strlen(end);
    char *out = malloc(prefix_len + suffix_len + CLI_SKIP_ONE);
    if (!out) {
        return NULL;
    }
    memcpy(out, content, prefix_len);
    memcpy(out + prefix_len, end, suffix_len);
    out[prefix_len + suffix_len] = '\0';
    return out;
}

/* Install/update the Codex SessionStart reminder hook in config.toml. */
int cbm_upsert_codex_hooks(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }
    char block[CLI_BUF_2K];
    snprintf(block, sizeof(block),
             "\n" CODEX_HOOK_BEGIN "\n"
             "[[hooks.SessionStart]]\n"
             "matcher = \"startup|resume|clear|compact\"\n\n"
             "[[hooks.SessionStart.hooks]]\n"
             "type = \"command\"\n"
             "command = '%s'\n" CODEX_HOOK_END "\n",
             CMM_SESSION_REMINDER_CMD);

    size_t len = 0;
    char *content = read_file_str(config_path, &len);
    if (!content) {
        return write_file_str(config_path, block + CLI_SKIP_ONE); /* skip leading newline */
    }
    char *stripped = codex_hook_strip(content);
    const char *base = stripped ? stripped : content;
    size_t base_len = strlen(base);
    char *result = malloc(base_len + strlen(block) + CLI_SKIP_ONE);
    if (!result) {
        free(content);
        free(stripped);
        return CLI_ERR;
    }
    memcpy(result, base, base_len);
    memcpy(result + base_len, block, strlen(block));
    result[base_len + strlen(block)] = '\0';
    int rc = write_file_str(config_path, result);
    free(content);
    free(stripped);
    free(result);
    return rc;
}

int cbm_remove_codex_hooks(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }
    size_t len = 0;
    char *content = read_file_str(config_path, &len);
    if (!content) {
        return CLI_TRUE;
    }
    char *stripped = codex_hook_strip(content);
    if (!stripped) {
        free(content);
        return CLI_TRUE; /* nothing to remove */
    }
    int rc = write_file_str(config_path, stripped);
    free(content);
    free(stripped);
    return rc;
}

/* ── OpenCode MCP config (JSON with "mcp" key) ───────────────── */

int cbm_upsert_opencode_mcp(const char *binary_path, const char *config_path) {
    if (!binary_path || !config_path) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Get or create "mcp" object */
    yyjson_mut_val *mcp = yyjson_mut_obj_get(root, "mcp");
    if (!mcp || !yyjson_mut_is_obj(mcp)) {
        mcp = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "mcp", mcp);
    }

    yyjson_mut_obj_remove_key(mcp, "codebase-memory-mcp");

    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_bool(mdoc, entry, "enabled", true);
    yyjson_mut_obj_add_str(mdoc, entry, "type", "local");
    yyjson_mut_val *cmd_arr = yyjson_mut_arr(mdoc);
    yyjson_mut_arr_add_str(mdoc, cmd_arr, binary_path);
    yyjson_mut_obj_add_val(mdoc, entry, "command", cmd_arr);
    yyjson_mut_obj_add_val(mdoc, mcp, "codebase-memory-mcp", entry);

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_remove_opencode_mcp(const char *config_path) {
    if (!config_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(config_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *mcp = yyjson_mut_obj_get(root, "mcp");
    if (!mcp || !yyjson_mut_is_obj(mcp)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_obj_remove_key(mcp, "codebase-memory-mcp");

    int rc = write_json_file(config_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── Antigravity MCP config (JSON, same mcpServers format) ────── */

int cbm_upsert_antigravity_mcp(const char *binary_path, const char *config_path) {
    /* Antigravity uses same mcpServers format as Cursor/Gemini */
    return cbm_install_editor_mcp(binary_path, config_path);
}

int cbm_remove_antigravity_mcp(const char *config_path) {
    return cbm_remove_editor_mcp(config_path);
}

/* ── Junie MCP config (JSON, same mcpServers format) ──────────── */

int cbm_upsert_junie_mcp(const char *binary_path, const char *config_path) {
    /* Junie (JetBrains) uses same mcpServers format as Cursor/Antigravity */
    return cbm_install_editor_mcp(binary_path, config_path);
}

int cbm_remove_junie_mcp(const char *config_path) {
    return cbm_remove_editor_mcp(config_path);
}

/* ── Claude Code pre-tool hooks ───────────────────────────────── */

/* Matcher includes Read for the indexing-coverage note (#963): when the agent
 * reads a file the indexer could not fully cover, the hook injects a warning
 * as additionalContext. The issue-#362 hazard (a GATING hook denying Read and
 * breaking the read-before-edit invariant) cannot recur: the augmenter is
 * structurally non-blocking — it always exits 0 and only ever ADDS context —
 * mirroring the Gemini matcher, which already includes read_file. */
#define CMM_HOOK_MATCHER "Grep|Glob|Read"
/* Basename only; the full command path is resolved at install time via
 * cbm_resolve_hook_command so $CLAUDE_CONFIG_DIR is honored. */
#define CMM_HOOK_GATE_SCRIPT "cbm-code-discovery-gate"
/* Hard backstop in settings.json; the binary also self-bounds with an
 * in-process deadline well under this. */
#define CMM_HOOK_TIMEOUT_SEC 5

/* Old matcher values from previous versions — recognized during upgrade so
 * upsert/remove can clean them up before inserting the current matcher.
 * Per-agent lists (no shared global): each caller passes its own. */
static const char *const cmm_claude_old_matchers[] = {
    "Grep|Glob|Read|Search",
    "Grep|Glob", /* pre-#963 matcher — Read re-added for the coverage note */
    NULL,
};
static const char *const cmm_gemini_old_matchers[] = {
    "google_search|read_file|grep_search",
    NULL,
};

/* Check if a hook array entry is ours (current matcher or a known old one).
 * When require_command_substr is non-NULL, the matcher match is not sufficient:
 * the entry must ALSO carry a hooks[].command containing that substring. This
 * disambiguates our entry from a user's own hook that happens to share the same
 * matcher (notably "*", which a user is likely to pick for a catch-all hook), so
 * upsert/remove never clobber a foreign entry. NULL preserves matcher-only
 * matching for callers whose matcher is already CMM-specific (e.g. "startup"). */
static bool is_cmm_hook_entry(yyjson_mut_val *entry, const char *matcher_str,
                              const char *const *old_matchers, const char *require_command_substr) {
    yyjson_mut_val *matcher = yyjson_mut_obj_get(entry, "matcher");
    if (!matcher || !yyjson_mut_is_str(matcher)) {
        return false;
    }
    const char *val = yyjson_mut_get_str(matcher);
    if (!val) {
        return false;
    }
    bool matcher_ok = strcmp(val, matcher_str) == 0;
    /* Also match old versions for backwards-compatible upgrade */
    for (int i = 0; !matcher_ok && old_matchers && old_matchers[i]; i++) {
        if (strcmp(val, old_matchers[i]) == 0) {
            matcher_ok = true;
        }
    }
    if (!matcher_ok) {
        return false;
    }
    if (require_command_substr) {
        yyjson_mut_val *hooks = yyjson_mut_obj_get(entry, "hooks");
        if (!hooks || !yyjson_mut_is_arr(hooks)) {
            return false;
        }
        size_t idx;
        size_t max;
        yyjson_mut_val *h;
        yyjson_mut_arr_foreach(hooks, idx, max, h) {
            yyjson_mut_val *cmd = yyjson_mut_obj_get(h, "command");
            if (cmd && yyjson_mut_is_str(cmd)) {
                const char *cs = yyjson_mut_get_str(cmd);
                if (cs && strstr(cs, require_command_substr)) {
                    return true;
                }
            }
        }
        return false;
    }
    return true;
}

/* Generic hook upsert for both Claude Code and Gemini CLI */

typedef struct {
    const char *settings_path;
    const char *hook_event;
    const char *matcher_str;
    const char *command_str;
    const char *const *old_matchers;  /* NULL-terminated; may be NULL */
    int timeout_sec;                  /* >0 adds "timeout" to the hook entry */
    const char *match_command_substr; /* non-NULL: also require this in the
                                       * entry command to claim ownership */
} hooks_upsert_args_t;
static int upsert_hooks_json(hooks_upsert_args_t args) {
    const char *settings_path = args.settings_path;
    const char *hook_event = args.hook_event;
    const char *matcher_str = args.matcher_str;
    const char *command_str = args.command_str;
    const char *const *old_matchers = args.old_matchers;
    if (!settings_path) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(settings_path);
    yyjson_mut_val *root;
    if (doc) {
        root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
    } else {
        root = yyjson_mut_obj(mdoc);
    }
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    /* Get or create hooks object */
    yyjson_mut_val *hooks = yyjson_mut_obj_get(root, "hooks");
    if (!hooks || !yyjson_mut_is_obj(hooks)) {
        hooks = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_val(mdoc, root, "hooks", hooks);
    }

    /* Get or create the hook event array (e.g. PreToolUse / BeforeTool) */
    yyjson_mut_val *event_arr = yyjson_mut_obj_get(hooks, hook_event);
    if (!event_arr || !yyjson_mut_is_arr(event_arr)) {
        event_arr = yyjson_mut_arr(mdoc);
        yyjson_mut_obj_add_val(mdoc, hooks, hook_event, event_arr);
    }

    /* Remove existing CMM entry if present */
    size_t idx;
    size_t max;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(event_arr, idx, max, item) {
        if (is_cmm_hook_entry(item, matcher_str, old_matchers, args.match_command_substr)) {
            yyjson_mut_arr_remove(event_arr, idx);
            break;
        }
    }

    /* Build our hook entry */
    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, entry, "matcher", matcher_str);

    yyjson_mut_val *hooks_arr = yyjson_mut_arr(mdoc);
    yyjson_mut_val *hook_obj = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_str(mdoc, hook_obj, "type", "command");
    yyjson_mut_obj_add_str(mdoc, hook_obj, "command", command_str);
    if (args.timeout_sec > 0) {
        yyjson_mut_obj_add_int(mdoc, hook_obj, "timeout", args.timeout_sec);
    }
    yyjson_mut_arr_append(hooks_arr, hook_obj);
    yyjson_mut_obj_add_val(mdoc, entry, "hooks", hooks_arr);

    yyjson_mut_arr_append(event_arr, entry);

    int rc = write_json_file(settings_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* Generic hook remove for both Claude Code and Gemini CLI */

typedef struct {
    const char *settings_path;
    const char *hook_event;
    const char *matcher_str;
    const char *const *old_matchers;  /* NULL-terminated; may be NULL */
    const char *match_command_substr; /* non-NULL: also require this in the
                                       * entry command to claim ownership */
} hooks_remove_args_t;
static int remove_hooks_json(hooks_remove_args_t args) {
    const char *settings_path = args.settings_path;
    const char *hook_event = args.hook_event;
    const char *matcher_str = args.matcher_str;
    const char *const *old_matchers = args.old_matchers;
    if (!settings_path) {
        return CLI_ERR;
    }

    yyjson_doc *doc = read_json_file(settings_path);
    if (!doc) {
        return CLI_ERR;
    }

    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!root) {
        yyjson_mut_doc_free(mdoc);
        return CLI_ERR;
    }
    yyjson_mut_doc_set_root(mdoc, root);

    yyjson_mut_val *hooks = yyjson_mut_obj_get(root, "hooks");
    if (!hooks) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    yyjson_mut_val *event_arr = yyjson_mut_obj_get(hooks, hook_event);
    if (!event_arr || !yyjson_mut_is_arr(event_arr)) {
        yyjson_mut_doc_free(mdoc);
        return 0;
    }

    size_t idx;
    size_t max;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(event_arr, idx, max, item) {
        if (is_cmm_hook_entry(item, matcher_str, old_matchers, args.match_command_substr)) {
            yyjson_mut_arr_remove(event_arr, idx);
            break;
        }
    }

    /* Prune the event key once its array is empty, so removing our hook leaves
     * no stale "<Event>": [] cruft behind. */
    if (yyjson_mut_arr_size(event_arr) == 0) {
        yyjson_mut_obj_remove_key(hooks, hook_event);
    }

    int rc = write_json_file(settings_path, mdoc);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int cbm_upsert_claude_hooks(const char *settings_path) {
    char command[CLI_BUF_1K];
    cbm_resolve_hook_command(CMM_HOOK_GATE_SCRIPT, command, sizeof(command));
    return upsert_hooks_json((hooks_upsert_args_t){
        .settings_path = settings_path,
        .hook_event = "PreToolUse",
        .matcher_str = CMM_HOOK_MATCHER,
        .command_str = command,
        .old_matchers = cmm_claude_old_matchers,
        .timeout_sec = CMM_HOOK_TIMEOUT_SEC,
    });
}

int cbm_remove_claude_hooks(const char *settings_path) {
    return remove_hooks_json((hooks_remove_args_t){
        .settings_path = settings_path,
        .hook_event = "PreToolUse",
        .matcher_str = CMM_HOOK_MATCHER,
        .old_matchers = cmm_claude_old_matchers,
    });
}

/* Install the search-augmenter shim to ~/.claude/hooks/.
 * The shim is a thin wrapper that delegates to `<binary> hook-augment`,
 * which adds graph context to Grep/Glob calls. It NEVER blocks a tool call:
 * a missing/old/hung binary results in a silent exit 0 (issue #362/#288).
 * The legacy filename `cbm-code-discovery-gate` is retained so existing
 * settings.json entries and uninstall keep working with zero migration. */
void cbm_install_hook_gate_script(const char *home, const char *binary_path) {
    if (!home || !binary_path) {
        return;
    }
    /* Defensive: refuse to embed a binary path containing a double-quote, which
     * would break the BIN="..." shell quoting in the generated shim. In normal
     * installs this is unreachable (paths come from cbm_detect_self_path), but
     * fail-loud here beats silently emitting a malformed script. */
    if (strchr(binary_path, '"') != NULL) {
        return;
    }
    char config_dir[CLI_BUF_1K];
    cbm_claude_config_dir(home, config_dir, sizeof(config_dir));
    if (!config_dir[0]) {
        return;
    }
    char hooks_dir[CLI_BUF_1K];
    snprintf(hooks_dir, sizeof(hooks_dir), "%s/hooks", config_dir);
    cbm_mkdir_p(hooks_dir, CLI_OCTAL_PERM);

    char script_path[CLI_BUF_1K];
    snprintf(script_path, sizeof(script_path), "%s/" CMM_HOOK_GATE_SCRIPT, hooks_dir);

    FILE *f = fopen(script_path, "w");
    if (!f) {
        return;
    }
    (void)fprintf(f,
                  "#!/usr/bin/env bash\n"
                  "# codebase-memory-mcp search augmenter (Claude Code PreToolUse).\n"
                  "# NOTE: the legacy filename is kept for zero-migration upgrades.\n"
                  "# Despite the name this NEVER blocks a tool call - it only adds\n"
                  "# graph context. Any failure is silent (exit 0, no output).\n"
                  "BIN=\"%s\"\n"
                  "[ -x \"$BIN\" ] || exit 0\n"
                  "\"$BIN\" hook-augment 2>/dev/null\n"
                  "exit 0\n",
                  binary_path);
    /* fchmod before close to avoid TOCTOU race (CodeQL cpp/toctou-race-condition) */
#ifndef _WIN32
    fchmod(fileno(f), CLI_OCTAL_PERM);
#endif
    (void)fclose(f);
#ifdef _WIN32
    chmod(script_path, CLI_OCTAL_PERM);
#endif
}

/* SessionStart hook: remind agent to use MCP tools on every context reset. */
#define CMM_SESSION_REMINDER_SCRIPT "cbm-session-reminder"

static void cbm_install_session_reminder_script(const char *home) {
    if (!home) {
        return;
    }
    char config_dir[CLI_BUF_1K];
    cbm_claude_config_dir(home, config_dir, sizeof(config_dir));
    if (!config_dir[0]) {
        return;
    }
    char hooks_dir[CLI_BUF_1K];
    snprintf(hooks_dir, sizeof(hooks_dir), "%s/hooks", config_dir);
    cbm_mkdir_p(hooks_dir, CLI_OCTAL_PERM);

    char script_path[CLI_BUF_1K];
    snprintf(script_path, sizeof(script_path), "%s/" CMM_SESSION_REMINDER_SCRIPT, hooks_dir);

    FILE *f = fopen(script_path, "w");
    if (!f) {
        return;
    }
    (void)fprintf(
        f, "#!/usr/bin/env bash\n"
           "# SessionStart hook: remind agent to use codebase-memory-mcp tools.\n"
           "# Installed by codebase-memory-mcp. Fires on startup/resume/clear/compact.\n"
           "cat << 'REMINDER'\n"
           "CRITICAL - Code Discovery Protocol:\n"
           "1. ALWAYS use codebase-memory-mcp tools FIRST for ANY code exploration:\n"
           "   - search_graph(name_pattern/label/qn_pattern) to find functions/classes/routes\n"
           "   - trace_path(function_name, mode=calls|data_flow|cross_service) for call chains\n"
           "   - get_code_snippet(qualified_name) for exact symbol source (precise ranges)\n"
           "   - query_graph(query) for complex Cypher patterns\n"
           "   - get_architecture(aspects) for project structure\n"
           "   - search_code(pattern) for text search (graph-augmented grep)\n"
           "2. Use Grep/Glob/Read freely for text, configs, non-code files, and\n"
           "   always Read a file before editing it.\n"
           "3. If a project is not indexed yet, run index_repository FIRST.\n"
           "REMINDER\n");
#ifndef _WIN32
    fchmod(fileno(f), CLI_OCTAL_PERM);
#endif
    (void)fclose(f);
#ifdef _WIN32
    chmod(script_path, CLI_OCTAL_PERM);
#endif
}

static int cbm_upsert_session_hooks(const char *settings_path) {
    static const char *matchers[] = {"startup", "resume", "clear", "compact"};
    char command[CLI_BUF_1K];
    cbm_resolve_hook_command(CMM_SESSION_REMINDER_SCRIPT, command, sizeof(command));
    int rc = 0;
    for (int i = 0; i < NUM_DIRS; i++) {
        if (upsert_hooks_json((hooks_upsert_args_t){.settings_path = settings_path,
                                                    .hook_event = "SessionStart",
                                                    .matcher_str = matchers[i],
                                                    .command_str = command}) != 0) {
            rc = CLI_ERR;
        }
    }
    return rc;
}

static int cbm_remove_session_hooks(const char *settings_path) {
    static const char *matchers[] = {"startup", "resume", "clear", "compact"};
    int rc = 0;
    for (int i = 0; i < NUM_DIRS; i++) {
        if (remove_hooks_json((hooks_remove_args_t){.settings_path = settings_path,
                                                    .hook_event = "SessionStart",
                                                    .matcher_str = matchers[i]}) != 0) {
            rc = CLI_ERR;
        }
    }
    return rc;
}

/* SubagentStart hook: subagents spawned via the Agent tool do NOT fire
 * SessionStart, so the SessionStart reminder above never reaches them. This
 * hook is their equivalent. Unlike SessionStart (where plain stdout is injected
 * as context), SubagentStart injects context only via a JSON object on stdout:
 *   {"hookSpecificOutput":{"hookEventName":"SubagentStart","additionalContext":"…"}}
 * The text is a leaner variant of the SessionStart protocol: it omits the
 * "run index_repository first" step, since the parent session has already
 * indexed the project. Matcher "*" fires for every agent type. */
#define CMM_SUBAGENT_REMINDER_SCRIPT "cbm-subagent-reminder"

static void cbm_install_subagent_reminder_script(const char *home) {
    if (!home) {
        return;
    }
    char config_dir[CLI_BUF_1K];
    cbm_claude_config_dir(home, config_dir, sizeof(config_dir));
    if (!config_dir[0]) {
        return;
    }
    char hooks_dir[CLI_BUF_1K];
    snprintf(hooks_dir, sizeof(hooks_dir), "%s/hooks", config_dir);
    cbm_mkdir_p(hooks_dir, CLI_OCTAL_PERM);

    char script_path[CLI_BUF_1K];
    snprintf(script_path, sizeof(script_path), "%s/" CMM_SUBAGENT_REMINDER_SCRIPT, hooks_dir);

    FILE *f = fopen(script_path, "w");
    if (!f) {
        return;
    }
    /* The additionalContext value is a single line with no embedded quotes,
     * backslashes, or newlines, so the JSON below is valid as written — no
     * runtime escaping (and no python3/jq dependency) is required. */
    (void)fprintf(f,
                  "#!/usr/bin/env bash\n"
                  "# SubagentStart hook: tell subagents to use codebase-memory-mcp tools.\n"
                  "# Installed by codebase-memory-mcp. Fires when any subagent is spawned.\n"
                  "# SubagentStart injects context via JSON additionalContext, not plain stdout.\n"
                  "cat << 'REMINDER'\n"
                  "{\"hookSpecificOutput\":{\"hookEventName\":\"SubagentStart\","
                  "\"additionalContext\":\"Code discovery: prefer codebase-memory-mcp tools "
                  "(search_graph, trace_path, get_code_snippet, query_graph, get_architecture, "
                  "search_code) over grep/file-read for navigating code. Use Grep/Glob/Read for "
                  "text, configs, and non-code files.\"}}\n"
                  "REMINDER\n");
#ifndef _WIN32
    fchmod(fileno(f), CLI_OCTAL_PERM);
#endif
    (void)fclose(f);
#ifdef _WIN32
    chmod(script_path, CLI_OCTAL_PERM);
#endif
}

int cbm_upsert_claude_subagent_hooks(const char *settings_path) {
    char command[CLI_BUF_1K];
    cbm_resolve_hook_command(CMM_SUBAGENT_REMINDER_SCRIPT, command, sizeof(command));
    /* matcher "*" is the natural choice a user would also pick for their own
     * catch-all SubagentStart hook, so claim ownership by command too — never
     * clobber or remove a foreign "*" entry. */
    return upsert_hooks_json(
        (hooks_upsert_args_t){.settings_path = settings_path,
                              .hook_event = "SubagentStart",
                              .matcher_str = "*",
                              .command_str = command,
                              .match_command_substr = CMM_SUBAGENT_REMINDER_SCRIPT});
}

int cbm_remove_claude_subagent_hooks(const char *settings_path) {
    return remove_hooks_json(
        (hooks_remove_args_t){.settings_path = settings_path,
                              .hook_event = "SubagentStart",
                              .matcher_str = "*",
                              .match_command_substr = CMM_SUBAGENT_REMINDER_SCRIPT});
}

/* Matcher excludes read_file for consistency with the Claude fix: the hook
 * is an advisory reminder, not a gate over the agent's file reads. */
#define GEMINI_HOOK_MATCHER "google_search|grep_search"
#define GEMINI_HOOK_COMMAND                                               \
    "echo 'Reminder: prefer codebase-memory-mcp search_graph/trace_path/" \
    "get_code_snippet over grep/file search for code discovery.' >&2"

int cbm_upsert_gemini_hooks(const char *settings_path) {
    return upsert_hooks_json((hooks_upsert_args_t){
        .settings_path = settings_path,
        .hook_event = "BeforeTool",
        .matcher_str = GEMINI_HOOK_MATCHER,
        .command_str = GEMINI_HOOK_COMMAND,
        .old_matchers = cmm_gemini_old_matchers,
    });
}

int cbm_remove_gemini_hooks(const char *settings_path) {
    return remove_hooks_json((hooks_remove_args_t){
        .settings_path = settings_path,
        .hook_event = "BeforeTool",
        .matcher_str = GEMINI_HOOK_MATCHER,
        .old_matchers = cmm_gemini_old_matchers,
    });
}

/* Gemini CLI / Antigravity SessionStart reminder. settings.json uses the same
 * hooks.<Event>[].hooks[] JSON shape as Claude, so it reuses upsert_hooks_json.
 * The SessionStart matcher is advisory in Gemini (it does not filter lifecycle
 * sources), so a single "startup" entry fires on startup/resume/clear. The
 * command's stdout is injected as session context. (Gemini/Antigravity parity
 * with the Claude/Codex SessionStart reminder.) */
int cbm_upsert_gemini_session_hooks(const char *settings_path) {
    return upsert_hooks_json((hooks_upsert_args_t){
        .settings_path = settings_path,
        .hook_event = "SessionStart",
        .matcher_str = "startup",
        .command_str = CMM_SESSION_REMINDER_CMD,
    });
}

int cbm_remove_gemini_session_hooks(const char *settings_path) {
    return remove_hooks_json((hooks_remove_args_t){
        .settings_path = settings_path,
        .hook_event = "SessionStart",
        .matcher_str = "startup",
    });
}

/* ── PATH management ──────────────────────────────────────────── */

int cbm_ensure_path(const char *bin_dir, const char *rc_file, bool dry_run) {
    if (!bin_dir || !rc_file) {
        return CLI_ERR;
    }

    /* fish uses a different syntax than POSIX shells: `export PATH="...:$PATH"`
     * is a syntax error in fish and breaks config.fish (#319). When the target
     * is a fish config, emit the fish-native `fish_add_path` (idempotent,
     * prepends only if absent) instead. */
    size_t rc_len = strlen(rc_file);
    bool is_fish = rc_len >= CBM_SZ_5 && strcmp(rc_file + rc_len - CBM_SZ_5, ".fish") == 0;

    char line[CLI_BUF_1K];
    if (is_fish) {
        snprintf(line, sizeof(line), "fish_add_path %s", bin_dir);
    } else {
        snprintf(line, sizeof(line), "export PATH=\"%s:$PATH\"", bin_dir);
    }

    /* Check if already present in rc file */
    FILE *f = fopen(rc_file, "r");
    if (f) {
        char buf[CLI_BUF_2K];
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, line)) {
                (void)fclose(f);
                return CLI_TRUE; /* already present */
            }
        }
        (void)fclose(f);
    }

    if (dry_run) {
        return 0;
    }

    f = fopen(rc_file, "a");
    if (!f) {
        return CLI_ERR;
    }

    (void)fprintf(f, "\n# Added by codebase-memory-mcp install\n%s\n", line);
    (void)fclose(f);
    return 0;
}

/* ── Tar.gz extraction ────────────────────────────────────────── */

/* Decompress gzip data into a malloc'd buffer. Returns NULL on failure.
 * *out_total receives the decompressed size. Caller must free the result. */
static unsigned char *gzip_decompress(const unsigned char *data, int data_len, size_t *out_total) {
    z_stream strm = {0};
    unsigned char *mutable_data;
    memcpy(&mutable_data, &data, sizeof(data));
    strm.next_in = mutable_data;
    strm.avail_in = (unsigned int)data_len;

    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        return NULL;
    }

    size_t buf_cap = (size_t)data_len * DECOMP_FACTOR;
    if (buf_cap < CLI_BUF_4K) {
        buf_cap = CLI_BUF_4K;
    }
    if (buf_cap > DECOMPRESS_MAX_BYTES) {
        buf_cap = DECOMPRESS_MAX_BYTES;
    }
    unsigned char *decompressed = malloc(buf_cap);
    if (!decompressed) {
        inflateEnd(&strm);
        return NULL;
    }

    size_t total = 0;
    int ret;
    do {
        if (total >= buf_cap) {
            size_t new_cap = buf_cap * GROWTH_FACTOR;
            if (new_cap > DECOMPRESS_MAX_BYTES) {
                free(decompressed);
                inflateEnd(&strm);
                return NULL;
            }
            unsigned char *nb = realloc(decompressed, new_cap);
            if (!nb) {
                free(decompressed);
                inflateEnd(&strm);
                return NULL;
            }
            decompressed = nb;
            buf_cap = new_cap;
        }
        strm.next_out = decompressed + total;
        strm.avail_out = (unsigned int)(buf_cap - total);
        ret = inflate(&strm, Z_NO_FLUSH);
        total = buf_cap - strm.avail_out;
    } while (ret == Z_OK);

    inflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        free(decompressed);
        return NULL;
    }
    *out_total = total;
    return decompressed;
}

/* Check if a tar block is all zeros (end of archive). */
static bool is_tar_end_of_archive(const unsigned char *hdr) {
    for (int i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (hdr[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Try to extract the target binary from a tar entry. Returns malloc'd data or NULL. */
static unsigned char *tar_try_extract_binary(const unsigned char *hdr, char typeflag,
                                             const char *name, const unsigned char *archive,
                                             size_t data_pos, long file_size, size_t total,
                                             int *out_len) {
    (void)hdr;
    if (typeflag != '0' && typeflag != '\0') {
        return NULL;
    }
    const char *basename = strrchr(name, '/');
    basename = basename ? basename + CLI_SKIP_ONE : name;
    if (strncmp(basename, TAR_BINARY_NAME, TAR_BINARY_NAME_LEN) != 0) {
        return NULL;
    }
    if (data_pos + (size_t)file_size > total) {
        return NULL;
    }
    unsigned char *result = malloc((size_t)file_size);
    if (!result) {
        return NULL;
    }
    memcpy(result, archive + data_pos, (size_t)file_size);
    *out_len = (int)file_size;
    return result;
}

unsigned char *cbm_extract_binary_from_targz(const unsigned char *data, int data_len,
                                             int *out_len) {
    if (!data || data_len <= 0 || !out_len) {
        return NULL;
    }

    size_t total = 0;
    unsigned char *decompressed = gzip_decompress(data, data_len, &total);
    if (!decompressed) {
        return NULL;
    }

    /* Parse tar: find entry starting with "codebase-memory-mcp" */
    size_t pos = 0;
    while (pos + TAR_BLOCK_SIZE <= total) {
        const unsigned char *hdr = decompressed + pos;

        if (is_tar_end_of_archive(hdr)) {
            break;
        }

        char name[TAR_NAME_LEN] = {0};
        memcpy(name, hdr, TAR_NAME_LEN - SKIP_ONE);
        char size_str[TAR_SIZE_LEN] = {0};
        memcpy(size_str, hdr + TAR_SIZE_OFFSET, TAR_SIZE_LEN - SKIP_ONE);
        long file_size = strtol(size_str, NULL, OCTAL_BASE);
        char typeflag = (char)hdr[TAR_TYPE_OFFSET];
        pos += TAR_BLOCK_SIZE;

        unsigned char *found = tar_try_extract_binary(hdr, typeflag, name, decompressed, pos,
                                                      file_size, total, out_len);
        if (found) {
            free(decompressed);
            return found;
        }

        size_t blocks = ((size_t)file_size + TAR_BLOCK_MASK) / TAR_BLOCK_SIZE;
        pos += blocks * TAR_BLOCK_SIZE;
    }

    free(decompressed);
    return NULL; /* binary not found */
}

/* ── Zip extraction (in-memory, replaces external unzip) ──────── */

/* Zip local file header constants */
enum {
    ZIP_SIG_0 = 0x50,
    ZIP_SIG_1 = 0x4B,
    ZIP_SIG_2 = 0x03,
    ZIP_SIG_3 = 0x04,
    ZIP_HDR_SZ = 30,
    ZIP_OFF_METHOD = 8,
    ZIP_OFF_COMP = 18,
    ZIP_OFF_UNCOMP = 22,
    ZIP_OFF_NAMELEN = 26,
    ZIP_OFF_EXTRALEN = 28,
    ZIP_STORED = 0,
    ZIP_DEFLATE = 8
};
static const size_t ZIP_MAX_UNCOMP = 500U * 1024U * 1024U;

static uint16_t zip_read_u16le(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << BYTE_SHIFT));
}

static uint32_t zip_read_u32le(const unsigned char *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << BYTE_SHIFT) |
           ((uint32_t)p[2] << (BYTE_SHIFT * CLI_PAIR_LEN)) |
           ((uint32_t)p[3] << (BYTE_SHIFT * CLI_JSON_INDENT));
}

/* Decompress a single zip entry (stored or deflated). Returns malloc'd buffer
 * or NULL on failure. *out_len receives the decompressed size. */
static unsigned char *zip_extract_entry(const unsigned char *file_data, uint16_t method,
                                        size_t comp_size, size_t uncomp_size, int *out_len) {
    if (method == ZIP_STORED) {
        if (comp_size > ZIP_MAX_UNCOMP) {
            return NULL;
        }
        unsigned char *out = malloc(comp_size);
        if (!out) {
            return NULL;
        }
        memcpy(out, file_data, comp_size);
        *out_len = (int)comp_size;
        return out;
    }
    if (method == ZIP_DEFLATE) {
        if (uncomp_size > ZIP_MAX_UNCOMP) {
            return NULL;
        }
        if (comp_size > UINT_MAX || uncomp_size > UINT_MAX) {
            return NULL;
        }
        unsigned char *out = malloc(uncomp_size);
        if (!out) {
            return NULL;
        }
        z_stream strm = {0};
        strm.next_in = (unsigned char *)file_data;
        strm.avail_in = (uInt)comp_size;
        strm.next_out = out;
        strm.avail_out = (uInt)uncomp_size;
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
            free(out);
            return NULL;
        }
        int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END) {
            free(out);
            return NULL;
        }
        *out_len = (int)strm.total_out;
        return out;
    }
    return NULL; /* unknown method */
}

unsigned char *cbm_extract_binary_from_zip(const unsigned char *data, int data_len, int *out_len) {
    if (!data || data_len <= 0 || !out_len) {
        return NULL;
    }
    *out_len = 0;

    int pos = 0;
    while (pos + ZIP_HDR_SZ <= data_len) {
        if (data[pos] != ZIP_SIG_0 || data[pos + CLI_SKIP_ONE] != ZIP_SIG_1 ||
            data[pos + CLI_PAIR_LEN] != ZIP_SIG_2 || data[pos + CLI_JSON_INDENT] != ZIP_SIG_3) {
            break;
        }

        uint16_t method = zip_read_u16le(data + pos + ZIP_OFF_METHOD);
        uint32_t comp_size = zip_read_u32le(data + pos + ZIP_OFF_COMP);
        uint32_t uncomp_size = zip_read_u32le(data + pos + ZIP_OFF_UNCOMP);
        uint16_t name_len = zip_read_u16le(data + pos + ZIP_OFF_NAMELEN);
        uint16_t extra_len = zip_read_u16le(data + pos + ZIP_OFF_EXTRALEN);

        int header_end = pos + ZIP_HDR_SZ + name_len + extra_len;
        if (header_end > data_len || comp_size > (uint32_t)(data_len - header_end)) {
            break;
        }

        char fname[CLI_BUF_512] = {0};
        int fn_copy = name_len < (int)sizeof(fname) - CLI_SKIP_ONE
                          ? name_len
                          : (int)sizeof(fname) - CLI_SKIP_ONE;
        memcpy(fname, data + pos + 30, (size_t)fn_copy);
        fname[fn_copy] = '\0';

        if (strstr(fname, "..")) {
            pos = header_end + (int)comp_size;
            continue;
        }

        const char *basename = strrchr(fname, '/');
        basename = basename ? basename + CLI_SKIP_ONE : fname;

        if (strcmp(basename, "codebase-memory-mcp") == 0 ||
            strcmp(basename, "codebase-memory-mcp.exe") == 0) {
            return zip_extract_entry(data + header_end, method, comp_size, uncomp_size, out_len);
        }

        pos = header_end + (int)comp_size;
    }

    return NULL;
}

/* ── Index management ─────────────────────────────────────────── */

static const char *get_cache_dir(const char *home_dir) {
    static char buf[CLI_BUF_1K];
    char override[CLI_BUF_1K] = "";
    /* An unset override selects the explicit/default home path. A present
     * value that does not fit must fail closed instead of silently selecting
     * and potentially deleting the default cache. */
    if (!cbm_safe_getenv("CBM_CACHE_DIR", override, sizeof(override), "")) {
        return NULL;
    }
    const char *resolved = NULL;
    if (override[0]) {
        resolved = override;
    } else if (home_dir && home_dir[0]) {
        int n = snprintf(buf, sizeof(buf), "%s/.cache/codebase-memory-mcp", home_dir);
        if (n <= 0 || n >= (int)sizeof(buf)) {
            return NULL;
        }
        cbm_normalize_path_sep(buf);
        return buf;
    } else {
        resolved = cbm_resolve_cache_dir();
    }
    if (!resolved) {
        return NULL;
    }
    int n = snprintf(buf, sizeof(buf), "%s", resolved);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return NULL;
    }
    cbm_normalize_path_sep(buf);
    return buf;
}

static bool cli_is_project_db_file(const cbm_dirent_t *entry) {
    if (!entry || entry->is_dir || !entry->name[0] || entry->name[0] == '_') {
        return false;
    }
    size_t len = strlen(entry->name);
    return len > DB_EXT_LEN && strcmp(entry->name + len - DB_EXT_LEN, ".db") == 0;
}

static char *cli_cache_entry_path(const char *cache_dir, const char *name) {
    if (!cache_dir || !name) {
        return NULL;
    }
    size_t dir_len = strlen(cache_dir);
    size_t name_len = strlen(name);
    if (dir_len > SIZE_MAX - name_len - CLI_PAIR_LEN) {
        return NULL;
    }
    char *path = malloc(dir_len + name_len + CLI_PAIR_LEN);
    if (!path) {
        return NULL;
    }
    memcpy(path, cache_dir, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + CLI_SKIP_ONE, name, name_len + CLI_SKIP_ONE);
    return path;
}

int cbm_list_indexes(const char *home_dir) {
    const char *cache_dir = get_cache_dir(home_dir);
    if (!cache_dir) {
        return CLI_ERR;
    }

    cbm_dir_t *d = cbm_opendir(cache_dir);
    if (!d) {
        return 0;
    }

    int count = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (cli_is_project_db_file(ent)) {
            printf("  %s/%s\n", cache_dir, ent->name);
            count++;
        }
    }
    bool failed = cbm_dir_had_error(d);
    cbm_closedir(d);
    return failed ? CLI_ERR : count;
}

int cbm_remove_indexes(const char *home_dir) {
    const char *cache_dir = get_cache_dir(home_dir);
    if (!cache_dir) {
        return CLI_ERR;
    }

    cbm_dir_t *d = cbm_opendir(cache_dir);
    if (!d) {
        return 0;
    }

    int count = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (!cli_is_project_db_file(ent)) {
            continue;
        }
        char *path = cli_cache_entry_path(cache_dir, ent->name);
        if (!path) {
            cbm_closedir(d);
            return CLI_ERR;
        }
        size_t path_len = strlen(path);
        char *tmp_path =
            path_len <= SIZE_MAX - sizeof(".tmp") ? malloc(path_len + sizeof(".tmp")) : NULL;
        if (tmp_path) {
            memcpy(tmp_path, path, path_len);
            memcpy(tmp_path + path_len, ".tmp", sizeof(".tmp"));
            (void)cbm_unlink(tmp_path);
            free(tmp_path);
        }
        if (cbm_unlink(path) == 0) {
            cbm_remove_db_sidecars(path);
            count++;
        }
        free(path);
    }
    bool failed = cbm_dir_had_error(d);
    cbm_closedir(d);
    return failed ? CLI_ERR : count;
}

/* ── Config store (persistent key-value in _config.db) ─────────── */

#include <sqlite3.h>

struct cbm_config {
    sqlite3 *db;
    char get_buf[CLI_BUF_4K]; /* static buffer for cbm_config_get return values */
};

cbm_config_t *cbm_config_open(const char *cache_dir) {
    if (!cache_dir) {
        return NULL;
    }

    char dbpath[CLI_BUF_1K];
    snprintf(dbpath, sizeof(dbpath), "%s/_config.db", cache_dir);

    /* Ensure directory exists */
    mkdirp(cache_dir, DIR_PERMS);

    sqlite3 *db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return NULL;
    }

    /* Create table if not exists */
    const char *sql = "CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT)";
    char *err_msg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return NULL;
    }

    cbm_config_t *cfg = calloc(CBM_ALLOC_ONE, sizeof(*cfg));
    if (!cfg) {
        sqlite3_close(db);
        return NULL;
    }
    cfg->db = db;
    return cfg;
}

void cbm_config_close(cbm_config_t *cfg) {
    if (!cfg) {
        return;
    }
    if (cfg->db) {
        sqlite3_close(cfg->db);
    }
    free(cfg);
}

const char *cbm_config_get(cbm_config_t *cfg, const char *key, const char *default_val) {
    if (!cfg || !key) {
        return default_val;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cfg->db, "SELECT value FROM config WHERE key = ?", SQL_NUL_TERM, &stmt,
                           NULL) != SQLITE_OK) {
        return default_val;
    }
    sqlite3_bind_text(stmt, SQL_PARAM_1, key, SQL_NUL_TERM, cbm_sqlite_transient);

    const char *result = default_val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) {
            snprintf(cfg->get_buf, sizeof(cfg->get_buf), "%s", val);
            result = cfg->get_buf;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

bool cbm_config_get_bool(cbm_config_t *cfg, const char *key, bool default_val) {
    const char *val = cbm_config_get(cfg, key, NULL);
    if (!val) {
        return default_val;
    }
    if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcmp(val, "on") == 0) {
        return true;
    }
    if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcmp(val, "off") == 0) {
        return false;
    }
    return default_val;
}

int cbm_config_get_int(cbm_config_t *cfg, const char *key, int default_val) {
    const char *val = cbm_config_get(cfg, key, NULL);
    if (!val) {
        return default_val;
    }
    char *endptr;
    long v = strtol(val, &endptr, CLI_STRTOL_BASE);
    if (endptr == val || *endptr != '\0') {
        return default_val;
    }
    return (int)v;
}

int cbm_config_set(cbm_config_t *cfg, const char *key, const char *value) {
    if (!cfg || !key || !value) {
        return CLI_ERR;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cfg->db, "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)",
                           SQL_NUL_TERM, &stmt, NULL) != SQLITE_OK) {
        return CLI_ERR;
    }
    sqlite3_bind_text(stmt, SQL_PARAM_1, key, SQL_NUL_TERM, cbm_sqlite_transient);
    sqlite3_bind_text(stmt, SQL_PARAM_2, value, SQL_NUL_TERM, cbm_sqlite_transient);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : CLI_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

int cbm_config_delete(cbm_config_t *cfg, const char *key) {
    if (!cfg || !key) {
        return CLI_ERR;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(cfg->db, "DELETE FROM config WHERE key = ?", SQL_NUL_TERM, &stmt,
                           NULL) != SQLITE_OK) {
        return CLI_ERR;
    }
    sqlite3_bind_text(stmt, SQL_PARAM_1, key, SQL_NUL_TERM, cbm_sqlite_transient);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : CLI_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

/* ── Config CLI subcommand ────────────────────────────────────── */

int cbm_cmd_config(int argc, char **argv) {
    if (argc == 0) {
        printf("Usage: codebase-memory-mcp config <command> [args]\n\n");
        printf("Commands:\n");
        printf("  list             Show all config values\n");
        printf("  get <key>        Get a config value\n");
        printf("  set <key> <val>  Set a config value\n");
        printf("  reset <key>      Reset a key to default\n\n");
        printf("Config keys:\n");
        printf("  %-25s  default=%-10s  %s\n", CBM_CONFIG_AUTO_INDEX, "false",
               "Enable auto-indexing on MCP session start");
        printf("  %-25s  default=%-10s  %s\n", CBM_CONFIG_AUTO_INDEX_LIMIT, "50000",
               "Max files for auto-indexing new projects");
        printf("  %-25s  default=%-10s  %s\n", CBM_CONFIG_AUTO_WATCH, "true",
               "Register background git watcher on session connect");
        printf("  %-25s  default=%-10s  %s\n", CBM_CONFIG_UI_LANG, "auto",
               "Pin graph UI language: en, zh, or auto");
        return 0;
    }

    char cache_dir[CLI_BUF_1K];
    const char *resolved_cache = cbm_resolve_cache_dir();
    int cache_len =
        resolved_cache ? snprintf(cache_dir, sizeof(cache_dir), "%s", resolved_cache) : -1;
    if (cache_len <= 0 || cache_len >= (int)sizeof(cache_dir)) {
        (void)fprintf(stderr, "error: cache directory is unavailable or too long\n");
        return CLI_TRUE;
    }

    cbm_config_t *cfg = cbm_config_open(cache_dir);
    if (!cfg) {
        (void)fprintf(stderr, "error: cannot open config database\n");
        return CLI_TRUE;
    }

    int rc = 0;
    if (strcmp(argv[0], "list") == 0 || strcmp(argv[0], "ls") == 0) {
        printf("Configuration:\n");
        printf("  %-25s = %-10s\n", CBM_CONFIG_AUTO_INDEX,
               cbm_config_get(cfg, CBM_CONFIG_AUTO_INDEX, "false"));
        printf("  %-25s = %-10s\n", CBM_CONFIG_AUTO_INDEX_LIMIT,
               cbm_config_get(cfg, CBM_CONFIG_AUTO_INDEX_LIMIT, "50000"));
        printf("  %-25s = %-10s\n", CBM_CONFIG_AUTO_WATCH,
               cbm_config_get(cfg, CBM_CONFIG_AUTO_WATCH, "true"));
        printf("  %-25s = %-10s\n", CBM_CONFIG_UI_LANG,
               cbm_config_get(cfg, CBM_CONFIG_UI_LANG, "auto"));
    } else if (strcmp(argv[0], "get") == 0) {
        if (argc < MIN_ARGC_GET) {
            (void)fprintf(stderr, "Usage: config get <key>\n");
            rc = CLI_TRUE;
        } else {
            printf("%s\n", cbm_config_get(cfg, argv[CLI_SKIP_ONE], ""));
        }
    } else if (strcmp(argv[0], "set") == 0) {
        if (argc < MIN_ARGC_CMD) {
            (void)fprintf(stderr, "Usage: config set <key> <value>\n");
            rc = CLI_TRUE;
        } else {
            if (cbm_config_set(cfg, argv[CLI_SKIP_ONE], argv[CLI_PAIR_LEN]) == 0) {
                printf("%s = %s\n", argv[CLI_SKIP_ONE], argv[CLI_PAIR_LEN]);
            } else {
                (void)fprintf(stderr, "error: failed to set %s\n", argv[CLI_SKIP_ONE]);
                rc = CLI_TRUE;
            }
        }
    } else if (strcmp(argv[0], "reset") == 0) {
        if (argc < MIN_ARGC_GET) {
            (void)fprintf(stderr, "Usage: config reset <key>\n");
            rc = CLI_TRUE;
        } else {
            cbm_config_delete(cfg, argv[CLI_SKIP_ONE]);
            printf("%s reset to default\n", argv[CLI_SKIP_ONE]);
        }
    } else {
        (void)fprintf(stderr, "Unknown config command: %s\n", argv[0]);
        rc = CLI_TRUE;
    }

    cbm_config_close(cfg);
    return rc;
}

/* ── Interactive prompt ───────────────────────────────────────── */

/* Global auto-answer mode: 0=interactive, 1=always yes, -1=always no */
static int g_auto_answer = 0;

/* Test seam: force the auto-answer state so non-interactive bug-repro tests
 * can drive prompt_yn() deterministically (1 => yes, -1 => no, 0 => prompt).
 * Not declared in cli.h (internal); the repro runner links cli.c directly and
 * carries an extern forward declaration. Production never calls this. */
void cbm_set_auto_answer_for_test(int value);
void cbm_set_auto_answer_for_test(int value) {
    g_auto_answer = value;
}

static void parse_auto_answer(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) {
            g_auto_answer = AUTO_YES;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no") == 0) {
            g_auto_answer = AUTO_NO;
        }
    }
}

static bool prompt_yn(const char *question) {
    if (g_auto_answer == AUTO_YES) {
        printf("%s (y/n): y (auto)\n", question);
        return true;
    }
    if (g_auto_answer == AUTO_NO) {
        printf("%s (y/n): n (auto)\n", question);
        return false;
    }

    /* Non-interactive stdin: default to "no" to avoid hanging */
#ifndef _WIN32
    if (!isatty(fileno(stdin))) {
        (void)fprintf(stderr,
                      "error: interactive prompt requires a terminal. Use -y or -n flags.\n");
        return false;
    }
#endif

    printf("%s (y/n): ", question);
    (void)fflush(stdout);

    char buf[CLI_BUF_16];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return false;
    }
    return (buf[0] == 'y' || buf[0] == 'Y') ? true : false;
}

/* ── SHA-256 checksum verification ─────────────────────────────── */

/* SHA-256 hex digest: 64 hex chars + NUL */
#define SHA256_HEX_LEN CBM_SZ_64
#define SHA256_BUF_SIZE (SHA256_HEX_LEN + CLI_SKIP_ONE)
/* Minimum line length in checksums.txt: 64 hex + 2 spaces + 1 char filename */

/* Compute the SHA-256 of a file in-process (no external hashing tool — those
 * differ per OS, may be absent, and mis-quote paths under cmd.exe). Writes a
 * 64-char hex digest + NUL to out. Returns 0 on success. Not static:
 * exercised directly by the self-update checksum regression test. */
int cbm_cli_sha256_file(const char *path, char *out, size_t out_size) {
    if (out_size < SHA256_BUF_SIZE) {
        return CLI_ERR;
    }
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return CLI_ERR;
    }
    cbm_sha256_ctx ctx;
    cbm_sha256_init(&ctx);
    unsigned char buf[CLI_BUF_1K];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        cbm_sha256_update(&ctx, buf, n);
    }
    int read_err = ferror(fp);
    int close_rc = fclose(fp);
    if (read_err || close_rc != 0) {
        return CLI_ERR;
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[SHA256_HEX_LEN] = '\0';
    return 0;
}

/* ── Download helper (curl stdout → caller-owned private fd) ───── */

enum { CLI_CHECKSUM_MAX_BYTES = 1024 * 1024 };

#ifndef _WIN32
static bool cli_posix_executable_candidate(const char *candidate, char *out, size_t out_size) {
    if (!candidate || candidate[0] != '/' || !out || out_size == 0 ||
        !cbm_canonical_path(candidate, out, out_size)) {
        return false;
    }
    struct stat st;
    return stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0;
}

static bool cli_resolve_executable_posix(const char *name, const char *env_name, char *out,
                                         size_t out_size) {
    if (!name || !name[0] || !env_name || !out || out_size == 0) {
        return false;
    }
    const char *configured = getenv(env_name);
    if (configured && configured[0]) {
        /* An explicit trust anchor never falls through to PATH. */
        return cli_posix_executable_candidate(configured, out, out_size);
    }

    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin";
    }
    size_t name_len = strlen(name);
    const char *cursor = path;
    while (*cursor) {
        const char *end = strchr(cursor, ':');
        size_t dir_len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (dir_len > 0 && cursor[0] == '/' && dir_len <= SIZE_MAX - name_len - CLI_PAIR_LEN) {
            bool separator = cursor[dir_len - CLI_SKIP_ONE] != '/';
            size_t candidate_size = dir_len + (separator ? 1U : 0U) + name_len + CLI_SKIP_ONE;
            char *candidate = malloc(candidate_size);
            if (!candidate) {
                return false;
            }
            memcpy(candidate, cursor, dir_len);
            size_t pos = dir_len;
            if (separator) {
                candidate[pos++] = '/';
            }
            memcpy(candidate + pos, name, name_len + CLI_SKIP_ONE);
            bool found = cli_posix_executable_candidate(candidate, out, out_size);
            free(candidate);
            if (found) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        cursor = end + CLI_SKIP_ONE;
    }
    return false;
}

#ifdef __APPLE__
static bool cli_resolve_apple_tool(const char *name, const char *env_name, char *out,
                                   size_t out_size) {
    if (!name || !name[0] || strchr(name, '/') || !env_name || !out || out_size == 0) {
        return false;
    }
    const char *configured = getenv(env_name);
    if (configured && configured[0]) {
        return cli_posix_executable_candidate(configured, out, out_size);
    }
    char system_candidate[CLI_BUF_256];
    int written = snprintf(system_candidate, sizeof(system_candidate), "/usr/bin/%s", name);
    return written > 0 && (size_t)written < sizeof(system_candidate) &&
           cli_posix_executable_candidate(system_candidate, out, out_size);
}

int cbm_cli_resolve_apple_tool_for_test(const char *name, const char *env_name, char *out,
                                        size_t out_size) {
    return cli_resolve_apple_tool(name, env_name, out, out_size) ? CLI_OK : CLI_ERR;
}
#endif
#endif

#ifdef _WIN32
static bool cli_windows_absolute_path(const wchar_t *path) {
    if (!path || !path[0]) {
        return false;
    }
    bool drive_absolute =
        (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
         path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'));
    bool unc_absolute = path[0] == L'\\' && path[1] == L'\\';
    return drive_absolute || unc_absolute;
}

static bool cli_windows_executable_candidate(const wchar_t *candidate, char *out, size_t out_size) {
    if (!cli_windows_absolute_path(candidate) || !out || out_size == 0) {
        return false;
    }
    HANDLE file = CreateFileW(candidate, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool valid = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
                 !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE));
    DWORD needed =
        valid ? GetFinalPathNameByHandleW(file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
              : 0;
    wchar_t *final_path =
        needed > 0 && needed < 32768U ? malloc(((size_t)needed + 1U) * sizeof(*final_path)) : NULL;
    DWORD written = final_path ? GetFinalPathNameByHandleW(file, final_path, needed + 1U,
                                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                               : 0;
    CloseHandle(file);
    if (!final_path || written == 0 || written > needed) {
        free(final_path);
        return false;
    }
    char *utf8 = cbm_wide_to_utf8(final_path);
    free(final_path);
    if (!utf8) {
        return false;
    }
    size_t len = strlen(utf8);
    bool fits = len > 0 && len < out_size;
    if (fits) {
        memcpy(out, utf8, len + CLI_SKIP_ONE);
    }
    free(utf8);
    return fits;
}

static wchar_t *cli_windows_env_alloc(const wchar_t *name) {
    DWORD needed = GetEnvironmentVariableW(name, NULL, 0);
    if (needed == 0 || needed > 32768U) {
        return NULL;
    }
    wchar_t *value = malloc((size_t)needed * sizeof(*value));
    if (!value) {
        return NULL;
    }
    DWORD written = GetEnvironmentVariableW(name, value, needed);
    if (written == 0 || written >= needed) {
        free(value);
        return NULL;
    }
    return value;
}

static bool cli_resolve_curl_windows(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    DWORD configured_needed = GetEnvironmentVariableW(L"CBM_CURL_BIN", NULL, 0);
    if (configured_needed > 0) {
        wchar_t *configured = cli_windows_env_alloc(L"CBM_CURL_BIN");
        bool found = configured && cli_windows_executable_candidate(configured, out, out_size);
        free(configured);
        return found; /* an explicit trust anchor never falls through to another executable */
    }

    /* Prefer the OS-owned curl shipped in System32 over a PATH entry. */
    wchar_t system_dir[32768];
    UINT system_len =
        GetSystemDirectoryW(system_dir, (UINT)(sizeof(system_dir) / sizeof(*system_dir)));
    if (system_len > 0 && system_len < sizeof(system_dir) / sizeof(*system_dir) &&
        (size_t)system_len <= SIZE_MAX - 10U) {
        bool separator = system_dir[system_len - CLI_SKIP_ONE] != L'\\' &&
                         system_dir[system_len - CLI_SKIP_ONE] != L'/';
        size_t candidate_len =
            (size_t)system_len + (separator ? 1U : 0U) + sizeof(L"curl.exe") / sizeof(wchar_t);
        wchar_t *candidate = malloc(candidate_len * sizeof(*candidate));
        if (candidate) {
            memcpy(candidate, system_dir, (size_t)system_len * sizeof(*candidate));
            size_t pos = system_len;
            if (separator) {
                candidate[pos++] = L'\\';
            }
            memcpy(candidate + pos, L"curl.exe", sizeof(L"curl.exe"));
            bool found = cli_windows_executable_candidate(candidate, out, out_size);
            free(candidate);
            if (found) {
                return true;
            }
        }
    }

    bool found = false;
    wchar_t *path = cli_windows_env_alloc(L"PATH");
    if (!path) {
        return false;
    }
    wchar_t *cursor = path;
    while (*cursor && !found) {
        wchar_t *end = wcschr(cursor, L';');
        if (end) {
            *end = L'\0';
        }
        wchar_t *start = cursor;
        size_t len = wcslen(start);
        if (len >= CLI_PAIR_LEN && start[0] == L'"' && start[len - CLI_SKIP_ONE] == L'"') {
            start[len - CLI_SKIP_ONE] = L'\0';
            start++;
            len -= CLI_PAIR_LEN;
        }
        if (len > 0 && cli_windows_absolute_path(start) && len <= SIZE_MAX - 10U) {
            bool separator =
                start[len - CLI_SKIP_ONE] != L'\\' && start[len - CLI_SKIP_ONE] != L'/';
            wchar_t *candidate = malloc((len + (separator ? 1U : 0U) + 9U) * sizeof(*candidate));
            if (candidate) {
                memcpy(candidate, start, len * sizeof(*candidate));
                size_t pos = len;
                if (separator) {
                    candidate[pos++] = L'\\';
                }
                memcpy(candidate + pos, L"curl.exe", 9U * sizeof(*candidate));
                found = cli_windows_executable_candidate(candidate, out, out_size);
                free(candidate);
            }
        }
        if (!end) {
            break;
        }
        cursor = end + CLI_SKIP_ONE;
    }
    free(path);
    return found;
}
#endif

static bool cli_resolve_curl(char *out, size_t out_size) {
#ifdef _WIN32
    return cli_resolve_curl_windows(out, out_size);
#else
    return cli_resolve_executable_posix("curl", "CBM_CURL_BIN", out, out_size);
#endif
}

static int cli_prepare_output_fd(int fd) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG || _chsize_s(fd, 0) != 0 ||
        _lseeki64(fd, 0, SEEK_SET) < 0) {
        return CLI_ERR;
    }
#else
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || ftruncate(fd, 0) != 0 ||
        lseek(fd, 0, SEEK_SET) < 0) {
        return CLI_ERR;
    }
#endif
    return CLI_OK;
}

static int cli_download_to_fd(const char *url, int output_fd, size_t max_bytes) {
    if (!url || !url[0] || output_fd < 0 || max_bytes == 0 ||
        cli_prepare_output_fd(output_fd) != CLI_OK) {
        return CLI_ERR;
    }
    char curl_path[CLI_BUF_4K];
    if (!cli_resolve_curl(curl_path, sizeof(curl_path))) {
        return CLI_ERR;
    }
    const char *argv[] = {curl_path, "-fL", "--silent", "--show-error", url, NULL};
    cbm_proc_opts_t opts = {.bin = curl_path,
                            .argv = argv,
                            .discard_stderr = true,
                            .use_output_fd = true,
                            .output_fd = output_fd,
                            .max_output_bytes = max_bytes};
    cbm_proc_result_t result;
    if (cbm_subprocess_run(&opts, &result) != 0 || result.outcome != CBM_PROC_CLEAN ||
        result.exit_code != 0 || cli_sync_fd(output_fd) != 0) {
        return CLI_ERR;
    }
    return CLI_OK;
}

/* Internal regression seam: exercises the exact descriptor-based downloader
 * without running the interactive update command. Intentionally omitted from
 * cli.h; tests carry an extern declaration. */
int cbm_cli_download_to_fd_for_test(const char *url, int output_fd, size_t max_bytes) {
    return cli_download_to_fd(url, output_fd, max_bytes);
}

static int cli_secure_temp_fd(const char *label, char *path, size_t path_size) {
    if (!label || !path || path_size == 0) {
        return CLI_ERR;
    }
    const char *tmp_dir = cbm_tmpdir();
    if (!tmp_dir) {
        return CLI_ERR;
    }
    int n = snprintf(path, path_size, "%s/%s-XXXXXX", tmp_dir, label);
    if (n <= 0 || (size_t)n >= path_size) {
        return CLI_ERR;
    }
    int fd = cbm_mkstemp(path);
    if (fd < 0) {
        return CLI_ERR;
    }
#ifndef _WIN32
    /* Keep only the descriptor. No pathname can be swapped between checksum,
     * extraction, and publication. */
    if (unlink(path) != 0) {
        (void)close(fd);
        return CLI_ERR;
    }
#endif
    return fd;
}

/* ── macOS ad-hoc signing ─────────────────────────────────────── */

#ifdef __APPLE__
static int cbm_macos_adhoc_sign(const char *binary_path) {
    char xattr_path[CLI_BUF_4K];
    if (cli_resolve_apple_tool("xattr", "CBM_XATTR_BIN", xattr_path, sizeof(xattr_path))) {
        /* Remove quarantine xattr (best effort — may not exist). */
        const char *xattr_argv[] = {xattr_path, "-d", "com.apple.quarantine", binary_path, NULL};
        (void)cbm_exec_no_shell(xattr_argv);
    }

    /* Ad-hoc sign (required for arm64, harmless for x86_64) */
    char codesign_path[CLI_BUF_4K];
    if (!cli_resolve_apple_tool("codesign", "CBM_CODESIGN_BIN", codesign_path,
                                sizeof(codesign_path))) {
        return CLI_ERR;
    }
    const char *sign_argv[] = {codesign_path, "--sign", "-", "--force", binary_path, NULL};
    return cbm_exec_no_shell(sign_argv);
}
#endif

/* ── Kill other MCP server instances ──────────────────────────── */

static int cbm_kill_other_instances(const char *target_path) {
    if (!target_path || !target_path[0] || cbm_path_probe(target_path) != 1) {
        return 0;
    }
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int killed = 0;
    DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry = {.dwSize = sizeof(entry)};
    BOOL have_entry = Process32FirstW(snapshot, &entry);
    while (have_entry) {
        if (entry.th32ProcessID != 0 && entry.th32ProcessID != self) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                         FALSE, entry.th32ProcessID);
            wchar_t image_path[32768];
            DWORD image_len = (DWORD)(sizeof(image_path) / sizeof(*image_path));
            bool matches = false;
            if (process && QueryFullProcessImageNameW(process, 0, image_path, &image_len) &&
                image_len > 0 && image_len < sizeof(image_path) / sizeof(*image_path)) {
                image_path[image_len] = L'\0';
                char *utf8_path = cbm_wide_to_utf8(image_path);
                matches = utf8_path && cbm_same_file(target_path, utf8_path);
                free(utf8_path);
            }
            if (matches && TerminateProcess(process, 0)) {
                (void)WaitForSingleObject(process, 5000U);
                killed++;
            }
            if (process) {
                CloseHandle(process);
            }
        }
        have_entry = Process32NextW(snapshot, &entry);
    }
    CloseHandle(snapshot);
    return killed;
#else
    int killed = 0;
#if defined(__linux__) || defined(__APPLE__)
    pid_t self = getpid();
#endif
    char pgrep_path[CLI_BUF_4K];
    if (!cli_resolve_executable_posix("pgrep", "CBM_PGREP_BIN", pgrep_path, sizeof(pgrep_path))) {
        return 0;
    }
    const char *argv[] = {pgrep_path, "-x", "codebase-memory-mcp", NULL};
    cbm_proc_opts_t opts = {
        .bin = pgrep_path, .argv = argv, .discard_stderr = true, .quiet_timeout_ms = 10000};
    char *output = NULL;
    size_t output_len = 0;
    char output_sha256[CBM_SHA256_HEX_LEN + 1];
    cbm_proc_result_t result;
    if (cbm_subprocess_capture(&opts, 64U * 1024U, &output, &output_len, output_sha256, &result) !=
        0) {
        free(output);
        return 0;
    }
    char *cursor = output;
    char *end = output + output_len;
    while (cursor < end) {
        errno = 0;
        char *after = NULL;
        long parsed = strtol(cursor, &after, CLI_STRTOL_BASE);
        pid_t pid = errno == 0 && after != cursor && parsed > 0 ? (pid_t)parsed : 0;
#if defined(__linux__) || defined(__APPLE__)
        bool matches = false;
        if (pid > 0 && pid != self) {
#ifdef __linux__
            char process_path[CBM_SZ_64];
            int process_len =
                snprintf(process_path, sizeof(process_path), "/proc/%ld/exe", (long)pid);
            struct stat target_st;
            struct stat process_st;
            matches = process_len > 0 && (size_t)process_len < sizeof(process_path) &&
                      stat(target_path, &target_st) == 0 && stat(process_path, &process_st) == 0 &&
                      target_st.st_dev == process_st.st_dev &&
                      target_st.st_ino == process_st.st_ino;
#elif defined(__APPLE__)
            char process_path[PROC_PIDPATHINFO_MAXSIZE];
            int process_len = proc_pidpath(pid, process_path, sizeof(process_path));
            matches = process_len > 0 && cbm_same_file(target_path, process_path);
#endif
        }
        if (matches) {
            if (kill(pid, SIGTERM) == 0) {
                killed++;
            }
        }
#else
        (void)pid;
#endif
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!newline) {
            break;
        }
        cursor = newline + CLI_SKIP_ONE;
    }
    free(output);
    return killed;
#endif
}

static int cli_stop_update_target(const char *path, void *ctx) {
    (void)ctx;
    int killed = cbm_kill_other_instances(path);
    if (killed > 0) {
        printf("Stopped %d running MCP server instance(s).\n", killed);
    }
    return CLI_OK;
}

static int cli_verify_published_update(const char *path, void *ctx) {
    (void)ctx;
#ifdef __APPLE__
    if (cbm_macos_adhoc_sign(path) != 0) {
        (void)fprintf(stderr,
                      "warning: ad-hoc signing failed — binary may not run on macOS arm64\n");
    }
#endif
    printf("\nVerifying installed binary:\n");
    const char *argv[] = {path, "--version", NULL};
    return cbm_exec_no_shell(argv) == 0 ? CLI_OK : CLI_ERR;
}

/* Download checksums.txt and verify the archive integrity.
 * Returns: 0 = verified OK, 1 = mismatch (FAIL), -1 = could not verify (warning). */
bool cbm_parse_release_checksum(const char *line, const char *archive_name, char *out,
                                size_t out_size) {
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!line || !archive_name || !archive_name[0] || !out || out_size < SHA256_BUF_SIZE) {
        return false;
    }
    size_t line_len = strlen(line);
    if (line_len <= SHA256_HEX_LEN ||
        (line[SHA256_HEX_LEN] != ' ' && line[SHA256_HEX_LEN] != '\t')) {
        return false;
    }
    for (int i = 0; i < SHA256_HEX_LEN; i++) {
        unsigned char c = (unsigned char)line[i];
        if (!isxdigit(c)) {
            return false;
        }
        out[i] = (char)tolower(c);
    }
    out[SHA256_HEX_LEN] = '\0';

    const char *filename = line + SHA256_HEX_LEN;
    while (*filename == ' ' || *filename == '\t') {
        filename++;
    }
    if (*filename == '*') {
        filename++;
    }
    size_t filename_len = strlen(filename);
    while (filename_len > 0 && (filename[filename_len - SKIP_ONE] == '\r' ||
                                filename[filename_len - SKIP_ONE] == '\n')) {
        filename_len--;
    }
    if (filename_len != strlen(archive_name) ||
        strncmp(filename, archive_name, filename_len) != 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static FILE *cli_fdopen_read_duplicate(int fd) {
#ifdef _WIN32
    int copy = _dup(fd);
    FILE *file = copy >= 0 ? _fdopen(copy, "rb") : NULL;
#else
    int copy = dup(fd);
    FILE *file = copy >= 0 ? fdopen(copy, "rb") : NULL;
#endif
    if (!file && copy >= 0) {
        (void)cli_close_fd(copy);
    }
    if (file && fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    return file;
}

static bool cli_file_generation_same(const struct stat *before, const struct stat *after) {
#ifdef _WIN32
    return before->st_size == after->st_size && before->st_mtime == after->st_mtime;
#else
    if (before->st_dev != after->st_dev || before->st_ino != after->st_ino ||
        before->st_size != after->st_size || before->st_mtime != after->st_mtime ||
        before->st_ctime != after->st_ctime) {
        return false;
    }
#ifdef __APPLE__
    return before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
#else
    return before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
#endif
#endif
}

static int cli_sha256_fd(int fd, size_t max_bytes, char out[SHA256_BUF_SIZE]) {
    if (fd < 0 || max_bytes == 0 || !out) {
        return CLI_ERR;
    }
    struct stat before;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > (uint64_t)max_bytes ||
        (uint64_t)before.st_size > (uint64_t)SIZE_MAX) {
        return CLI_ERR;
    }
    size_t expected = (size_t)before.st_size;
    FILE *file = cli_fdopen_read_duplicate(fd);
    if (!file) {
        return CLI_ERR;
    }
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    unsigned char buffer[CLI_BUF_8K];
    size_t total = 0;
    while (total < expected) {
        size_t remaining = expected - total;
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t got = fread(buffer, CLI_ELEM_SIZE, wanted, file);
        if (got == 0) {
            break;
        }
        cbm_sha256_update(&hash, buffer, got);
        total += got;
    }
    int extra = total == expected ? fgetc(file) : 0;
    struct stat after;
    bool ok = total == expected && extra == EOF && !ferror(file) &&
              fstat(fileno(file), &after) == 0 && cli_file_generation_same(&before, &after);
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        return CLI_ERR;
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * CLI_PAIR_LEN] = hex[digest[i] >> 4];
        out[i * CLI_PAIR_LEN + CLI_SKIP_ONE] = hex[digest[i] & 0x0f];
    }
    out[SHA256_HEX_LEN] = '\0';
    return CLI_OK;
}

static int verify_download_checksum(int archive_fd, const char *archive_name,
                                    char verified_sha256[SHA256_BUF_SIZE]) {
    if (!archive_name || !archive_name[0] || !verified_sha256) {
        return CLI_ERR;
    }
    verified_sha256[0] = '\0';
    char checksum_path[CLI_BUF_1K];
    int checksum_fd = cli_secure_temp_fd("cbm-checksums", checksum_path, sizeof(checksum_path));
    if (checksum_fd < 0) {
        return CLI_ERR;
    }

    const char *dl_base = getenv("CBM_DOWNLOAD_URL");
    if (!dl_base || !dl_base[0]) {
        dl_base = CBM_GITHUB_LATEST_DOWNLOAD_URL;
    }
    char checksum_url[CLI_BUF_512];
    int url_len = snprintf(checksum_url, sizeof(checksum_url), "%s/checksums.txt", dl_base);
    if (url_len <= 0 || (size_t)url_len >= sizeof(checksum_url)) {
        (void)cli_close_fd(checksum_fd);
        (void)cbm_unlink(checksum_path);
        return CLI_ERR;
    }
    int rc = cli_download_to_fd(checksum_url, checksum_fd, CLI_CHECKSUM_MAX_BYTES);
    if (rc != 0) {
        (void)fprintf(stderr, "error: could not download checksums.txt\n");
        (void)cli_close_fd(checksum_fd);
        (void)cbm_unlink(checksum_path);
        return CLI_ERR;
    }

    FILE *fp = cli_fdopen_read_duplicate(checksum_fd);
    int checksum_close_rc = cli_close_fd(checksum_fd);
    if (!fp || checksum_close_rc != 0) {
        if (fp) {
            (void)fclose(fp);
        }
        (void)cbm_unlink(checksum_path);
        return CLI_ERR;
    }

    char expected[SHA256_BUF_SIZE] = {0};
    char line[CLI_BUF_512];
    bool malformed_line = false;
    while (fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) {
            malformed_line = true;
            break; /* never accept a valid-looking suffix of an overlong line */
        }
        if (cbm_parse_release_checksum(line, archive_name, expected, sizeof(expected))) {
            break;
        }
    }
    bool checksum_read_ok = !ferror(fp) && !malformed_line;
    if (fclose(fp) != 0) {
        checksum_read_ok = false;
    }
    (void)cbm_unlink(checksum_path); /* required after fclose on Windows */

    if (!checksum_read_ok || expected[0] == '\0') {
        (void)fprintf(stderr, "error: exact checksum entry for %s not found\n", archive_name);
        return CLI_ERR;
    }

    char actual[SHA256_BUF_SIZE] = {0};
    if (cli_sha256_fd(archive_fd, DECOMPRESS_MAX_BYTES, actual) != 0) {
        (void)fprintf(stderr, "error: could not compute downloaded archive checksum\n");
        return CLI_ERR;
    }

    if (strcmp(expected, actual) != 0) {
        (void)fprintf(stderr, "error: CHECKSUM MISMATCH — downloaded binary may be compromised!\n");
        (void)fprintf(stderr, "  expected: %s\n", expected);
        (void)fprintf(stderr, "  actual:   %s\n", actual);
        return CLI_TRUE;
    }

    memcpy(verified_sha256, actual, sizeof(actual));
    printf("Checksum verified: %s\n", actual);
    return 0;
}

/* ── Detect OS/arch for download URL ──────────────────────────── */

static const char *detect_os(void) {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

static const char *detect_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "amd64";
#endif
}

/* ── Agent config install/refresh (shared by install + update) ── */

/* Print detected agent names on a single line. */
static void print_detected_agents(const cbm_detected_agents_t *a) {
    struct {
        bool flag;
        const char *name;
    } agents[] = {
        {a->claude_code, "Claude-Code"},
        {a->codex, "Codex"},
        {a->gemini, "Gemini-CLI"},
        {a->zed, "Zed"},
        {a->opencode, "OpenCode"},
        {a->antigravity, "Antigravity"},
        {a->aider, "Aider"},
        {a->kilocode, "KiloCode"},
        {a->vscode, "VS-Code"},
        {a->cursor, "Cursor"},
        {a->openclaw, "OpenClaw"},
        {a->kiro, "Kiro"},
        {a->junie, "Junie"},
    };
    printf("Detected agents:");
    bool any = false;
    for (int i = 0; i < (int)(sizeof(agents) / sizeof(agents[0])); i++) {
        if (agents[i].flag) {
            printf(" %s", agents[i].name);
            any = true;
        }
    }
    if (!any) {
        printf(" (none)");
    }
    printf("\n\n");
}

/* Install Claude Code-specific configs (skills, MCP, hooks). */
/* ── Install plan recorder (issue #388) ────────────────────────────
 * When g_install_plan != NULL, the install path runs as a dry-run and each
 * write site records its planned target HERE — at the same point it would
 * perform the write — so the emitted plan cannot drift from actual install
 * behavior (it is the same code path with mutations disabled). */
typedef struct {
    char agent[CLI_BUF_32];
    char kind[CLI_BUF_32]; /* mcp_config | instructions | skills | hook | cleanup */
    char path[CLI_BUF_1K];
} cbm_plan_entry_t;

typedef struct {
    cbm_plan_entry_t *items;
    int count;
    int cap;
} cbm_install_plan_t;

static cbm_install_plan_t *g_install_plan = NULL;

static void plan_record(const char *agent, const char *kind, const char *path) {
    if (!g_install_plan || !path || !path[0]) {
        return;
    }
    cbm_install_plan_t *pl = g_install_plan;
    if (pl->count >= pl->cap) {
        int ncap = pl->cap ? pl->cap * 2 : CLI_BUF_16;
        cbm_plan_entry_t *ni = realloc(pl->items, (size_t)ncap * sizeof(*ni));
        if (!ni) {
            return;
        }
        pl->items = ni;
        pl->cap = ncap;
    }
    cbm_plan_entry_t *e = &pl->items[pl->count++];
    snprintf(e->agent, sizeof(e->agent), "%s", agent);
    snprintf(e->kind, sizeof(e->kind), "%s", kind);
    snprintf(e->path, sizeof(e->path), "%s", path);
}

#define LEGACY_CODEX_INSTRUCTIONS_RELATIVE_PATH ".codex/instructions/codebase-memory-mcp.md"
#define LEGACY_CODEX_INSTRUCTIONS_SHA256 \
    "7fb17c8373ff9b12886f0a701f4061986bd6fedbc39cebf93eed767b7e4de7bf"

/* Remove the exact product-generated Go-era Codex instructions file. Unknown
 * content is preserved because users may have edited the legacy file. This is
 * intentionally not part of the public CLI API; tests link it directly.
 * Returns 1 when removal is performed/planned, 0 when no safe cleanup applies,
 * and -1 on an operational error. */
int cbm_cleanup_legacy_codex_instructions(const char *home, bool dry_run);
int cbm_cleanup_legacy_codex_instructions(const char *home, bool dry_run) {
    if (!home || !home[0]) {
        return CLI_ERR;
    }

    char path[CLI_BUF_1K];
    int n = snprintf(path, sizeof(path), "%s/%s", home, LEGACY_CODEX_INSTRUCTIONS_RELATIVE_PATH);
    if (n < 0 || n >= (int)sizeof(path)) {
        return CLI_ERR;
    }
    if (!cbm_file_exists(path)) {
        return CLI_OK;
    }

    size_t content_len = 0;
    char *content = read_file_str(path, &content_len);
    if (!content) {
        if (!g_install_plan) {
            (void)fprintf(stderr, "warning: cannot inspect legacy Codex instructions: %s\n", path);
        }
        return CLI_ERR;
    }

    char digest[CBM_SHA256_HEX_LEN + CLI_SKIP_ONE];
    cbm_sha256_hex(content, content_len, digest);
    free(content);

    if (strcmp(digest, LEGACY_CODEX_INSTRUCTIONS_SHA256) != 0) {
        if (!g_install_plan) {
            (void)fprintf(stderr, "note: preserving modified legacy Codex instructions: %s\n",
                          path);
        }
        return CLI_OK;
    }

    if (g_install_plan) {
        plan_record("Codex CLI", "cleanup", path);
        return CLI_TRUE;
    }
    if (dry_run) {
        printf("Codex CLI: would remove legacy generated instructions: %s\n", path);
        return CLI_TRUE;
    }
    if (cbm_unlink(path) != 0) {
        (void)fprintf(stderr, "warning: failed to remove legacy Codex instructions: %s\n", path);
        return CLI_ERR;
    }

    printf("Codex CLI: removed legacy generated instructions: %s\n", path);
    return CLI_TRUE;
}

static void install_claude_code_config(const char *home, const char *binary_path, bool force,
                                       bool dry_run) {
    char config_dir[CLI_BUF_1K];
    cbm_claude_config_dir(home, config_dir, sizeof(config_dir));
    char user_root[CLI_BUF_1K];
    cbm_claude_user_root(home, user_root, sizeof(user_root));

    char skills_dir[CLI_BUF_1K];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", config_dir);

    /* Plan mode: record the planned writes and return without mutating (#388). */
    if (g_install_plan) {
        char p[CLI_BUF_1K];
        plan_record("Claude Code", "skills", skills_dir);
        snprintf(p, sizeof(p), "%s/.mcp.json", config_dir);
        plan_record("Claude Code", "mcp_config", p);
        snprintf(p, sizeof(p), "%s/.claude.json", user_root);
        plan_record("Claude Code", "mcp_config", p);
        snprintf(p, sizeof(p), "%s/settings.json", config_dir);
        plan_record("Claude Code", "mcp_config", p);
        snprintf(p, sizeof(p), "%s/hooks/%s", config_dir, CMM_HOOK_GATE_SCRIPT);
        plan_record("Claude Code", "hook", p);
        snprintf(p, sizeof(p), "%s/hooks/%s", config_dir, CMM_SESSION_REMINDER_SCRIPT);
        plan_record("Claude Code", "hook", p);
        snprintf(p, sizeof(p), "%s/hooks/%s", config_dir, CMM_SUBAGENT_REMINDER_SCRIPT);
        plan_record("Claude Code", "hook", p);
        return;
    }

    printf("Claude Code:\n");

    int skill_count = cbm_install_skills(skills_dir, force, dry_run);
    printf("  skills: %d installed\n", skill_count);

    if (cbm_remove_old_monolithic_skill(skills_dir, dry_run)) {
        printf("  removed old monolithic skill\n");
    }

    char mcp_path[CLI_BUF_1K];
    snprintf(mcp_path, sizeof(mcp_path), "%s/.mcp.json", config_dir);
    if (!dry_run) {
        cbm_install_editor_mcp(binary_path, mcp_path);
    }
    printf("  mcp: %s\n", mcp_path);

    char mcp_path2[CLI_BUF_1K];
    snprintf(mcp_path2, sizeof(mcp_path2), "%s/.claude.json", user_root);
    if (!dry_run) {
        cbm_install_editor_mcp(binary_path, mcp_path2);
    }
    printf("  mcp: %s\n", mcp_path2);

    char settings_path[CLI_BUF_1K];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", config_dir);
    if (!dry_run) {
        cbm_upsert_claude_hooks(settings_path);
        cbm_install_hook_gate_script(home, binary_path);
        cbm_install_session_reminder_script(home);
        cbm_upsert_session_hooks(settings_path);
        cbm_install_subagent_reminder_script(home);
        cbm_upsert_claude_subagent_hooks(settings_path);
    }
    printf("  hooks: PreToolUse (Grep/Glob search-graph augmenter, non-blocking)\n");
    printf("  hooks: SessionStart (MCP usage reminder on startup/resume/clear/compact)\n");
    printf("  hooks: SubagentStart (MCP usage reminder for subagents)\n");

    /* Migration nudge: when CLAUDE_CONFIG_DIR is set and a legacy ~/.claude tree
     * still exists, mention it so users can clean up stale artifacts. */
    if (home && home[0]) {
        char legacy_dir[CLI_BUF_1K];
        snprintf(legacy_dir, sizeof(legacy_dir), "%s/.claude", home);
        if (strcmp(legacy_dir, config_dir) != 0 && dir_exists(legacy_dir)) {
            (void)fprintf(stderr,
                          "  note: $CLAUDE_CONFIG_DIR=%s used; legacy %s still exists.\n"
                          "        Remove stale {skills,hooks,settings.json,.mcp.json} there if "
                          "no longer needed.\n",
                          config_dir, legacy_dir);
        }
    }
}

/* Install MCP config + optional instructions for a generic agent. */
static void install_generic_agent_config(const char *label, const char *binary_path,
                                         const char *config_path, const char *instr_path,
                                         bool dry_run,
                                         int (*install_mcp)(const char *, const char *)) {
    /* Plan mode: record planned writes, mutate nothing (#388). */
    if (g_install_plan) {
        plan_record(label, "mcp_config", config_path);
        if (instr_path) {
            plan_record(label, "instructions", instr_path);
        }
        return;
    }
    printf("%s:\n", label);
    if (!dry_run) {
        install_mcp(binary_path, config_path);
    }
    printf("  mcp: %s\n", config_path);
    if (instr_path) {
        if (!dry_run) {
            cbm_upsert_instructions(instr_path, cbm_get_agent_instructions());
        }
        printf("  instructions: %s\n", instr_path);
    }
}

/* Install MCP configs for CLI-based agents (Codex, Gemini, OpenCode, Antigravity, Aider). */
/* Install Gemini CLI config with hooks. */
static void install_gemini_config(const char *home, const char *binary_path, bool dry_run) {
    char cp[CLI_BUF_1K];
    char ip[CLI_BUF_1K];
    snprintf(cp, sizeof(cp), "%s/.gemini/settings.json", home);
    snprintf(ip, sizeof(ip), "%s/.gemini/GEMINI.md", home);
    install_generic_agent_config("Gemini CLI", binary_path, cp, ip, dry_run,
                                 cbm_install_editor_mcp);
    if (g_install_plan) {
        plan_record("Gemini CLI", "hook", cp); /* BeforeTool + SessionStart in settings.json */
        return;
    }
    if (!dry_run) {
        cbm_upsert_gemini_hooks(cp);
        cbm_upsert_gemini_session_hooks(cp);
    }
    printf("  hooks: BeforeTool + SessionStart (codebase-memory-mcp reminder)\n");
}

static void install_cli_agent_configs(const cbm_detected_agents_t *agents, const char *home,
                                      const char *binary_path, bool dry_run) {
    if (agents->codex) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.codex/config.toml", home);
        snprintf(ip, sizeof(ip), "%s/.codex/AGENTS.md", home);
        (void)cbm_cleanup_legacy_codex_instructions(home, dry_run);
        install_generic_agent_config("Codex CLI", binary_path, cp, ip, dry_run,
                                     cbm_upsert_codex_mcp);
        /* Choose the hook target: if ~/.codex/hooks.json already exists, the
         * user manages Codex hooks via the JSON representation — write the
         * SessionStart reminder there instead of config.toml. Writing both
         * makes Codex warn about loading hooks from two representations (#570).
         * config.toml remains the mcp_config target above either way. */
        char hooks_json[CLI_BUF_1K];
        snprintf(hooks_json, sizeof(hooks_json), "%s/.codex/hooks.json", home);
        bool use_hooks_json = cbm_file_exists(hooks_json);
        const char *hook_target = use_hooks_json ? hooks_json : cp;
        if (g_install_plan) {
            plan_record("Codex CLI", "hook", hook_target);
        } else {
            if (!dry_run) {
                if (use_hooks_json) {
                    cbm_upsert_gemini_session_hooks(hooks_json);
                } else {
                    cbm_upsert_codex_hooks(cp);
                }
            }
            printf("  hooks: SessionStart (codebase-memory-mcp reminder)\n");
        }
    }
    if (agents->gemini) {
        install_gemini_config(home, binary_path, dry_run);
    }
    if (agents->opencode) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.config/opencode/opencode.json", home);
        snprintf(ip, sizeof(ip), "%s/.config/opencode/AGENTS.md", home);
        install_generic_agent_config("OpenCode", binary_path, cp, ip, dry_run,
                                     cbm_upsert_opencode_mcp);
    }
    if (agents->antigravity) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        /* MCP config is the SHARED Antigravity config (CLI + IDE), not a
         * per-tool file (2026 unification). */
        snprintf(cp, sizeof(cp), "%s/.gemini/config/mcp_config.json", home);
        snprintf(ip, sizeof(ip), "%s/.gemini/antigravity-cli/AGENTS.md", home);
        if (!dry_run && !g_install_plan) {
            char cfg_dir[CLI_BUF_1K];
            snprintf(cfg_dir, sizeof(cfg_dir), "%s/.gemini/config", home);
            cbm_mkdir_p(cfg_dir, CLI_OCTAL_PERM);
        }
        install_generic_agent_config("Antigravity", binary_path, cp, ip, dry_run,
                                     cbm_upsert_antigravity_mcp);
        /* Antigravity CLI is Gemini-lineage and keeps a settings.json under
         * ~/.gemini/antigravity-cli/; install the SessionStart reminder there
         * using the shared Gemini hook JSON schema. */
        char sp[CLI_BUF_1K];
        snprintf(sp, sizeof(sp), "%s/.gemini/antigravity-cli/settings.json", home);
        if (g_install_plan) {
            plan_record("Antigravity", "hook", sp);
        } else {
            if (!dry_run) {
                cbm_upsert_gemini_session_hooks(sp);
            }
            printf("  hooks: SessionStart (codebase-memory-mcp reminder)\n");
        }
    }
    if (agents->aider) {
        char ip[CLI_BUF_1K];
        snprintf(ip, sizeof(ip), "%s/CONVENTIONS.md", home);
        if (g_install_plan) {
            plan_record("Aider", "instructions", ip);
        } else {
            printf("Aider:\n");
            if (!dry_run) {
                cbm_upsert_instructions(ip, cbm_get_agent_instructions());
            }
            printf("  instructions: %s\n", ip);
        }
    }
}

/* Scan Code/User/profiles/ and install (or plan) a per-profile mcp.json for
 * each existing profile subdirectory, so VS Code profile users inherit the MCP
 * server without manual steps (#431). No-op when profiles/ is absent. */
static void install_vscode_profile_configs(const char *code_user, const char *binary_path,
                                           bool dry_run) {
    char profiles_dir[CLI_BUF_1K];
    snprintf(profiles_dir, sizeof(profiles_dir), "%s/profiles", code_user);
    cbm_dir_t *d = cbm_opendir(profiles_dir);
    if (!d) {
        return;
    }
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        char profile_path[CLI_BUF_1K];
        snprintf(profile_path, sizeof(profile_path), "%s/%s", profiles_dir, ent->name);
        struct stat st;
        if (stat(profile_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/mcp.json", profile_path);
        install_generic_agent_config("VS Code", binary_path, cp, NULL, dry_run,
                                     cbm_install_vscode_mcp);
    }
    cbm_closedir(d);
}

static bool cli_join_platform_path(char *out, size_t out_size, const char *base, const char *suffix,
                                   const char *agent) {
    if (!out || out_size == 0 || !base || !base[0] || !suffix) {
        if (out && out_size > 0) {
            out[0] = '\0';
        }
        (void)fprintf(stderr, "warning: cannot resolve %s configuration directory\n", agent);
        return false;
    }
    int n = snprintf(out, out_size, "%s%s", base, suffix);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        (void)fprintf(stderr, "warning: %s configuration path is too long\n", agent);
        return false;
    }
    return true;
}

/* Install MCP configs for editor-based agents (Zed, KiloCode, VS Code, OpenClaw). */
static void install_editor_agent_configs(const cbm_detected_agents_t *agents, const char *home,
                                         const char *binary_path, bool dry_run) {
    if (agents->zed) {
        char cp[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(cp, sizeof(cp), home,
                                         "/Library/Application Support/Zed/settings.json", "Zed");
#elif defined(_WIN32)
        path_ok = cli_join_platform_path(cp, sizeof(cp), cbm_app_local_dir(), "/Zed/settings.json",
                                         "Zed");
#else
        path_ok = cli_join_platform_path(cp, sizeof(cp), cbm_app_config_dir(), "/zed/settings.json",
                                         "Zed");
#endif
        if (path_ok) {
            install_generic_agent_config("Zed", binary_path, cp, NULL, dry_run,
                                         cbm_install_zed_mcp);
        }
    }
    if (agents->kilocode) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(cp, sizeof(cp), home,
                                         "/Library/Application Support/Code/User/globalStorage/"
                                         "kilocode.kilo-code/settings/mcp_settings.json",
                                         "KiloCode");
#else
        path_ok = cli_join_platform_path(
            cp, sizeof(cp), cbm_app_config_dir(),
            "/Code/User/globalStorage/kilocode.kilo-code/settings/mcp_settings.json", "KiloCode");
#endif
        bool instructions_ok = cli_join_platform_path(
            ip, sizeof(ip), home, "/.kilocode/rules/codebase-memory-mcp.md", "KiloCode");
        if (path_ok && instructions_ok) {
            install_generic_agent_config("KiloCode", binary_path, cp, ip, dry_run,
                                         cbm_install_editor_mcp);
        }
    }
    if (agents->vscode) {
        char code_user[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(code_user, sizeof(code_user), home,
                                         "/Library/Application Support/Code/User", "VS Code");
#else
        path_ok = cli_join_platform_path(code_user, sizeof(code_user), cbm_app_config_dir(),
                                         "/Code/User", "VS Code");
#endif
        char cp[CLI_BUF_1K];
        bool config_ok =
            path_ok && cli_join_platform_path(cp, sizeof(cp), code_user, "/mcp.json", "VS Code");
        if (config_ok) {
            install_generic_agent_config("VS Code", binary_path, cp, NULL, dry_run,
                                         cbm_install_vscode_mcp);
        }
        /* VS Code profiles each keep their own settings under
         * Code/User/profiles/<id>/. The default mcp.json above does NOT apply
         * to a named profile, so write/plan a per-profile mcp.json for every
         * existing profile directory (#431). */
        if (path_ok) {
            install_vscode_profile_configs(code_user, binary_path, dry_run);
        }
    }
    if (agents->cursor) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.cursor/mcp.json", home);
        install_generic_agent_config("Cursor", binary_path, cp, NULL, dry_run,
                                     cbm_install_editor_mcp);
    }
    if (agents->openclaw) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.openclaw/openclaw.json", home);
        install_generic_agent_config("OpenClaw", binary_path, cp, NULL, dry_run,
                                     cbm_install_openclaw_mcp);
    }
    if (agents->kiro) {
        char cp[CLI_BUF_1K];
        char sd[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.kiro/settings/mcp.json", home);
        snprintf(sd, sizeof(sd), "%s/.kiro/settings", home);
        if (!dry_run) {
            cbm_mkdir_p(sd, CLI_OCTAL_PERM);
        }
        install_generic_agent_config("Kiro", binary_path, cp, NULL, dry_run,
                                     cbm_install_editor_mcp);
    }
    if (agents->junie) {
        char cp[CLI_BUF_1K];
        char sd[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.junie/mcp/mcp.json", home);
        snprintf(sd, sizeof(sd), "%s/.junie/mcp", home);
        if (!dry_run) {
            cbm_mkdir_p(sd, CLI_OCTAL_PERM);
        }
        install_generic_agent_config("Junie", binary_path, cp, NULL, dry_run, cbm_upsert_junie_mcp);
    }
}

static void cbm_install_agent_configs(const char *home, const char *binary_path, bool force,
                                      bool dry_run) {
    cbm_detected_agents_t agents = cbm_detect_agents(home);
    if (!g_install_plan) {
        print_detected_agents(&agents);
    }

    if (agents.claude_code) {
        install_claude_code_config(home, binary_path, force, dry_run);
    }
    install_cli_agent_configs(&agents, home, binary_path, dry_run);
    install_editor_agent_configs(&agents, home, binary_path, dry_run);
}

/* Count .db files in the cache directory. */
static int count_db_indexes(const char *home) {
    const char *cache_dir = get_cache_dir(home);
    if (!cache_dir) {
        return CLI_ERR;
    }
    cbm_dir_t *d = cbm_opendir(cache_dir);
    if (!d) {
        return 0;
    }
    int count = 0;
    cbm_dirent_t *ent;
    while ((ent = cbm_readdir(d)) != NULL) {
        if (cli_is_project_db_file(ent)) {
            count++;
        }
    }
    bool failed = cbm_dir_had_error(d);
    cbm_closedir(d);
    return failed ? CLI_ERR : count;
}

/* Handle pre-existing indexes during (re)install (#607).
 *
 * Returns 1 to proceed with the install, 0 to abort (user declined the
 * destructive reset prompt).
 *
 * Default (reset=false): PRESERVE the indexed graph. We do NOT delete any
 * .db. We print an honest message telling the user the indexes are kept and
 * that they should re-index after install to pick up this version's
 * extraction improvements. The old behaviour deleted every index here while
 * printing "must be rebuilt" and never rebuilt — silent, irrecoverable data
 * loss (#607). Deletion is NOT a schema requirement (the store uses CREATE
 * TABLE IF NOT EXISTS with no migrations); it only guarded against stale
 * content, which a re-index fixes without destroying anything.
 *
 * Opt-in (reset=true, via `install --reset-indexes`): keep the original
 * prompt-and-delete behaviour, with honest "Delete" wording.
 *
 * Not static: linked into the bug-repro test runner so repro_issue607.c can
 * assert the default path preserves the DB. It is intentionally NOT declared
 * in cli.h (internal helper); the test carries an extern forward declaration.
 */
int cbm_install_handle_existing_indexes(const char *home, bool reset, bool dry_run);
int cbm_install_handle_existing_indexes(const char *home, bool reset, bool dry_run) {
    int index_count = count_db_indexes(home);
    if (index_count < 0) {
        (void)fprintf(stderr, "error: could not enumerate existing indexes; install cancelled\n");
        return 0;
    }
    if (index_count == 0) {
        return 1; /* nothing to handle, proceed */
    }

    if (!reset) {
        /* Default: preserve. Be honest — keep the indexes, advise re-index. */
        printf("Found %d existing index(es). Keeping them. After install, "
               "re-index to pick up this version's improvements:\n",
               index_count);
        cbm_list_indexes(home);
        printf("\n");
        return 1; /* proceed without deleting */
    }

    /* Opt-in reset (--reset-indexes): the original prompt-and-delete path. */
    printf("Found %d existing index(es):\n", index_count);
    cbm_list_indexes(home);
    printf("\n");
    if (!prompt_yn("Delete these indexes and continue with install?")) {
        printf("Install cancelled.\n");
        return 0; /* abort */
    }
    if (!dry_run) {
        int removed = cbm_remove_indexes(home);
        if (removed < 0) {
            (void)fprintf(stderr, "error: index removal was incomplete; install cancelled\n");
            return 0;
        }
        printf("Removed %d index(es).\n\n", removed);
    }
    return 1; /* proceed */
}

/* ── Subcommand: install ──────────────────────────────────────── */

/* Detect the running binary's path at runtime. Falls back to ~/.local/bin/. */
static void cbm_detect_self_path(char *buf, size_t buf_sz, const char *home) {
    buf[0] = '\0';
#ifdef _WIN32
    GetModuleFileNameA(NULL, buf, (DWORD)buf_sz);
    cbm_normalize_path_sep(buf);
#elif defined(__APPLE__)
    uint32_t sp_sz = (uint32_t)buf_sz;
    if (_NSGetExecutablePath(buf, &sp_sz) != 0) {
        buf[0] = '\0';
    }
#else
    ssize_t sp_len = readlink("/proc/self/exe", buf, buf_sz - SKIP_ONE);
    if (sp_len > 0) {
        buf[sp_len] = '\0';
    }
#endif
    if (!buf[0]) {
#ifdef _WIN32
        snprintf(buf, buf_sz, "%s/.local/bin/codebase-memory-mcp.exe", home);
#else
        snprintf(buf, buf_sz, "%s/.local/bin/codebase-memory-mcp", home);
#endif
    }
}

static bool install_receipt_value(const char *receipt, const char *key, char *out, size_t out_sz) {
    if (!receipt || !key || !out || out_sz == 0) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *line = receipt;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t)(end - line) : strlen(line);
        if (line_len > 0 && line[line_len - SKIP_ONE] == '\r') {
            line_len--;
        }
        if (line_len > key_len + SKIP_ONE && strncmp(line, key, key_len) == 0 &&
            line[key_len] == '=') {
            size_t value_len = line_len - key_len - SKIP_ONE;
            if (value_len >= out_sz) {
                return false;
            }
            memcpy(out, line + key_len + SKIP_ONE, value_len);
            out[value_len] = '\0';
            return true;
        }
        if (!end) {
            break;
        }
        line = end + SKIP_ONE;
    }
    return false;
}

int cbm_resolve_update_target(const char *home, const char *self_path, char *out, size_t out_size) {
    if (!home || !home[0] || !self_path || !self_path[0] || !out || out_size == 0) {
        return CLI_ERR;
    }
    out[0] = '\0';

    char receipt_path[CLI_BUF_1K];
    snprintf(receipt_path, sizeof(receipt_path), "%s/.config/codebase-memory-mcp/install.conf",
             home);
    size_t receipt_len = 0;
    char *receipt = read_file_str(receipt_path, &receipt_len);
    if (receipt && receipt_len > 0) {
        const char *receipt_view = receipt;
        if (receipt_len >= 3 && (unsigned char)receipt[0] == 0xef &&
            (unsigned char)receipt[1] == 0xbb && (unsigned char)receipt[2] == 0xbf) {
            receipt_view += 3;
        }
        char format[CLI_BUF_8] = {0};
        char method[CLI_BUF_32] = {0};
        char repository[CLI_BUF_256] = {0};
        char install_path[CLI_BUF_1K] = {0};
        bool valid =
            install_receipt_value(receipt_view, "format", format, sizeof(format)) &&
            strcmp(format, "1") == 0 &&
            install_receipt_value(receipt_view, "method", method, sizeof(method)) &&
            strcmp(method, "official-script") == 0 &&
            install_receipt_value(receipt_view, "repository", repository, sizeof(repository)) &&
            strcmp(repository, CBM_GITHUB_REPOSITORY) == 0 &&
            install_receipt_value(receipt_view, "install_path", install_path, sizeof(install_path));
        free(receipt);
        if (!valid || strchr(install_path, '\n') || strchr(install_path, '\r') ||
            strlen(install_path) >= out_size || !cbm_same_file(self_path, install_path)) {
            return CLI_ERR;
        }
        snprintf(out, out_size, "%s", install_path);
        return out[0] ? CLI_OK : CLI_ERR;
    }
    free(receipt);

    /* Backward-compatible fallback for `codebase-memory-mcp install`, which
     * owns the canonical target even before install receipts existed. Do not
     * infer ownership for Homebrew/npm/PyPI or arbitrary PATH entries. */
    char canonical[CLI_BUF_1K];
#ifdef _WIN32
    snprintf(canonical, sizeof(canonical), "%s/.local/bin/codebase-memory-mcp.exe", home);
#else
    snprintf(canonical, sizeof(canonical), "%s/.local/bin/codebase-memory-mcp", home);
#endif
    if (!cbm_same_file(self_path, canonical)) {
        return CLI_ERR;
    }
    if (strlen(canonical) >= out_size) {
        return CLI_ERR;
    }
    snprintf(out, out_size, "%s", canonical);
    return CLI_OK;
}

static void parent_dir_copy(const char *path, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", path ? path : "");
    cbm_normalize_path_sep(out);
    char *slash = strrchr(out, '/');
    if (slash) {
        if (slash == out) {
            slash[SKIP_ONE] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        out[0] = '\0';
    }
}

/* Build the agent.install.plan.v1 receipt (#388): a machine-readable list of
 * the config / instruction / hook files `install` WOULD write, produced by
 * running the real install dispatch in record-only mode (no mutation, no
 * network). Returns a heap JSON string (caller frees) or NULL. */
char *cbm_build_install_plan_json(const char *home, const char *binary_path) {
    if (!home || !binary_path) {
        return NULL;
    }

    /* Same code path as a real install, but mutations disabled and every write
     * site records into `plan` — so the receipt cannot drift from behavior. */
    cbm_install_plan_t plan = {0};
    g_install_plan = &plan;
    cbm_install_agent_configs(home, binary_path, false, true);
    g_install_plan = NULL;

    cbm_detected_agents_t det = cbm_detect_agents(home);
    struct {
        bool flag;
        const char *name;
    } names[] = {
        {det.claude_code, "claude-code"},
        {det.codex, "codex"},
        {det.gemini, "gemini"},
        {det.zed, "zed"},
        {det.opencode, "opencode"},
        {det.antigravity, "antigravity"},
        {det.aider, "aider"},
        {det.kilocode, "kilocode"},
        {det.vscode, "vscode"},
        {det.cursor, "cursor"},
        {det.openclaw, "openclaw"},
        {det.kiro, "kiro"},
        {det.junie, "junie"},
    };

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "agent.install.plan.v1");

    yyjson_mut_val *agents = yyjson_mut_arr(doc);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (names[i].flag) {
            yyjson_mut_arr_add_str(doc, agents, names[i].name);
        }
    }
    yyjson_mut_obj_add_val(doc, root, "agents_detected", agents);

    yyjson_mut_val *configs = yyjson_mut_arr(doc);
    yyjson_mut_val *instrs = yyjson_mut_arr(doc);
    yyjson_mut_val *hooks = yyjson_mut_arr(doc);
    yyjson_mut_val *cleanups = yyjson_mut_arr(doc);
    for (int i = 0; i < plan.count; i++) {
        cbm_plan_entry_t *e = &plan.items[i];
        if (strcmp(e->kind, "mcp_config") == 0) {
            yyjson_mut_arr_add_strcpy(doc, configs, e->path);
        } else if (strcmp(e->kind, "cleanup") == 0) {
            yyjson_mut_arr_add_strcpy(doc, cleanups, e->path);
        } else if (strcmp(e->kind, "hook") == 0) {
            yyjson_mut_val *h = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, h, "agent", e->agent);
            yyjson_mut_obj_add_strcpy(doc, h, "path", e->path);
            yyjson_mut_arr_add_val(hooks, h);
        } else {
            yyjson_mut_arr_add_strcpy(doc, instrs, e->path);
        }
    }
    yyjson_mut_obj_add_val(doc, root, "config_files_planned", configs);
    yyjson_mut_obj_add_val(doc, root, "instruction_files_planned", instrs);
    yyjson_mut_obj_add_val(doc, root, "hooks_planned", hooks);
    yyjson_mut_obj_add_val(doc, root, "cleanup_files_planned", cleanups);
    yyjson_mut_obj_add_bool(doc, root, "writes_started", false);
    yyjson_mut_obj_add_bool(doc, root, "network_after_install", false);
    yyjson_mut_obj_add_str(doc, root, "next_safe_command", "codebase-memory-mcp install -y");

    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(doc);
    free(plan.items);
    return json; /* malloc'd; caller frees */
}

int cbm_cmd_install(int argc, char **argv) {
    parse_auto_answer(argc, argv);
    bool dry_run = false;
    bool force = false;
    bool plan = false;
    bool reset_indexes = false;
    bool managed_path = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        }
        if (strcmp(argv[i], "--force") == 0) {
            force = true;
        }
        if (strcmp(argv[i], "--plan") == 0) {
            plan = true;
        }
        if (strcmp(argv[i], "--managed-path") == 0) {
            managed_path = true;
        }
        /* Opt-in: delete existing indexes during install. Default preserves
         * the indexed graph (#607). Only this flag triggers deletion. */
        if (strcmp(argv[i], "--reset-indexes") == 0) {
            reset_indexes = true;
        }
    }

    const char *home = cbm_get_home_dir();
    if (!home) {
        (void)fprintf(stderr, "error: HOME not set (use USERPROFILE on Windows)\n");
        return CLI_TRUE;
    }

    /* --plan: emit the machine-readable install receipt and exit WITHOUT
     * mutating anything (no config writes, no index deletion, no network) so
     * an agent can inspect exactly what install would touch first (#388). */
    if (plan) {
        char self_path[CLI_BUF_1K] = {0};
        cbm_detect_self_path(self_path, sizeof(self_path), home);
        char *json = cbm_build_install_plan_json(home, self_path);
        if (!json) {
            (void)fprintf(stderr, "error: failed to build install plan\n");
            return CLI_TRUE;
        }
        printf("%s\n", json);
        free(json);
        return 0;
    }

    printf("codebase-memory-mcp install %s\n\n", CBM_VERSION);

    /* (#607) Default: preserve existing indexes. `--reset-indexes` opts into
     * the old prompt-and-delete behaviour. The helper returns 0 only when the
     * user declines the reset prompt, in which case we abort the install. */
    if (cbm_install_handle_existing_indexes(home, reset_indexes, dry_run) == 0) {
        return CLI_TRUE;
    }

    /* Step 1b: Place the running binary at the canonical install target.
     * Previously install only re-signed whatever was already at the target, so
     * `install --force` from a freshly built binary silently kept the OLD file
     * — operators ran stale code believing they had upgraded (#472). Copy the
     * running binary to ~/.local/bin (unless we ARE that file), then sign it. */
    char self_path[CLI_BUF_1K] = {0};
    cbm_detect_self_path(self_path, sizeof(self_path), home);

    char bin_target[CLI_BUF_1K];
    if (managed_path) {
        snprintf(bin_target, sizeof(bin_target), "%s", self_path);
    } else {
#ifdef _WIN32
        snprintf(bin_target, sizeof(bin_target), "%s/.local/bin/codebase-memory-mcp.exe", home);
#else
        snprintf(bin_target, sizeof(bin_target), "%s/.local/bin/codebase-memory-mcp", home);
#endif
    }

    if (!cbm_same_file(self_path, bin_target)) {
        struct stat tgt_st;
        bool target_exists = (stat(bin_target, &tgt_st) == 0);
        bool do_copy = !target_exists || force;
        if (target_exists && !force) {
            printf("A different binary already exists at:\n  %s\n", bin_target);
            if (prompt_yn("Replace it with the binary you ran install from?")) {
                do_copy = true;
                force = true; /* user approved replacement for this run */
            } else {
                printf("Keeping existing binary; configs will point at it.\n\n");
            }
        }
        if (do_copy) {
            char bin_dir[CLI_BUF_1K];
            snprintf(bin_dir, sizeof(bin_dir), "%s/.local/bin", home);
            if (dry_run) {
                printf("Would install binary -> %s\n\n", bin_target);
            } else {
                /* Stop only processes executing the exact target, and only
                 * after the user has approved an actual replacement. */
                int killed = cbm_kill_other_instances(bin_target);
                if (killed > 0) {
                    printf("Stopped %d running MCP server instance(s).\n\n", killed);
                }
                cbm_mkdir_p(bin_dir, CLI_OCTAL_PERM);
                if (cbm_copy_binary_to_target(self_path, bin_target) != 0) {
                    (void)fprintf(stderr, "error: failed to copy binary to %s\n", bin_target);
                    return CLI_TRUE;
                }
                printf("Installed binary -> %s\n\n", bin_target);
            }
        }
    }

    /* Step 1d: macOS ad-hoc signing of the installed binary. A freshly
     * clang-built arm64 binary is linker-signed (flags=0x20002) and gets
     * Killed:9 when spawned by an MCP host; re-signing ad-hoc (flags=0x2)
     * makes it launchable. Sign the target, not whatever the operator ran. */
#ifdef __APPLE__
    if (!dry_run) {
        struct stat sign_st;
        if (stat(bin_target, &sign_st) == 0) {
            if (cbm_macos_adhoc_sign(bin_target) != 0) {
                (void)fprintf(
                    stderr, "warning: ad-hoc signing failed — binary may not run on macOS arm64\n");
            }
        }
    }
#endif

    /* Step 3: Install/refresh all agent configs, pointing at the install target. */
    cbm_install_agent_configs(home, bin_target, force, dry_run);

    /* Step 4: Ensure PATH */
    char bin_dir[CLI_BUF_1K];
    parent_dir_copy(bin_target, bin_dir, sizeof(bin_dir));
    const char *rc = cbm_detect_shell_rc(home);
    if (rc[0]) {
        int path_rc = cbm_ensure_path(bin_dir, rc, dry_run);
        if (path_rc == 0) {
            printf("\nAdded %s to PATH in %s\n", bin_dir, rc);
        } else if (path_rc == CLI_TRUE) {
            printf("\nPATH already includes %s\n", bin_dir);
        }
    }

    printf("\nInstall complete. Restart your shell or run:\n");
    printf("  source %s\n", rc);
    if (dry_run) {
        printf("\n(dry-run — no files were modified)\n");
    }
    return 0;
}

/* ── Subcommand: uninstall ────────────────────────────────────── */

/* Remove Claude Code agent configs. */
static void uninstall_claude_code(const char *home, bool dry_run) {
    char config_dir[CLI_BUF_1K];
    cbm_claude_config_dir(home, config_dir, sizeof(config_dir));
    char user_root[CLI_BUF_1K];
    cbm_claude_user_root(home, user_root, sizeof(user_root));

    char skills_dir[CLI_BUF_1K];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", config_dir);
    int removed = cbm_remove_skills(skills_dir, dry_run);
    printf("Claude Code: removed %d skill(s)\n", removed);

    char mcp_path[CLI_BUF_1K];
    snprintf(mcp_path, sizeof(mcp_path), "%s/.mcp.json", config_dir);
    if (!dry_run) {
        cbm_remove_editor_mcp(mcp_path);
    }
    printf("  removed MCP config entry\n");

    char mcp_path2[CLI_BUF_1K];
    snprintf(mcp_path2, sizeof(mcp_path2), "%s/.claude.json", user_root);
    if (!dry_run) {
        cbm_remove_editor_mcp(mcp_path2);
    }

    char settings_path[CLI_BUF_1K];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", config_dir);
    if (!dry_run) {
        cbm_remove_claude_hooks(settings_path);
        cbm_remove_session_hooks(settings_path);
        cbm_remove_claude_subagent_hooks(settings_path);
    }
    printf("  removed PreToolUse + SessionStart + SubagentStart hooks\n");
}

/* Remove MCP + instructions for a generic agent. */

typedef struct {
    const char *name;
    const char *config_path;
    const char *instr_path;
} mcp_uninstall_args_t;
static void uninstall_agent_mcp_instr(mcp_uninstall_args_t paths, bool dry_run,
                                      int (*remove_fn)(const char *)) {
    const char *name = paths.name;
    const char *instr_path = paths.instr_path;
    if (!dry_run) {
        remove_fn(paths.config_path);
    }
    printf("%s: removed MCP config entry\n", name);
    if (instr_path) {
        if (!dry_run) {
            cbm_remove_instructions(instr_path);
        }
        printf("  removed instructions\n");
    }
}

/* Remove CLI agent configs (Codex, Gemini, OpenCode, Antigravity, Aider). */
/* Uninstall Gemini CLI config + hooks. */
static void uninstall_gemini_config(const char *home, bool dry_run) {
    char cp[CLI_BUF_1K];
    char ip[CLI_BUF_1K];
    snprintf(cp, sizeof(cp), "%s/.gemini/settings.json", home);
    snprintf(ip, sizeof(ip), "%s/.gemini/GEMINI.md", home);
    if (!dry_run) {
        cbm_remove_editor_mcp(cp);
        cbm_remove_gemini_hooks(cp);
        cbm_remove_gemini_session_hooks(cp);
        cbm_remove_instructions(ip);
    }
    printf("Gemini CLI: removed MCP config + hooks + instructions\n");
}

static void uninstall_cli_agents(const cbm_detected_agents_t *agents, const char *home,
                                 bool dry_run) {
    if (agents->codex) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.codex/config.toml", home);
        snprintf(ip, sizeof(ip), "%s/.codex/AGENTS.md", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Codex CLI", cp, ip}, dry_run,
                                  cbm_remove_codex_mcp);
        if (!dry_run) {
            cbm_remove_codex_hooks(cp);
        }
        (void)cbm_cleanup_legacy_codex_instructions(home, dry_run);
    }
    if (agents->gemini) {
        uninstall_gemini_config(home, dry_run);
    }
    if (agents->opencode) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.config/opencode/opencode.json", home);
        snprintf(ip, sizeof(ip), "%s/.config/opencode/AGENTS.md", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"OpenCode", cp, ip}, dry_run,
                                  cbm_remove_opencode_mcp);
    }
    if (agents->antigravity) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.gemini/config/mcp_config.json", home);
        snprintf(ip, sizeof(ip), "%s/.gemini/antigravity-cli/AGENTS.md", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Antigravity", cp, ip}, dry_run,
                                  cbm_remove_antigravity_mcp);
        if (!dry_run) {
            char sp[CLI_BUF_1K];
            snprintf(sp, sizeof(sp), "%s/.gemini/antigravity-cli/settings.json", home);
            cbm_remove_gemini_session_hooks(sp);
        }
    }
    if (agents->aider) {
        char ip[CLI_BUF_1K];
        snprintf(ip, sizeof(ip), "%s/CONVENTIONS.md", home);
        if (!dry_run) {
            cbm_remove_instructions(ip);
        }
        printf("Aider: removed instructions\n");
    }
}

/* Remove editor agent configs (Zed, KiloCode, VS Code, OpenClaw). */
static void uninstall_editor_agents(const cbm_detected_agents_t *agents, const char *home,
                                    bool dry_run) {
    if (agents->zed) {
        char cp[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(cp, sizeof(cp), home,
                                         "/Library/Application Support/Zed/settings.json", "Zed");
#elif defined(_WIN32)
        path_ok = cli_join_platform_path(cp, sizeof(cp), cbm_app_local_dir(), "/Zed/settings.json",
                                         "Zed");
#else
        path_ok = cli_join_platform_path(cp, sizeof(cp), cbm_app_config_dir(), "/zed/settings.json",
                                         "Zed");
#endif
        if (path_ok) {
            uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Zed", cp, NULL}, dry_run,
                                      cbm_remove_zed_mcp);
        }
    }
    if (agents->kilocode) {
        char cp[CLI_BUF_1K];
        char ip[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(cp, sizeof(cp), home,
                                         "/Library/Application Support/Code/User/globalStorage/"
                                         "kilocode.kilo-code/settings/mcp_settings.json",
                                         "KiloCode");
#else
        path_ok = cli_join_platform_path(
            cp, sizeof(cp), cbm_app_config_dir(),
            "/Code/User/globalStorage/kilocode.kilo-code/settings/mcp_settings.json", "KiloCode");
#endif
        bool instructions_ok = cli_join_platform_path(
            ip, sizeof(ip), home, "/.kilocode/rules/codebase-memory-mcp.md", "KiloCode");
        if (path_ok && instructions_ok) {
            uninstall_agent_mcp_instr((mcp_uninstall_args_t){"KiloCode", cp, ip}, dry_run,
                                      cbm_remove_editor_mcp);
        }
    }
    if (agents->vscode) {
        char cp[CLI_BUF_1K];
        bool path_ok;
#ifdef __APPLE__
        path_ok = cli_join_platform_path(
            cp, sizeof(cp), home, "/Library/Application Support/Code/User/mcp.json", "VS Code");
#else
        path_ok = cli_join_platform_path(cp, sizeof(cp), cbm_app_config_dir(),
                                         "/Code/User/mcp.json", "VS Code");
#endif
        if (path_ok) {
            uninstall_agent_mcp_instr((mcp_uninstall_args_t){"VS Code", cp, NULL}, dry_run,
                                      cbm_remove_vscode_mcp);
        }
    }
    if (agents->cursor) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.cursor/mcp.json", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Cursor", cp, NULL}, dry_run,
                                  cbm_remove_editor_mcp);
    }
    if (agents->openclaw) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.openclaw/openclaw.json", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"OpenClaw", cp, NULL}, dry_run,
                                  cbm_remove_openclaw_mcp);
    }
    if (agents->kiro) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.kiro/settings/mcp.json", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Kiro", cp, NULL}, dry_run,
                                  cbm_remove_editor_mcp);
    }
    if (agents->junie) {
        char cp[CLI_BUF_1K];
        snprintf(cp, sizeof(cp), "%s/.junie/mcp/mcp.json", home);
        uninstall_agent_mcp_instr((mcp_uninstall_args_t){"Junie", cp, NULL}, dry_run,
                                  cbm_remove_junie_mcp);
    }
}

int cbm_cmd_uninstall(int argc, char **argv) {
    parse_auto_answer(argc, argv);
    bool dry_run = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        }
    }

    const char *home = cbm_get_home_dir();
    if (!home) {
        (void)fprintf(stderr, "error: HOME not set (use USERPROFILE on Windows)\n");
        return CLI_TRUE;
    }

    printf("codebase-memory-mcp uninstall\n\n");

    cbm_detected_agents_t agents = cbm_detect_agents(home);
    if (agents.claude_code) {
        uninstall_claude_code(home, dry_run);
    }
    uninstall_cli_agents(&agents, home, dry_run);
    uninstall_editor_agents(&agents, home, dry_run);

    /* Step 2: Remove indexes */
    int index_count = count_db_indexes(home);
    if (index_count < 0) {
        (void)fprintf(stderr, "warning: could not enumerate indexes; indexes kept\n");
    } else if (index_count > 0) {
        printf("\nFound %d index(es):\n", index_count);
        cbm_list_indexes(home);
        if (prompt_yn("Delete these indexes?")) {
            int idx_removed = cbm_remove_indexes(home);
            if (idx_removed < 0) {
                (void)fprintf(stderr, "error: index removal was incomplete\n");
            } else {
                printf("Removed %d index(es).\n", idx_removed);
            }
        } else {
            printf("Indexes kept.\n");
        }
    }

    /* Step 3: Remove binary */
    char bin_path[CLI_BUF_1K];
#ifdef _WIN32
    snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/codebase-memory-mcp.exe", home);
#else
    snprintf(bin_path, sizeof(bin_path), "%s/.local/bin/codebase-memory-mcp", home);
#endif
    struct stat st;
    if (stat(bin_path, &st) == 0) {
        if (!dry_run) {
            cbm_unlink(bin_path);
        }
        printf("Removed %s\n", bin_path);
    }

    printf("\nUninstall complete.\n");
    if (dry_run) {
        printf("(dry-run — no files were modified)\n");
    }
    return 0;
}

/* ── Subcommand: update ───────────────────────────────────────── */

/* Read a bounded archive from the already-open private download descriptor and
 * return its verified extracted binary. Publication is deliberately separate:
 * no running server is stopped until download, checksum, and extraction all
 * succeed. */

typedef struct {
    int archive_fd;
    const char *ext;
    const char *verified_sha256;
} extract_install_args_t;
static int extract_verified_binary(extract_install_args_t args, unsigned char **out_data,
                                   int *out_length) {
    const char *ext = args.ext;
    if (out_data) {
        *out_data = NULL;
    }
    if (out_length) {
        *out_length = 0;
    }
    if (args.archive_fd < 0 || !ext || !args.verified_sha256 || !out_data || !out_length ||
        strlen(args.verified_sha256) != CBM_SHA256_HEX_LEN) {
        return CLI_TRUE;
    }
    struct stat archive_st;
    if (fstat(args.archive_fd, &archive_st) != 0 || !S_ISREG(archive_st.st_mode) ||
        archive_st.st_size <= 0 || (uint64_t)archive_st.st_size > DECOMPRESS_MAX_BYTES ||
        (uint64_t)archive_st.st_size > INT_MAX) {
        (void)fprintf(stderr, "error: downloaded archive size is invalid or exceeds the limit\n");
        return CLI_TRUE;
    }
    size_t archive_size = (size_t)archive_st.st_size;
    FILE *f = cli_fdopen_read_duplicate(args.archive_fd);
    if (!f) {
        return CLI_TRUE;
    }

    unsigned char *data = malloc(archive_size);
    if (!data) {
        (void)fclose(f);
        return CLI_TRUE;
    }
    size_t total = 0;
    while (total < archive_size) {
        size_t got = fread(data + total, CLI_ELEM_SIZE, archive_size - total, f);
        if (got == 0) {
            break;
        }
        total += got;
    }
    int extra = total == archive_size ? fgetc(f) : 0;
    struct stat archive_after;
    bool read_ok = total == archive_size && extra == EOF && !ferror(f) &&
                   fstat(fileno(f), &archive_after) == 0 &&
                   cli_file_generation_same(&archive_st, &archive_after);
    if (fclose(f) != 0) {
        read_ok = false;
    }
    if (!read_ok) {
        free(data);
        return CLI_TRUE;
    }
    char extracted_archive_sha256[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex(data, archive_size, extracted_archive_sha256);
    if (strcmp(extracted_archive_sha256, args.verified_sha256) != 0) {
        (void)fprintf(stderr, "error: downloaded archive changed after checksum verification\n");
        free(data);
        return CLI_TRUE;
    }

    int bin_len = 0;
    unsigned char *bin_data = NULL;
    if (strcmp(ext, "tar.gz") == 0) {
        bin_data = cbm_extract_binary_from_targz(data, (int)archive_size, &bin_len);
    } else if (strcmp(ext, "zip") == 0) {
        bin_data = cbm_extract_binary_from_zip(data, (int)archive_size, &bin_len);
    }
    free(data);

    if (!bin_data || bin_len <= 0) {
        (void)fprintf(stderr, "error: binary not found in archive\n");
        free(bin_data);
        return CLI_TRUE;
    }

    *out_data = bin_data;
    *out_length = bin_len;
    return 0;
}

/* Build the download URL for the update command without accepting a silently
 * truncated environment override. */
static bool build_update_url(char *url, size_t url_size, const char *os, const char *arch,
                             const char *ext, bool want_ui) {
    if (!url || url_size == 0 || !os || !arch || !ext) {
        return false;
    }
    const char *base_url = getenv("CBM_DOWNLOAD_URL");
    if (!base_url || !base_url[0]) {
        base_url = CBM_GITHUB_LATEST_DOWNLOAD_URL;
    }
    /* Linux ships a fully-static "-portable" build; the standard linux binary
     * dynamically links glibc 2.38+ and fails on older distros. macOS/Windows
     * have no such variant. Keep in sync with install.sh / install.js / pypi
     * _cli.py. */
    const char *portable = (strcmp(os, "linux") == 0) ? "-portable" : "";
    int written = snprintf(url, url_size, "%s/codebase-memory-mcp-%s%s-%s%s.%s", base_url,
                           want_ui ? "ui-" : "", os, arch, portable, ext);
    return written > 0 && (size_t)written < url_size;
}

/* Confirm an index reset but defer the destructive step until after the new
 * archive has downloaded, verified, and installed. A failed network request
 * must never erase the only usable graph generation. */
static int update_plan_index_reset(const char *home, bool dry_run, bool *out_reset) {
    if (!out_reset) {
        return CLI_TRUE;
    }
    *out_reset = false;
    int index_count = count_db_indexes(home);
    if (index_count < 0) {
        (void)fprintf(stderr, "error: could not enumerate existing indexes\n");
        return CLI_TRUE;
    }
    if (index_count == 0) {
        return 0;
    }
    printf("Found %d existing index(es) that must be rebuilt after update:\n", index_count);
    cbm_list_indexes(home);
    printf("\n");
    if (dry_run) {
        printf("(dry-run — indexes would be deleted only after successful installation)\n\n");
        return 0;
    }
    if (!prompt_yn("Delete these indexes after the new binary is installed?")) {
        printf("Update cancelled.\n");
        return CLI_TRUE;
    }
    *out_reset = true;
    return 0;
}

/* Download, verify, extract, then publish transactionally. The exact managed
 * target's running processes are stopped only after every non-mutating stage
 * succeeds. A post-publication execution failure restores the old binary. */
static int download_verify_install(const char *url, const char *ext, const char *os,
                                   const char *arch, bool want_ui, const char *bin_dest) {
    char tmp_archive[CLI_BUF_1K];
    int archive_fd = cli_secure_temp_fd("cbm-update", tmp_archive, sizeof(tmp_archive));
    if (archive_fd < 0) {
        (void)fprintf(stderr, "error: cannot create private update staging file\n");
        return CLI_TRUE;
    }

    int rc = cli_download_to_fd(url, archive_fd, DECOMPRESS_MAX_BYTES);
    if (rc != 0) {
        (void)fprintf(stderr, "error: download failed or exceeded the archive limit\n");
        (void)cli_close_fd(archive_fd);
        (void)cbm_unlink(tmp_archive);
        return CLI_TRUE;
    }

    char archive_name[CLI_BUF_256];
    /* Must match build_update_url: linux uses the static "-portable" asset. */
    const char *portable = (strcmp(os, "linux") == 0) ? "-portable" : "";
    int archive_name_len =
        snprintf(archive_name, sizeof(archive_name), "codebase-memory-mcp-%s%s-%s%s.%s",
                 want_ui ? "ui-" : "", os, arch, portable, ext);
    if (archive_name_len <= 0 || (size_t)archive_name_len >= sizeof(archive_name)) {
        (void)cli_close_fd(archive_fd);
        (void)cbm_unlink(tmp_archive);
        return CLI_TRUE;
    }
    /* Fail closed: install only a positively-verified download. A mismatch,
     * a missing checksum entry, or an unavailable hash tool (crc != 0) all
     * abort rather than install an unverified binary. */
    char verified_sha256[SHA256_BUF_SIZE];
    int crc = verify_download_checksum(archive_fd, archive_name, verified_sha256);
    if (crc != 0) {
        (void)fprintf(stderr, "error: refusing to install an unverified download\n");
        (void)cli_close_fd(archive_fd);
        (void)cbm_unlink(tmp_archive);
        return CLI_TRUE;
    }

    unsigned char *binary = NULL;
    int binary_length = 0;
    int install_rc = extract_verified_binary(
        (extract_install_args_t){
            .archive_fd = archive_fd, .ext = ext, .verified_sha256 = verified_sha256},
        &binary, &binary_length);
    if (cli_close_fd(archive_fd) != 0) {
        install_rc = CLI_TRUE;
    }
    (void)cbm_unlink(tmp_archive);
    if (install_rc != 0) {
        free(binary);
        return CLI_TRUE;
    }
    install_rc = cli_publish_verified_binary(bin_dest, binary, binary_length, CLI_OCTAL_PERM,
                                             cli_stop_update_target, NULL,
                                             cli_verify_published_update, NULL);
    free(binary);
    if (install_rc != CLI_OK) {
        (void)fprintf(stderr,
                      "error: new binary failed publication or verification; the previous binary "
                      "was kept or rollback was attempted\n");
        return CLI_TRUE;
    }
    return CLI_OK;
}

/* Select update variant. Returns 0=standard, 1=ui, -1=error. */
static int select_update_variant(int variant_flag) {
    if (variant_flag == VARIANT_A) {
        return 0;
    }
    if (variant_flag == VARIANT_B) {
        return CLI_TRUE;
    }
#ifndef _WIN32
    if (!isatty(fileno(stdin))) {
        (void)fprintf(stderr, "error: variant selection requires a terminal. "
                              "Use --standard or --ui flag.\n");
        return CLI_ERR;
    }
#endif
    printf("Which binary variant do you want?\n");
    printf("  1) standard  — MCP server only\n");
    printf("  2) ui        — MCP server + embedded graph visualization\n");
    printf("Choose (1/2): ");
    (void)fflush(stdout);
    char choice[CLI_BUF_16];
    if (!fgets(choice, sizeof(choice), stdin)) {
        (void)fprintf(stderr, "error: failed to read input\n");
        return CLI_ERR;
    }
    return (choice[0] == '2') ? CLI_TRUE : 0;
}

/* Case-insensitive prefix match (portable — no strncasecmp dependency). */
static bool prefix_icase(const char *s, const char *prefix) {
    if (!s || !prefix) {
        return false;
    }
    while (*prefix) {
        if (*s == '\0') {
            return false;
        }
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static bool cli_valid_release_tag(const char *tag, size_t len) {
    if (!tag || len < CLI_PAIR_LEN || tag[0] != 'v' || !isdigit((unsigned char)tag[1])) {
        return false;
    }
    for (size_t i = CLI_SKIP_ONE; i < len; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (!isalnum(c) && c != '.' && c != '-' && c != '+') {
            return false;
        }
    }
    return true;
}

/* Pure, length-bounded redirect parser used by fetch_latest_tag and sanitizer
 * regression tests. It rejects embedded NULs and path/query punctuation so a
 * header cannot smuggle an unintended asset name into version handling. */
char *cbm_cli_parse_latest_tag_for_test(const char *headers, size_t header_len) {
    if (!headers || header_len == 0 || header_len == SIZE_MAX ||
        memchr(headers, '\0', header_len)) {
        return NULL;
    }
    char *owned = malloc(header_len + CLI_SKIP_ONE);
    if (!owned) {
        return NULL;
    }
    memcpy(owned, headers, header_len);
    owned[header_len] = '\0';

    char *tag = NULL;
    char *cursor = owned;
    char *end = owned + header_len;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        char *line_end = newline ? newline : end;
        while (line_end > cursor &&
               (line_end[-1] == '\r' || line_end[-1] == ' ' || line_end[-1] == '\t')) {
            line_end--;
        }
        char saved = *line_end;
        *line_end = '\0';
        if (prefix_icase(cursor, "location:")) {
            char *value = cursor + strlen("location:");
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            char *slash = strrchr(value, '/');
            if (slash) {
                const char *candidate = slash + CLI_SKIP_ONE;
                size_t candidate_len = strlen(candidate);
                if (cli_valid_release_tag(candidate, candidate_len)) {
                    tag = strdup(candidate);
                }
            }
        }
        *line_end = saved;
        if (tag || !newline) {
            break;
        }
        cursor = newline + CLI_SKIP_ONE;
    }
    free(owned);
    return tag;
}

/* Fetch latest release tag from GitHub via redirect header.
 * Returns heap-allocated tag (e.g. "v0.5.7") or NULL on failure. */
static char *fetch_latest_tag(void) {
    char curl_path[CLI_BUF_4K];
    if (!cli_resolve_curl(curl_path, sizeof(curl_path))) {
        return NULL;
    }
    const char *argv[] = {curl_path, "-sfI", CBM_GITHUB_LATEST_RELEASE_URL, NULL};
    cbm_proc_opts_t opts = {
        .bin = curl_path, .argv = argv, .discard_stderr = true, .quiet_timeout_ms = 30000};
    char *headers = NULL;
    size_t header_len = 0;
    char header_sha256[CBM_SHA256_HEX_LEN + 1];
    cbm_proc_result_t result;
    if (cbm_subprocess_capture(&opts, 64U * 1024U, &headers, &header_len, header_sha256, &result) !=
        0) {
        free(headers);
        return NULL;
    }
    char *tag = cbm_cli_parse_latest_tag_for_test(headers, header_len);
    free(headers);
    return tag;
}

/* Check if current version is already latest. Returns true to skip update. */
static bool check_already_latest(void) {
    const char *dl_env = getenv("CBM_DOWNLOAD_URL");
    if (dl_env && dl_env[0]) {
        return false; /* testing override — always update */
    }
    char *latest = fetch_latest_tag();
    if (!latest) {
        (void)fprintf(stderr, "warning: could not check latest version (network unavailable?). "
                              "Proceeding with update.\n");
        return false;
    }
    int cmp = cbm_compare_versions(latest, CBM_VERSION);
    if (cmp <= 0) {
        if (cmp < 0) {
            printf("Already up to date (%s, ahead of latest %s).\n", CBM_VERSION, latest);
        } else {
            printf("Already up to date (%s).\n", CBM_VERSION);
        }
        free(latest);
        return true;
    }
    printf("Update available: %s -> %s\n", CBM_VERSION, latest);
    free(latest);
    return false;
}

int cbm_cmd_update(int argc, char **argv) {
    parse_auto_answer(argc, argv);

    bool dry_run = false;
    bool force = false;
    int variant_flag = 0; /* 0 = ask, 1 = standard, 2 = ui */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--standard") == 0) {
            variant_flag = VARIANT_A;
        } else if (strcmp(argv[i], "--ui") == 0) {
            variant_flag = VARIANT_B;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = true;
        }
    }

    const char *home = cbm_get_home_dir();
    if (!home) {
        (void)fprintf(stderr, "error: HOME not set (use USERPROFILE on Windows)\n");
        return CLI_TRUE;
    }

    printf("codebase-memory-mcp update (current: %s)\n\n", CBM_VERSION);

    /* Version check — skip download if already on latest (not in dry-run). */
    if (!force && !dry_run && check_already_latest()) {
        return 0;
    }

    char self_path[CLI_BUF_1K] = {0};
    char bin_dest[CLI_BUF_1K] = {0};
    cbm_detect_self_path(self_path, sizeof(self_path), home);
    if (cbm_resolve_update_target(home, self_path, bin_dest, sizeof(bin_dest)) != CLI_OK) {
        (void)fprintf(
            stderr,
            "error: this installation is not owned by the official installer.\n"
            "Update it with its package manager, or reinstall using install.sh/install.ps1.\n");
        return CLI_TRUE;
    }

    /* Step 1: confirm, but do not yet perform, a requested index reset. */
    bool reset_indexes_after_install = false;
    if (update_plan_index_reset(home, dry_run, &reset_indexes_after_install) != 0) {
        return CLI_TRUE;
    }

    /* Step 2: Determine variant */
    int want_ui_rc = select_update_variant(variant_flag);
    if (want_ui_rc < 0) {
        return CLI_TRUE;
    }
    bool want_ui = (want_ui_rc == CLI_TRUE);
    const char *variant = want_ui ? "ui-" : "";
    const char *variant_label = want_ui ? "ui" : "standard";

    const char *os = detect_os();
    const char *arch = detect_arch();
    const char *ext = strcmp(os, "windows") == 0 ? "zip" : "tar.gz";

    char url[CLI_BUF_512];
    if (!build_update_url(url, sizeof(url), os, arch, ext, want_ui)) {
        (void)fprintf(stderr, "error: update download URL is too long or invalid\n");
        return CLI_TRUE;
    }

    if (dry_run) {
        printf("\nWould download %s binary for %s/%s ...\n", variant_label, os, arch);
    } else {
        printf("\nDownloading %s binary for %s/%s ...\n", variant_label, os, arch);
    }
    printf("  %s\n", url);

    if (dry_run) {
        printf("\n(dry-run — skipping download, extraction, and binary replacement)\n");
        printf("  target: %s\n", bin_dest);
        printf("  variant: %s\n", variant_label);
        printf("  os/arch: %s/%s\n", os, arch);
        printf("\nUpdate dry-run complete.\n");
        (void)variant;
        return 0;
    }

    /* Step 4-5: Download, verify, and install binary */
    char bin_dir[CLI_BUF_1K];
    parent_dir_copy(bin_dest, bin_dir, sizeof(bin_dir));
    cbm_mkdir_p(bin_dir, CLI_OCTAL_PERM);

    int rc = download_verify_install(url, ext, os, arch, want_ui, bin_dest);
    if (rc != 0) {
        return CLI_TRUE;
    }

    /* Only an installed and executable binary authorizes the deferred reset. */
    int removed_indexes = 0;
    if (reset_indexes_after_install) {
        removed_indexes = cbm_remove_indexes(home);
        if (removed_indexes < 0) {
            (void)fprintf(stderr,
                          "warning: new binary installed, but index cleanup was incomplete; "
                          "re-index manually\n");
            removed_indexes = 0;
        } else {
            printf("Removed %d index(es).\n\n", removed_indexes);
        }
    }

    /* Step 6: Refresh all agent configs (skills, MCP entries, hooks) */
    printf("Refreshing agent configurations...\n");
    cbm_install_agent_configs(home, bin_dest, true, false);

    printf("\nUpdate complete.\n");

    if (reset_indexes_after_install && removed_indexes > 0) {
        printf(
            "\nProject indexes were cleared after the successful update. They will be rebuilt\n");
        printf("automatically when you next use the MCP server.\n");
    } else {
        printf("\nExisting project indexes were kept. Re-index to apply extraction changes.\n");
    }
    printf("\nPlease restart your MCP client to use the new binary.\n");
    (void)variant;
    return 0;
}

/* ── CLI tool arguments (flags / --args-file / --help) ────────────── */

/* Flag-name normalization: kebab-case CLI flags map to snake_case JSON keys
 * (--name-pattern -> name_pattern). In-place; buffer is NUL-terminated. */
static void cli_kebab_to_snake(char *s) {
    for (; *s; s++) {
        if (*s == '-') {
            *s = '_';
        }
    }
}

/* snake_case JSON key -> kebab-case flag name (for --help display). In-place. */
static void cli_snake_to_kebab(char *s) {
    for (; *s; s++) {
        if (*s == '_') {
            *s = '-';
        }
    }
}

/* Heap-format a one-argument error message for *err_out. Caller frees. */
static char *cli_heap_msgf(const char *fmt, const char *arg) {
    char buf[CLI_BUF_512];
    snprintf(buf, sizeof(buf), fmt, arg);
    return cbm_strdup(buf);
}

/* Levenshtein distance for near-miss flag suggestions (two-row DP; inputs
 * are schema property names, well under the buffer sizes used here). */
static int cli_edit_distance(const char *a, const char *b) {
    enum { CLI_ED_MAX = 128 };
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la >= CLI_ED_MAX || lb >= CLI_ED_MAX) {
        return CLI_ED_MAX;
    }
    int prev[CLI_ED_MAX + 1];
    int cur[CLI_ED_MAX + 1];
    for (size_t j = 0; j <= lb; j++) {
        prev[j] = (int)j;
    }
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            cur[j] = m < sub ? m : sub;
        }
        memcpy(prev, cur, (lb + 1) * sizeof(int));
    }
    return prev[lb];
}

/* Closest schema property to `key` for a "did you mean" suggestion, or NULL
 * when nothing is plausibly near (distance > half the key length, min 2). */
static const char *cli_closest_prop(yyjson_val *props, const char *key) {
    const char *best = NULL;
    int best_d = 0;
    size_t idx;
    size_t max;
    yyjson_val *k;
    yyjson_val *v;
    yyjson_obj_foreach(props, idx, max, k, v) {
        const char *name = yyjson_get_str(k);
        if (!name) {
            continue;
        }
        int d = cli_edit_distance(key, name);
        if (!best || d < best_d) {
            best = name;
            best_d = d;
        }
    }
    int limit = (int)(strlen(key) / 2);
    if (limit < 2) {
        limit = 2;
    }
    return (best && best_d <= limit) ? best : NULL;
}

/* True if the schema's required[] array contains `key`. */
static bool cli_schema_required_has(yyjson_val *required, const char *key) {
    if (!required || !yyjson_is_arr(required)) {
        return false;
    }
    size_t idx;
    size_t max;
    yyjson_val *v;
    yyjson_arr_foreach(required, idx, max, v) {
        if (yyjson_is_str(v) && strcmp(yyjson_get_str(v), key) == 0) {
            return true;
        }
    }
    return false;
}

/* Look up a property's JSON-schema "type" string (string/integer/number/
 * boolean/array). Returns NULL when the schema or property is unknown — the
 * caller then treats the value as a plain string. */
static const char *cli_schema_type(yyjson_val *props, const char *key) {
    if (!props || !yyjson_is_obj(props)) {
        return NULL;
    }
    yyjson_val *p = yyjson_obj_get(props, key);
    if (!p || !yyjson_is_obj(p)) {
        return NULL;
    }
    yyjson_val *t = yyjson_obj_get(p, "type");
    return (t && yyjson_is_str(t)) ? yyjson_get_str(t) : NULL;
}

/* Append a typed value to the output object under `key`. For array-typed
 * properties, repeated flags accumulate into a single JSON array. */
static void cli_add_typed(yyjson_mut_doc *out, yyjson_mut_val *obj, const char *key,
                          const char *type, const char *value, bool have_value) {
    if (type && strcmp(type, "array") == 0) {
        yyjson_mut_val *arr = yyjson_mut_obj_get(obj, key);
        if (!arr || !yyjson_mut_is_arr(arr)) {
            arr = yyjson_mut_arr(out);
            yyjson_mut_obj_add(obj, yyjson_mut_strcpy(out, key), arr);
        }
        yyjson_mut_arr_add_strcpy(out, arr, have_value ? value : "");
        return;
    }

    yyjson_mut_val *vv;
    if (type && strcmp(type, "boolean") == 0) {
        bool b = !have_value || strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
                 strcmp(value, "yes") == 0;
        vv = yyjson_mut_bool(out, b);
    } else if (type && strcmp(type, "integer") == 0) {
        char *endp = NULL;
        const char *v = have_value ? value : "";
        long n = strtol(v, &endp, CLI_STRTOL_BASE);
        vv = (endp && endp != v && *endp == '\0') ? yyjson_mut_int(out, (int64_t)n)
                                                  : yyjson_mut_strcpy(out, v);
    } else if (type && strcmp(type, "number") == 0) {
        char *endp = NULL;
        const char *v = have_value ? value : "";
        double d = strtod(v, &endp);
        vv = (endp && endp != v && *endp == '\0') ? yyjson_mut_real(out, d)
                                                  : yyjson_mut_strcpy(out, v);
    } else {
        /* string or unknown type */
        vv = yyjson_mut_strcpy(out, have_value ? value : "");
    }
    yyjson_mut_obj_add(obj, yyjson_mut_strcpy(out, key), vv);
}

char *cbm_cli_build_args_json(const char *tool_name, int argc, char **argv, char **err_out) {
    if (err_out) {
        *err_out = NULL;
    }

    /* The tool's input_schema (may be NULL for an unknown tool — then every
     * value is treated as a string). Static lifetime; do not free. */
    const char *schema_str = cbm_mcp_tool_input_schema(tool_name);
    yyjson_doc *schema_doc = NULL;
    yyjson_val *props = NULL;
    if (schema_str) {
        schema_doc = yyjson_read(schema_str, strlen(schema_str), 0);
        if (schema_doc) {
            props = yyjson_obj_get(yyjson_doc_get_root(schema_doc), "properties");
        }
    }

    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
    if (!out) {
        if (schema_doc) {
            yyjson_doc_free(schema_doc);
        }
        return NULL;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(out);
    yyjson_mut_doc_set_root(out, obj);

    bool ok = true;
    for (int i = 0; i < argc && ok; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            break; /* end of flag parsing */
        }
        if (strncmp(arg, "--", CLI_PAIR_LEN) != 0) {
            if (err_out) {
                *err_out = cli_heap_msgf("unexpected argument '%s' (expected --flag value)", arg);
            }
            ok = false;
            break;
        }

        const char *body = arg + CLI_PAIR_LEN; /* skip leading "--" */
        const char *eq = strchr(body, '=');
        char key[CLI_BUF_256];
        const char *value = NULL;
        bool have_value = false;

        if (eq) {
            /* --key=value : split on the FIRST '='; value may contain '='/spaces. */
            size_t klen = (size_t)(eq - body);
            if (klen >= sizeof(key)) {
                klen = sizeof(key) - CLI_SKIP_ONE;
            }
            memcpy(key, body, klen);
            key[klen] = '\0';
            value = eq + CLI_SKIP_ONE;
            have_value = true;
        } else {
            snprintf(key, sizeof(key), "%s", body);
            /* Consume the next token as the value unless it is itself a flag
             * (then this is a bare boolean/string flag). */
            if (i + CLI_SKIP_ONE < argc &&
                strncmp(argv[i + CLI_SKIP_ONE], "--", CLI_PAIR_LEN) != 0) {
                value = argv[i + CLI_SKIP_ONE];
                have_value = true;
                i++;
            }
        }

        cli_kebab_to_snake(key);
        const char *type = cli_schema_type(props, key);

        /* Unknown flag for a known tool: reject loudly (#997). Silently
         * typing it as a string ships it as an ignored JSON arg — the
         * server applies its default and the caller gets silently-wrong
         * output (e.g. `trace_path --max-depth 1` traced at depth 3). */
        if (props && !type) {
            char kebab_key[CLI_BUF_256];
            snprintf(kebab_key, sizeof(kebab_key), "%s", key);
            cli_snake_to_kebab(kebab_key);
            const char *close = cli_closest_prop(props, key);
            char suggestion[CLI_BUF_256] = "";
            if (close) {
                char close_kebab[CLI_BUF_256];
                snprintf(close_kebab, sizeof(close_kebab), "%s", close);
                cli_snake_to_kebab(close_kebab);
                snprintf(suggestion, sizeof(suggestion), " (did you mean --%s?)", close_kebab);
            }
            if (err_out) {
                char buf[CLI_BUF_512];
                snprintf(buf, sizeof(buf),
                         "unknown flag --%s for this tool%s — run 'cli %s --help' for the "
                         "supported flags",
                         kebab_key, suggestion, tool_name);
                *err_out = cbm_strdup(buf);
            }
            ok = false;
            break;
        }

        if (type && strcmp(type, "array") == 0 && !have_value) {
            if (err_out) {
                *err_out = cli_heap_msgf("flag --%s requires a value", key);
            }
            ok = false;
            break;
        }

        cli_add_typed(out, obj, key, type, value, have_value);
    }

    char *result = NULL;
    if (ok) {
        size_t len = 0;
        result = yyjson_mut_write(out, 0, &len); /* malloc'd; caller frees */
    }

    yyjson_mut_doc_free(out);
    if (schema_doc) {
        yyjson_doc_free(schema_doc);
    }
    return result;
}

int cbm_cli_print_tool_help(const char *tool_name) {
    const char *schema_str = cbm_mcp_tool_input_schema(tool_name);
    if (!schema_str) {
        return CLI_ERR;
    }

    yyjson_doc *doc = yyjson_read(schema_str, strlen(schema_str), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *props = root ? yyjson_obj_get(root, "properties") : NULL;
    yyjson_val *required = root ? yyjson_obj_get(root, "required") : NULL;

    printf("Usage:\n");
    printf("  codebase-memory-mcp cli %s --flag value [--flag2 value2 ...]\n", tool_name);
    printf("  codebase-memory-mcp cli %s --args-file <path-to-json>\n", tool_name);
    printf("  echo '<json>' | codebase-memory-mcp cli %s\n", tool_name);
    printf("  codebase-memory-mcp cli %s '<raw-json-args>'\n", tool_name);

    printf("\nFlags:\n");
    if (props && yyjson_is_obj(props)) {
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(props, &iter);
        yyjson_val *pkey;
        while ((pkey = yyjson_obj_iter_next(&iter)) != NULL) {
            yyjson_val *pval = yyjson_obj_iter_get_val(pkey);
            const char *name = yyjson_get_str(pkey);
            if (!name) {
                continue;
            }
            const char *type = "string";
            const char *desc = "";
            if (yyjson_is_obj(pval)) {
                yyjson_val *t = yyjson_obj_get(pval, "type");
                if (t && yyjson_is_str(t)) {
                    type = yyjson_get_str(t);
                }
                yyjson_val *d = yyjson_obj_get(pval, "description");
                if (d && yyjson_is_str(d)) {
                    desc = yyjson_get_str(d);
                }
            }
            char flag[CLI_BUF_256];
            snprintf(flag, sizeof(flag), "%s", name);
            cli_snake_to_kebab(flag);
            bool req = cli_schema_required_has(required, name);
            printf("  --%s <%s>%s", flag, type, req ? " [required]" : "");
            if (desc[0]) {
                printf("  %s", desc);
            }
            printf("\n");
        }
    }

    if (doc) {
        yyjson_doc_free(doc);
    }
    return CLI_OK;
}
