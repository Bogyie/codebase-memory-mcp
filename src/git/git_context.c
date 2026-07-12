#include "git/git_context.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"
#include "foundation/str_util.h"
#include "foundation/subprocess.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "foundation/win_utf8.h"
#include <wctype.h>
#else
#include <unistd.h>
#endif

enum {
    GIT_OUTPUT_MAX = 16384,
    GIT_MAX_ARGS = 16,
    GIT_TIMEOUT_MS = 30000,
};

static char *git_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

static void trim_newlines(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

#ifdef _WIN32
static bool git_win_absolute(const wchar_t *path) {
    return path &&
           ((iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
            (path[0] == L'\\' && path[1] == L'\\'));
}

static bool git_win_resolve_candidate(const wchar_t *candidate, char out[GIT_OUTPUT_MAX]) {
    if (!git_win_absolute(candidate)) {
        return false;
    }
    HANDLE file = CreateFileW(candidate, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(candidate);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(file);
        return false;
    }
    DWORD need = GetFinalPathNameByHandleW(file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (need == 0 || need > 32768U) {
        CloseHandle(file);
        return false;
    }
    wchar_t *final_path = (wchar_t *)malloc(((size_t)need + 1U) * sizeof(*final_path));
    if (!final_path) {
        CloseHandle(file);
        return false;
    }
    DWORD written = GetFinalPathNameByHandleW(file, final_path, need + 1U,
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (written == 0 || written > need) {
        free(final_path);
        return false;
    }
    char *utf8 = cbm_wide_to_utf8(final_path);
    free(final_path);
    if (!utf8) {
        return false;
    }
    size_t len = strlen(utf8);
    bool fits = len > 0 && len < GIT_OUTPUT_MAX;
    if (fits) {
        memcpy(out, utf8, len + 1U);
    }
    free(utf8);
    return fits;
}

static wchar_t *git_win_environment(const wchar_t *name) {
    DWORD need = GetEnvironmentVariableW(name, NULL, 0);
    if (need == 0 || need > 32768U) {
        return NULL;
    }
    wchar_t *value = (wchar_t *)malloc((size_t)need * sizeof(*value));
    if (!value) {
        return NULL;
    }
    DWORD written = GetEnvironmentVariableW(name, value, need);
    if (written == 0 || written >= need) {
        free(value);
        return NULL;
    }
    return value;
}

static bool git_win_resolve_from_path(char out[GIT_OUTPUT_MAX]) {
    wchar_t *path = git_win_environment(L"PATH");
    if (!path) {
        return false;
    }
    bool found = false;
    wchar_t *cursor = path;
    while (*cursor && !found) {
        wchar_t *end = wcschr(cursor, L';');
        if (end) {
            *end = L'\0';
        }
        wchar_t *start = cursor;
        size_t len = wcslen(start);
        if (len >= 2U && start[0] == L'"' && start[len - 1U] == L'"') {
            start[len - 1U] = L'\0';
            start++;
            len -= 2U;
        }
        if (len > 0 && git_win_absolute(start) && len <= SIZE_MAX - 10U) {
            bool separator = start[len - 1U] != L'\\' && start[len - 1U] != L'/';
            wchar_t *candidate =
                (wchar_t *)malloc((len + (separator ? 1U : 0U) + 8U) * sizeof(*candidate));
            if (candidate) {
                memcpy(candidate, start, len * sizeof(*candidate));
                size_t pos = len;
                if (separator) {
                    candidate[pos++] = L'\\';
                }
                memcpy(candidate + pos, L"git.exe", 8U * sizeof(*candidate));
                found = git_win_resolve_candidate(candidate, out);
                free(candidate);
            }
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    free(path);
    return found;
}
#endif

static bool resolve_git_binary(char out[GIT_OUTPUT_MAX]) {
#ifdef _WIN32
    wchar_t *configured = git_win_environment(L"CBM_GIT_BIN");
    bool found = configured && git_win_resolve_candidate(configured, out);
    free(configured);
    return found || git_win_resolve_from_path(out);
#else
    char configured[GIT_OUTPUT_MAX] = "";
    if (cbm_safe_getenv("CBM_GIT_BIN", configured, sizeof(configured), NULL) &&
        configured[0] == '/' && access(configured, X_OK) == 0 &&
        cbm_canonical_path(configured, out, GIT_OUTPUT_MAX)) {
        return true;
    }
    char path_env[GIT_OUTPUT_MAX] = "";
    const char *path = cbm_safe_getenv("PATH", path_env, sizeof(path_env),
                                       "/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin");
    if (!path) {
        return false;
    }
    const char *cursor = path;
    while (*cursor) {
        const char *end = strchr(cursor, ':');
        size_t dir_len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (dir_len > 1 && cursor[0] == '/' && dir_len + sizeof("/git") < GIT_OUTPUT_MAX) {
            char candidate[GIT_OUTPUT_MAX];
            int n = snprintf(candidate, sizeof(candidate), "%.*s/git", (int)dir_len, cursor);
            struct stat st;
            if (n > 0 && n < (int)sizeof(candidate) && stat(candidate, &st) == 0 &&
                S_ISREG(st.st_mode) && access(candidate, X_OK) == 0 &&
                cbm_canonical_path(candidate, out, GIT_OUTPUT_MAX)) {
                return true;
            }
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return false;
#endif
}

bool cbm_git_resolve_binary(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    char resolved[GIT_OUTPUT_MAX];
    if (!resolve_git_binary(resolved)) {
        return false;
    }
    size_t len = strlen(resolved);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, resolved, len + 1U);
    return true;
}

static int git_capture(const char *repo_path, const char *const *git_args, char **out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    *out = NULL;
    if (!repo_path || !git_args) {
        return CBM_NOT_FOUND;
    }
    char git_binary[GIT_OUTPUT_MAX];
    if (!resolve_git_binary(git_binary)) {
        return CBM_NOT_FOUND;
    }
    const char *argv[GIT_MAX_ARGS];
    int argc = 0;
    argv[argc++] = git_binary;
    argv[argc++] = "-C";
    argv[argc++] = repo_path;
    for (int i = 0; git_args[i]; i++) {
        if (argc + 1 >= GIT_MAX_ARGS) {
            return CBM_NOT_FOUND;
        }
        argv[argc++] = git_args[i];
    }
    argv[argc] = NULL;
    cbm_proc_opts_t opts = {
        .bin = git_binary,
        .argv = argv,
        .quiet_timeout_ms = GIT_TIMEOUT_MS,
        .discard_stderr = true,
    };
    char *data = NULL;
    size_t data_len = 0;
    char digest[CBM_SHA256_HEX_LEN + 1];
    cbm_proc_result_t process;
    if (cbm_subprocess_capture(&opts, GIT_OUTPUT_MAX - 1U, &data, &data_len, digest, &process) !=
        0) {
        return CBM_NOT_FOUND;
    }
    if (data_len == 0 || memchr(data, '\0', data_len) || memchr(data, '\n', data_len - 1U) ||
        memchr(data, '\r', data_len - 1U)) {
        free(data);
        return CBM_NOT_FOUND;
    }
    trim_newlines(data);
    if (data[0] == '\0') {
        free(data);
        return CBM_NOT_FOUND;
    }
    *out = data;
    return 0;
}

static bool revision_is_object_id(const char *revision) {
    if (!revision) {
        return true;
    }
    size_t len = strlen(revision);
    if (len != 40 && len != CBM_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)revision[i])) {
            return false;
        }
    }
    return true;
}

int cbm_git_history_capture(const char *repo_path, const char *revision, char **out_data,
                            size_t *out_len, char out_sha256[CBM_SHA256_HEX_LEN + 1]) {
    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out_sha256) {
        out_sha256[0] = '\0';
    }
    if (!repo_path || !out_data || !out_len || !out_sha256 || !revision_is_object_id(revision)) {
        return CBM_NOT_FOUND;
    }
    char git_binary[GIT_OUTPUT_MAX];
    if (!resolve_git_binary(git_binary)) {
        return CBM_NOT_FOUND;
    }
    const char *argv[] = {
        git_binary,
        "-C",
        repo_path,
        "log",
        "-z",
        "--name-only",
        "--pretty=format:COMMIT:%H:%ct%x00",
        "--since=1 year ago",
        "--max-count=10000",
        revision ? revision : "HEAD",
        NULL,
    };
    cbm_proc_opts_t opts = {
        .bin = git_binary,
        .argv = argv,
        .quiet_timeout_ms = GIT_TIMEOUT_MS,
        .discard_stderr = true,
    };
    cbm_proc_result_t process;
    return cbm_subprocess_capture(&opts, CBM_GIT_HISTORY_MAX_BYTES, out_data, out_len, out_sha256,
                                  &process) == 0
               ? 0
               : CBM_NOT_FOUND;
}

static bool path_is_absolute(const char *path) {
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/') {
        return true;
    }
#ifdef _WIN32
    return isalpha((unsigned char)path[0]) && path[1] == ':';
#else
    return false;
#endif
}

static char *join_root_relative(const char *root, const char *rel) {
    if (!root || !root[0]) {
        return git_strdup(rel);
    }
    int n = snprintf(NULL, 0, "%s/%s", root, rel);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s/%s", root, rel);
    return out;
}

/* Derive the canonical repo root.
 *
 * Preferred (git 2.31+): abs_common_dir is `git rev-parse --path-format=absolute
 * --git-common-dir` — git's OWN absolute, canonical common-dir. Because a main
 * repo and its linked worktree both ask the same git binary, they resolve to the
 * IDENTICAL path, so canonical_root is consistent across worktrees AND platforms
 * with no manual join or realpath/_fullpath — which diverged under msys vs native
 * path representations (#659 root cause, and the worktree==main-root breakage in
 * test_pipeline.c git_context_linked_worktree). git also resolves the relative
 * ".." internally, so the subdirectory case (#659) is fixed here too.
 *
 * Fallback (git < 2.31, no --path-format → abs_common_dir empty): the relative
 * --git-common-dir is relative to input_path (the -C dir), so join against it and
 * realpath-normalize the "..". Unix only — on Windows git emits an absolute
 * common-dir so this branch isn't reached in practice, and _fullpath there
 * reintroduces the msys divergence. */
static char *derive_canonical_root(const char *input_path, const char *worktree_root,
                                   const char *git_common_dir, const char *abs_common_dir) {
    char *root = NULL;
    if (abs_common_dir && abs_common_dir[0] && path_is_absolute(abs_common_dir)) {
        root = git_strdup(abs_common_dir);
        if (!root) {
            return NULL;
        }
    } else {
        const char *src = git_common_dir && git_common_dir[0] ? git_common_dir : worktree_root;
        if (!src) {
            return git_strdup("");
        }
#ifndef _WIN32
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(input_path, src);
        if (!root) {
            return NULL;
        }
        {
            char resolved[4096];
            if (realpath(root, resolved) != NULL) {
                free(root);
                root = git_strdup(resolved);
                if (!root) {
                    return NULL;
                }
            }
        }
#else
        (void)input_path;
        root = path_is_absolute(src) ? git_strdup(src) : join_root_relative(worktree_root, src);
        if (!root) {
            return NULL;
        }
#endif
    }

    size_t len = strlen(root);
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) {
        root[--len] = '\0';
    }

    if (len >= 5 && strcmp(root + len - 5, "/.git") == 0) {
        root[len - 5] = '\0';
    }
#ifdef _WIN32
    else if (len >= 5 && strcmp(root + len - 5, "\\.git") == 0) {
        root[len - 5] = '\0';
    }
#endif

    return root;
}

static char *slug_from_branch(const char *branch, bool detached) {
    const char *fallback = detached ? "detached" : "working-tree";
    const char *src = detached ? fallback : (branch && branch[0] ? branch : fallback);
    size_t len = strlen(src);
    char *slug = (char *)malloc(len + 1);
    if (!slug) {
        return NULL;
    }

    size_t j = 0;
    bool in_dash = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            if (j == 0 && c == '-') {
                in_dash = true;
                continue;
            }
            slug[j++] = (char)c;
            in_dash = false;
        } else if (j > 0 && !in_dash) {
            slug[j++] = '-';
            in_dash = true;
        }
    }
    while (j > 0 && slug[j - 1] == '-') {
        j--;
    }
    slug[j] = '\0';

    if (slug[0] == '\0') {
        free(slug);
        return git_strdup(fallback);
    }
    return slug;
}

void cbm_git_context_free(cbm_git_context_t *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx->input_path);
    free(ctx->worktree_root);
    free(ctx->git_dir);
    free(ctx->git_common_dir);
    free(ctx->canonical_root);
    free(ctx->branch);
    free(ctx->branch_slug);
    free(ctx->head_sha);
    free(ctx->base_sha);
    free(ctx->remote_url);
    memset(ctx, 0, sizeof(*ctx));
}

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        return CBM_NOT_FOUND;
    }

    out->input_path = git_strdup(path);
    if (!out->input_path) {
        return CBM_NOT_FOUND;
    }

    struct stat st;
    out->root_exists = (stat(path, &st) == 0);
    if (!out->root_exists) {
        return 0;
    }

    const char *show_toplevel[] = {"rev-parse", "--show-toplevel", NULL};
    if (git_capture(path, show_toplevel, &out->worktree_root) != 0) {
        out->is_git = false;
        return 0;
    }
    out->is_git = true;
    const char *inside_args[] = {"rev-parse", "--is-inside-work-tree", NULL};
    char *inside = NULL;
    out->inside_work_tree =
        git_capture(path, inside_args, &inside) == 0 && strcmp(inside, "true") == 0;
    free(inside);

    const char *git_dir_args[] = {"rev-parse", "--git-dir", NULL};
    if (git_capture(path, git_dir_args, &out->git_dir) != 0) {
        out->git_dir = git_strdup("");
    }
    const char *common_dir_args[] = {"rev-parse", "--git-common-dir", NULL};
    if (git_capture(path, common_dir_args, &out->git_common_dir) != 0) {
        out->git_common_dir = git_strdup("");
    }
    const char *head_args[] = {"rev-parse", "--verify", "HEAD", NULL};
    if (git_capture(path, head_args, &out->head_sha) != 0) {
        out->head_sha = git_strdup("");
    }

    const char *branch_args[] = {"symbolic-ref", "--quiet", "--short", "HEAD", NULL};
    if (git_capture(path, branch_args, &out->branch) != 0) {
        out->branch = git_strdup("DETACHED");
        out->is_detached = true;
    }

    out->is_worktree =
        out->git_dir && out->git_common_dir && strcmp(out->git_dir, out->git_common_dir) != 0;
    /* git 2.31+ canonical absolute common-dir (best-effort; NULL on older git,
     * where derive_canonical_root falls back to the relative common-dir). */
    char *abs_common_dir = NULL;
    const char *abs_common_args[] = {"rev-parse", "--path-format=absolute", "--git-common-dir",
                                     NULL};
    (void)git_capture(path, abs_common_args, &abs_common_dir);
    out->canonical_root =
        derive_canonical_root(path, out->worktree_root, out->git_common_dir, abs_common_dir);
    free(abs_common_dir);
    out->branch_slug = slug_from_branch(out->branch, out->is_detached);
    const char *base_args[] = {"merge-base", "HEAD", "@{upstream}", NULL};
    if (git_capture(path, base_args, &out->base_sha) != 0) {
        out->base_sha = git_strdup("");
    }
    const char *remote_args[] = {"config", "--get", "remote.origin.url", NULL};
    if (git_capture(path, remote_args, &out->remote_url) != 0) {
        out->remote_url = git_strdup("");
    }

    if (!out->git_dir || !out->git_common_dir || !out->head_sha || !out->branch ||
        !out->canonical_root || !out->branch_slug || !out->base_sha || !out->remote_url) {
        cbm_git_context_free(out);
        return CBM_NOT_FOUND;
    }

    return 0;
}

char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx) {
    const char *project = project_name && project_name[0] ? project_name : "project";
    const char *slug = "working-tree";
    if (ctx) {
        if (ctx->is_detached) {
            slug = "detached";
        } else if (ctx->is_git && ctx->branch_slug && ctx->branch_slug[0]) {
            slug = ctx->branch_slug;
        }
    }

    int n = snprintf(NULL, 0, "%s.__branch__.%s", project, slug);
    if (n < 0) {
        return NULL;
    }
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) {
        return NULL;
    }
    snprintf(out, (size_t)n + 1, "%s.__branch__.%s", project, slug);
    return out;
}

static bool append_fmt_checked(char *buf, int buf_size, int *off, const char *fmt, ...) {
    if (!buf || !off || buf_size <= 0 || *off < 0 || *off >= buf_size) {
        return false;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, (size_t)(buf_size - *off), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= buf_size - *off) {
        buf[buf_size - 1] = '\0';
        return false;
    }
    *off += n;
    return true;
}

static int json_escaped_len(const char *src) {
    if (!src) {
        return 0;
    }
    int len = 0;
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            len += 2;
        } else if (c < 0x20) {
            len += 6; /* \u00XX */
        } else {
            len++;
        }
    }
    return len;
}

static bool json_append_bool(char *buf, int buf_size, int *off, const char *name, bool value,
                             bool comma) {
    return append_fmt_checked(buf, buf_size, off, "\"%s\":%s%s", name, value ? "true" : "false",
                              comma ? "," : "");
}

static bool json_append_string(char *buf, int buf_size, int *off, const char *name,
                               const char *value, bool comma) {
    int needed = json_escaped_len(value ? value : "");
    char *escaped = malloc((size_t)needed + 1);
    if (!escaped) {
        return false;
    }
    int actual = cbm_json_escape(escaped, needed + 1, value ? value : "");
    bool ok = actual == needed && append_fmt_checked(buf, buf_size, off, "\"%s\":\"%s\"%s", name,
                                                     escaped, comma ? "," : "");
    free(escaped);
    return ok;
}

int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size) {
    if (!ctx || !buf || buf_size <= 0) {
        return 0;
    }

    int off = 0;
    bool ok =
        append_fmt_checked(buf, buf_size, &off, "{") &&
        json_append_bool(buf, buf_size, &off, "is_git", ctx->is_git, true) &&
        json_append_bool(buf, buf_size, &off, "inside_work_tree", ctx->inside_work_tree, true) &&
        json_append_bool(buf, buf_size, &off, "is_worktree", ctx->is_worktree, true) &&
        json_append_bool(buf, buf_size, &off, "is_detached", ctx->is_detached, true) &&
        json_append_bool(buf, buf_size, &off, "root_exists", ctx->root_exists, true) &&
        json_append_string(buf, buf_size, &off, "canonical_root", ctx->canonical_root, true) &&
        json_append_string(buf, buf_size, &off, "worktree_root", ctx->worktree_root, true) &&
        json_append_string(buf, buf_size, &off, "git_common_dir", ctx->git_common_dir, true) &&
        json_append_string(buf, buf_size, &off, "branch", ctx->branch, true) &&
        json_append_string(buf, buf_size, &off, "head_sha", ctx->head_sha, true) &&
        json_append_string(buf, buf_size, &off, "base_sha", ctx->base_sha, true) &&
        json_append_string(buf, buf_size, &off, "remote_url", ctx->remote_url, false) &&
        append_fmt_checked(buf, buf_size, &off, "}");
    if (!ok) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return off;
}

static void git_hash_u64(cbm_sha256_ctx *hash, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t)(value & 0xffU);
        value >>= 8;
    }
    cbm_sha256_update(hash, bytes, sizeof(bytes));
}

static void git_hash_string(cbm_sha256_ctx *hash, const char *value) {
    size_t len = value ? strlen(value) : 0;
    git_hash_u64(hash, (uint64_t)len);
    if (len > 0) {
        cbm_sha256_update(hash, value, len);
    }
}

int cbm_git_context_fingerprint(const cbm_git_context_t *ctx,
                                char out_sha256[CBM_SHA256_HEX_LEN + 1]) {
    if (!ctx || !out_sha256) {
        return CBM_NOT_FOUND;
    }
    static const char domain[] = "cbm-git-context-v1";
    cbm_sha256_ctx hash;
    cbm_sha256_init(&hash);
    cbm_sha256_update(&hash, domain, sizeof(domain));
    const uint8_t flags[] = {
        ctx->is_git ? 1U : 0U,      ctx->inside_work_tree ? 1U : 0U, ctx->is_worktree ? 1U : 0U,
        ctx->is_detached ? 1U : 0U, ctx->root_exists ? 1U : 0U,
    };
    cbm_sha256_update(&hash, flags, sizeof(flags));
    git_hash_string(&hash, ctx->input_path);
    git_hash_string(&hash, ctx->worktree_root);
    git_hash_string(&hash, ctx->git_dir);
    git_hash_string(&hash, ctx->git_common_dir);
    git_hash_string(&hash, ctx->canonical_root);
    git_hash_string(&hash, ctx->branch);
    git_hash_string(&hash, ctx->branch_slug);
    git_hash_string(&hash, ctx->head_sha);
    git_hash_string(&hash, ctx->base_sha);
    git_hash_string(&hash, ctx->remote_url);
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&hash, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out_sha256[i * 2] = hex[digest[i] >> 4];
        out_sha256[i * 2 + 1] = hex[digest[i] & 0x0fU];
    }
    out_sha256[CBM_SHA256_HEX_LEN] = '\0';
    return 0;
}
