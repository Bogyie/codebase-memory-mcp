/*
 * compat_fs.h — Portable directory iteration, popen, and file operations.
 *
 * POSIX: thin wrappers around opendir/readdir, popen/pclose, mkdir, unlink.
 * Windows: FindFirstFile/FindNextFile, _popen/_pclose, _mkdir, _unlink.
 */
#ifndef CBM_COMPAT_FS_H
#define CBM_COMPAT_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ── Directory iteration ──────────────────────────────────────── */

/* A Windows filename may contain 255 UTF-16 code units, which can require up
 * to 1020 UTF-8 bytes. Keep the converted name lossless. */
#define CBM_DIRENT_NAME_MAX 1024

typedef struct cbm_dir cbm_dir_t;

typedef struct {
    char name[CBM_DIRENT_NAME_MAX];
    bool is_dir;
    bool is_reparse;      /* Windows junction/symlink; POSIX symbolic link */
    unsigned char d_type; /* DT_REG, DT_DIR, DT_LNK, etc. (POSIX only, 0 on Windows) */
} cbm_dirent_t;

/* Open a directory for iteration. Returns NULL on error. */
cbm_dir_t *cbm_opendir(const char *path);

/* Read next entry. Returns NULL when done. The returned pointer is
 * valid until the next cbm_readdir call on the same handle. */
cbm_dirent_t *cbm_readdir(cbm_dir_t *d);

/* Distinguish a clean end-of-directory from conversion, allocation, or OS
 * iteration failure after cbm_readdir() returns NULL. */
bool cbm_dir_had_error(const cbm_dir_t *d);

/* Close directory handle. */
void cbm_closedir(cbm_dir_t *d);

/* ── Portable popen/pclose ────────────────────────────────────── */

FILE *cbm_popen(const char *cmd, const char *mode);
int cbm_pclose(FILE *f);

/* ── File operations ──────────────────────────────────────────── */

/* Create directory (and parents). mode is ignored on Windows. Returns true
 * only when every final path component resolves to a directory; existing
 * symlinks/junctions to directories retain their historical acceptance. */
bool cbm_mkdir_p(const char *path, int mode);

/* Delete a file. Returns 0 on success. */
int cbm_unlink(const char *path);
/* Remove <db_path>-wal/-shm. MUST be called by any path installing a fresh
 * DB file where a previous generation lived — a leftover WAL is otherwise
 * replayed on top of the new file at the next open (#897). */
void cbm_remove_db_sidecars(const char *db_path);
/* rename() that replaces an existing destination on every platform
 * (Windows rename fails with EEXIST; this uses MoveFileExW there). */
int cbm_rename_replace(const char *src, const char *dst);
/* Canonicalize an EXISTING path (realpath / wide handle final path), resolving
 * symlinks and Windows reparse points. Locale-independent on Windows — never
 * routes UTF-8 through the ANSI CRT (#973). The output size is checked against
 * out_sz; small buffers fail without writing past the end.
 * Returns 1 on success and 0 on failure (it is an int for legacy ABI reasons;
 * do not interpret 0 as a POSIX-style success status). */
int cbm_canonical_path(const char *path, char *out, size_t out_sz);
bool cbm_path_is_reparse_point(const char *path);
/* UTF-8-safe existence probe: 1 exists, 0 definitely absent, -1 for invalid
 * input, permission, conversion, or other OS errors. */
int cbm_path_probe(const char *path);

/* Delete an empty directory. Returns 0 on success. */
int cbm_rmdir(const char *path);

/* Open a file by UTF-8 path.
 * On Windows, converts to wide-char and calls _wfopen so paths with
 * non-ASCII characters (accents, CJK, etc.) are handled correctly.
 * On POSIX, delegates to fopen. mode must be an ASCII string. */
FILE *cbm_fopen(const char *path, const char *mode);

/* Execute a command without shell interpretation.
 * argv is a NULL-terminated array: {"cmd", "arg1", "arg2", NULL}.
 * Returns the process exit code, or -1 on fork/exec failure.
 * POSIX: fork() + execvp(). Windows: CreateProcess with proper quoting. */
int cbm_exec_no_shell(const char *const *argv);

#endif /* CBM_COMPAT_FS_H */
