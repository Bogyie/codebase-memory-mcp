#ifndef CBM_SHA256_H
#define CBM_SHA256_H

/* In-process SHA-256 (FIPS 180-4). Used to verify the integrity of a
 * downloaded release before installing it, without shelling out to a
 * platform hashing tool (shasum / sha256sum / certutil) — those differ per
 * OS, may be absent, and mis-quote paths under cmd.exe. */

#include <stddef.h>
#include <stdint.h>

#define CBM_SHA256_DIGEST_LEN 32 /* raw digest bytes */
#define CBM_SHA256_HEX_LEN 64    /* lowercase hex chars (no NUL) */

enum {
    CBM_SHA256_FILE_OUT_OF_MEMORY = -2,
    CBM_SHA256_FILE_ERROR = -1,
    CBM_SHA256_FILE_HASHED = 0,
    CBM_SHA256_FILE_SKIPPED = 1,
};

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} cbm_sha256_ctx;

void cbm_sha256_init(cbm_sha256_ctx *c);
void cbm_sha256_update(cbm_sha256_ctx *c, const void *data, size_t len);
void cbm_sha256_final(cbm_sha256_ctx *c, uint8_t out[CBM_SHA256_DIGEST_LEN]);

/* One-shot hash of a buffer to lowercase hex. `out` must hold
 * CBM_SHA256_HEX_LEN + 1 bytes (hex chars + NUL). */
void cbm_sha256_hex(const void *data, size_t len, char out[CBM_SHA256_HEX_LEN + 1]);

/* Stream a stable file generation into SHA-256 without loading it wholly into
 * memory. Returns 0 on success and -1 for invalid arguments, I/O failures, or
 * a file that is modified/replaced while it is being hashed. */
int cbm_sha256_file_hex(const char *path, char out[CBM_SHA256_HEX_LEN + 1]);

/* Same stable-generation contract, but aborts after reading more than
 * max_bytes. A max_bytes value of 0 means unlimited. */
int cbm_sha256_file_hex_limited(const char *path, size_t max_bytes,
                                char out[CBM_SHA256_HEX_LEN + 1]);

/* Read and hash one stable regular-file generation from the same descriptor.
 * max_bytes must be positive. On success, *out_data is a malloc-owned,
 * NUL-terminated byte buffer (*out_len excludes the terminator) and `out`
 * contains its SHA-256. Returns CBM_SHA256_FILE_OUT_OF_MEMORY specifically
 * when the owned source buffer cannot be allocated, and -1 for other errors. */
int cbm_sha256_file_read_hex(const char *path, size_t max_bytes, char **out_data, size_t *out_len,
                             char out[CBM_SHA256_HEX_LEN + 1]);

/* Capture UTF-8-path metadata and, when size <= max_bytes, a stable SHA-256 in
 * one regular-file open. Returns HASHED, SKIPPED (oversized, empty hash), or
 * ERROR. Metadata is populated once fstat succeeds, including on later read /
 * generation-validation errors. */
int cbm_sha256_file_version_hex(const char *path, size_t max_bytes,
                                char out[CBM_SHA256_HEX_LEN + 1], int64_t *out_mtime_ns,
                                int64_t *out_size);

#endif /* CBM_SHA256_H */
