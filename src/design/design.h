/*
 * design.h — Read-only Design Context indexing.
 *
 * Normalizes repository-local DESIGN.md documents, DTCG token JSON, and
 * CSS/SCSS custom properties into the ordinary project knowledge graph.
 * Authoring and code generation deliberately remain external concerns.
 */
#ifndef CBM_DESIGN_H
#define CBM_DESIGN_H

#include "discover/discover.h"
#include "graph_buffer/graph_buffer.h"

/* Return 0 only when `source_sha256` is the file generation selected by the
 * caller's indexing snapshot. The callback may promote a previously
 * unverified snapshot after this stable read. */
typedef int (*cbm_design_source_verifier_fn)(void *userdata, const char *rel_path,
                                             const char *source_sha256);

typedef struct {
    const char *project_name;
    const char *repo_path;
    cbm_gbuf_t *gbuf;
    const cbm_file_info_t *files;
    int file_count;
    int mode;
    cbm_design_source_verifier_fn verify_source;
    void *verify_source_userdata;
} cbm_design_index_opts_t;

/* Index supported design artifacts into opts->gbuf. Fail-open: malformed
 * individual artifacts are logged and skipped. Returns 0 unless the arguments
 * are invalid or a fatal allocation failure occurs. */
int cbm_design_index(const cbm_design_index_opts_t *opts);

#endif /* CBM_DESIGN_H */
