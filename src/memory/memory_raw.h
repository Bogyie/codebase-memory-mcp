#ifndef CBM_MEMORY_RAW_H
#define CBM_MEMORY_RAW_H

#include <stdbool.h>
#include <stddef.h>

enum { CBM_MEMORY_RAW_MAX_SOURCE_BYTES = 16 * 1024 * 1024 };

/* Result contract for cbm_memory_raw_read_authorized_path().  The caller owns
 * *out_bytes only on CBM_MEMORY_RAW_READ_OK and must release it with free(). */
typedef enum {
    CBM_MEMORY_RAW_READ_OK = 0,
    CBM_MEMORY_RAW_READ_DENIED = -1,
    CBM_MEMORY_RAW_READ_AUTHORITY_REQUIRED = -2,
    CBM_MEMORY_RAW_READ_FAILED = -3,
} cbm_memory_raw_read_result_t;

/* Authorize a local ingest path and read it from the same descriptor/handle
 * that was validated.  The function rejects non-regular files and reparse or
 * symlink escapes, and checks the post-open canonical identity before reading.
 */
cbm_memory_raw_read_result_t cbm_memory_raw_read_authorized_path(const char *path,
                                                                 unsigned char **out_bytes,
                                                                 size_t *out_len);

/* Read a regular file without following its final symlink/reparse point.  The
 * descriptor/handle identity and size are rechecked after the read so a path
 * replacement is rejected.  This is intentionally narrower than a generic
 * read-file helper and is used only for immutable raw-object verification.
 * The caller owns *out_bytes on success. */
int cbm_memory_raw_read_regular_file(const char *path, size_t max_len, unsigned char **out_bytes,
                                     size_t *out_len);

/* Read an immutable raw object and, on Windows, prove from the same open
 * handle that its final path remains below home/raw/objects. */
int cbm_memory_raw_read_regular_object(const char *home, const char *path, size_t max_len,
                                       unsigned char **out_bytes, size_t *out_len);

#ifndef _WIN32
/* Read a single regular-file entry relative to an already-open directory.
 * name must be one component.  The directory descriptor anchors all checks and
 * the read against parent renames or symlink replacement. */
int cbm_memory_raw_read_regular_at(int directory_fd, const char *name, size_t max_len,
                                   unsigned char **out_bytes, size_t *out_len);

/* Acquire a non-blocking shared lease on a plain regular-file entry.  GC takes
 * the conflicting exclusive lease before unlinking an orphan, so callers must
 * retain *out_fd until the database transaction that references the object has
 * committed or rolled back. */
int cbm_memory_raw_lock_regular_at(int directory_fd, const char *name, int *out_fd);
#endif

/* Ensure home/name is a private directory.  An existing final symlink/reparse
 * point is rejected, and its canonical location must remain inside canonical
 * home (home itself may intentionally be a symlink). */
int cbm_memory_raw_ensure_private_subdir(const char *home, const char *name, char *out_path,
                                         size_t out_path_size);

/* Create the same kind of directory but fail rather than join an existing
 * entry.  Used for per-operation staging directories. */
int cbm_memory_raw_create_private_subdir(const char *home, const char *name, char *out_path,
                                         size_t out_path_size);

/* Resolve an existing directory to its final canonical location, including
 * Windows junction/reparse traversal. */
int cbm_memory_raw_resolve_directory(const char *path, char *out_path, size_t out_path_size);

/* Ensure target_parent is a plain directory immediately below canonical
 * home/raw/objects and that target is directly inside target_parent.  The raw
 * root and resulting parent must both canonicalize inside canonical home.
 * *out_created reports whether this call created target_parent.  The canonical
 * target and parent used for subsequent I/O are returned in the output paths. */
int cbm_memory_raw_ensure_object_parent(const char *home, const char *target,
                                        const char *target_parent, bool *out_created,
                                        char *out_target, size_t out_target_size, char *out_parent,
                                        size_t out_parent_size);

/* Validate the same object scope without creating a missing parent. */
int cbm_memory_raw_validate_object_parent(const char *home, const char *target,
                                          const char *target_parent, char *out_target,
                                          size_t out_target_size, char *out_parent,
                                          size_t out_parent_size);

typedef struct cbm_memory_raw_stage cbm_memory_raw_stage_t;

/* Create a private, durable staging object for bytes destined for target.
 * home is the memory home (the staging directory is home/.ingest-staging), and
 * target_parent is target's content-addressed parent directory.  On success,
 * *out_stage is owned by the caller and must be passed exactly once to
 * cbm_memory_raw_stage_dispose(), whether promotion or the database transaction
 * later succeeds or fails.  On failure, *out_stage remains NULL and all
 * partially-created staging state is cleaned up.
 */
int cbm_memory_raw_stage_create(const char *home, const char *target, const char *target_parent,
                                const unsigned char *bytes, size_t len, const char *expected_hash,
                                cbm_memory_raw_stage_t **out_stage);

/* Create staging for repairing a raw object already referenced by the database.
 * Unlike ordinary staging, POSIX permits an existing non-matching final entry;
 * it is still replaced only by cbm_memory_raw_stage_repair_promote(). */
int cbm_memory_raw_stage_create_repair(const char *home, const char *target,
                                       const char *target_parent, const unsigned char *bytes,
                                       size_t len, const char *expected_hash,
                                       cbm_memory_raw_stage_t **out_stage);

/* Install the staged inode at target with a no-replace hard link.  A concurrent
 * identical winner is accepted; a different existing object is rejected.  The
 * stage remains owned by the caller through the database transaction outcome;
 * rollback removes private staging state but retains any installed target
 * rather than risk deleting a concurrent replacement.
 */
int cbm_memory_raw_stage_promote(cbm_memory_raw_stage_t *stage, const unsigned char *bytes,
                                 size_t len, const char *expected_hash);

/* Repair promotion is reserved for a database row whose immutable raw object
 * is already known to be missing or corrupt.  POSIX moves a corrupt final
 * entry aside within its anchored parent before installing the verified stage;
 * ordinary ingest must continue to use the no-replace promotion above. */
int cbm_memory_raw_stage_repair_promote(cbm_memory_raw_stage_t *stage, const unsigned char *bytes,
                                        size_t len, const char *expected_hash);

/* Release and invalidate stage.  Installed content-addressed targets are never
 * deleted here: conditional check-then-unlink is not atomic, so rollback keeps
 * a valid orphan rather than risk deleting a concurrent replacement. */
void cbm_memory_raw_stage_dispose(cbm_memory_raw_stage_t *stage, bool rollback_installed);

typedef bool (*cbm_memory_raw_reference_fn)(const char *object_relpath, void *opaque);

/* Remove stale private staging entries and old unreferenced content-addressed
 * objects.  POSIX traversal is descriptor-relative and never follows entries.
 * grace_seconds must be non-negative; production callers should use a generous
 * grace so another process cannot lose a not-yet-committed promotion.  The
 * current Windows implementation is intentionally non-mutating and reports
 * zero removals until handle-relative deletion and transaction leases are
 * available there. */
int cbm_memory_raw_gc(const char *home, long long grace_seconds,
                      cbm_memory_raw_reference_fn is_referenced, void *opaque,
                      size_t *out_staging_removed, size_t *out_orphans_removed);

#endif /* CBM_MEMORY_RAW_H */
