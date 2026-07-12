#ifndef CBM_GIT_CONTEXT_H
#define CBM_GIT_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "foundation/sha256.h"

#define CBM_GIT_HISTORY_MAX_BYTES (64U * 1024U * 1024U)

typedef struct {
    bool is_git;
    bool inside_work_tree;
    bool is_worktree;
    bool is_detached;
    bool root_exists;
    char *input_path;
    char *worktree_root;
    char *git_dir;
    char *git_common_dir;
    char *canonical_root;
    char *branch;
    char *branch_slug;
    char *head_sha;
    char *base_sha;
    char *remote_url;
} cbm_git_context_t;

int cbm_git_context_resolve(const char *path, cbm_git_context_t *out);
void cbm_git_context_free(cbm_git_context_t *ctx);
char *cbm_git_context_branch_qn(const char *project_name, const cbm_git_context_t *ctx);
int cbm_git_context_props_json(const cbm_git_context_t *ctx, char *buf, int buf_size);
int cbm_git_context_fingerprint(const cbm_git_context_t *ctx,
                                char out_sha256[CBM_SHA256_HEX_LEN + 1]);

/* Resolve the exact executable used by git subprocesses. Only an explicit
 * absolute CBM_GIT_BIN or git.exe/git in an absolute PATH component is
 * accepted; relative/current-directory search is deliberately excluded. */
bool cbm_git_resolve_binary(char *out, size_t out_size);

/* Capture the exact bounded byte stream consumed by the history pass. argv is
 * used directly (no shell); revision must be a 40- or 64-digit hex object id. */
int cbm_git_history_capture(const char *repo_path, const char *revision, char **out_data,
                            size_t *out_len, char out_sha256[CBM_SHA256_HEX_LEN + 1]);

#endif
