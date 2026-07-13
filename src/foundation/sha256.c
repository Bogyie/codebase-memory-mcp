/* SHA-256 per FIPS 180-4. Straightforward reference implementation; validated
 * against the NIST test vectors in tests/test_cli.c. */

#include "foundation/sha256.h"
#include "foundation/compat.h"

#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#include "foundation/win_utf8.h"
#else
#include <unistd.h>
#endif

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(cbm_sha256_ctx *c, const uint8_t *data) {
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | (uint32_t)data[j + 3];
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, cc);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}

void cbm_sha256_init(cbm_sha256_ctx *c) {
    c->bitlen = 0;
    c->buflen = 0;
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
}

void cbm_sha256_update(cbm_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = p[i];
        if (c->buflen == 64) {
            sha256_transform(c, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

void cbm_sha256_final(cbm_sha256_ctx *c, uint8_t out[CBM_SHA256_DIGEST_LEN]) {
    c->bitlen += (uint64_t)c->buflen * 8;

    size_t i = c->buflen;
    c->buf[i++] = 0x80; /* append the '1' bit + zero padding */
    if (i > 56) {
        while (i < 64) {
            c->buf[i++] = 0;
        }
        sha256_transform(c, c->buf);
        i = 0;
    }
    while (i < 56) {
        c->buf[i++] = 0;
    }
    /* append the 64-bit big-endian message length */
    for (int j = 0; j < 8; j++) {
        c->buf[56 + j] = (uint8_t)(c->bitlen >> (56 - 8 * j));
    }
    sha256_transform(c, c->buf);

    for (int j = 0; j < 8; j++) {
        out[j * 4] = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(c->state[j]);
    }
}

void cbm_sha256_hex(const void *data, size_t len, char out[CBM_SHA256_HEX_LEN + 1]) {
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_ctx c;
    cbm_sha256_init(&c);
    cbm_sha256_update(&c, data, len);
    cbm_sha256_final(&c, digest);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
}

#ifndef _WIN32
static bool sha256_file_stat_equal(const struct stat *left, const struct stat *right) {
    if (left->st_dev != right->st_dev || left->st_ino != right->st_ino ||
        left->st_size != right->st_size || left->st_mtime != right->st_mtime ||
        left->st_ctime != right->st_ctime) {
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
static int64_t sha256_stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return ((int64_t)st->st_mtimespec.tv_sec * 1000000000LL) + st->st_mtimespec.tv_nsec;
#else
    return ((int64_t)st->st_mtim.tv_sec * 1000000000LL) + st->st_mtim.tv_nsec;
#endif
}
#endif

#ifdef _WIN32
static bool sha256_windows_mtime_ns(const FILETIME *mtime, int64_t *out) {
    const uint64_t windows_to_unix_epoch_100ns = 116444736000000000ULL;
    uint64_t ticks = ((uint64_t)mtime->dwHighDateTime << 32U) | mtime->dwLowDateTime;
    if (ticks < windows_to_unix_epoch_100ns) {
        *out = 0;
        return true;
    }
    uint64_t seconds = (ticks - windows_to_unix_epoch_100ns) / 10000000ULL;
    if (seconds > (uint64_t)INT64_MAX / 1000000000ULL) {
        return false;
    }
    *out = (int64_t)(seconds * 1000000000ULL);
    return true;
}

static bool sha256_windows_info_equal(const BY_HANDLE_FILE_INFORMATION *left,
                                      const BY_HANDLE_FILE_INFORMATION *right) {
    return left->dwVolumeSerialNumber == right->dwVolumeSerialNumber &&
           left->nFileIndexHigh == right->nFileIndexHigh &&
           left->nFileIndexLow == right->nFileIndexLow &&
           left->nFileSizeHigh == right->nFileSizeHigh &&
           left->nFileSizeLow == right->nFileSizeLow &&
           left->ftLastWriteTime.dwHighDateTime == right->ftLastWriteTime.dwHighDateTime &&
           left->ftLastWriteTime.dwLowDateTime == right->ftLastWriteTime.dwLowDateTime;
}

static bool sha256_windows_same_generation(const BY_HANDLE_FILE_INFORMATION *opened,
                                           const char *path) {
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return false;
    }
    HANDLE live = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    BY_HANDLE_FILE_INFORMATION info;
    bool same = live != INVALID_HANDLE_VALUE && GetFileInformationByHandle(live, &info) &&
                sha256_windows_info_equal(opened, &info);
    if (live != INVALID_HANDLE_VALUE) {
        CloseHandle(live);
    }
    return same;
}
#endif

static int sha256_file_process(const char *path, size_t max_bytes, char **out_data, size_t *out_len,
                               char out[CBM_SHA256_HEX_LEN + 1], int64_t *out_mtime_ns,
                               int64_t *out_size) {
    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out) {
        out[0] = '\0';
    }
    if (out_mtime_ns) {
        *out_mtime_ns = 0;
    }
    if (out_size) {
        *out_size = -1;
    }
    if (!path || !path[0] || !out || ((out_data == NULL) != (out_len == NULL)) ||
        (out_data && max_bytes == 0)) {
        return -1;
    }
#ifdef _WIN32
    wchar_t *wide = cbm_utf8_to_wide(path);
    if (!wide) {
        return -1;
    }
    /* Deliberately omit FILE_SHARE_WRITE. NTFS may defer LastWriteTime updates
     * until a writer closes; excluding concurrent writers prevents a same-size
     * in-place write from yielding mixed bytes with unchanged handle metadata. */
    HANDLE opened_handle =
        CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(wide);
    if (opened_handle == INVALID_HANDLE_VALUE || GetFileType(opened_handle) != FILE_TYPE_DISK) {
        if (opened_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(opened_handle);
        }
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION opened_info;
    if (!GetFileInformationByHandle(opened_handle, &opened_info)) {
        CloseHandle(opened_handle);
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)opened_handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(opened_handle);
        return -1;
    }
    FILE *file = _fdopen(fd, "rb");
    if (!file) {
        _close(fd); /* owns and closes opened_handle after _open_osfhandle */
        return -1;
    }
#else
    int flags = O_RDONLY;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOCTTY
    flags |= O_NOCTTY;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return -1;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return -1;
    }
#endif
#ifdef _WIN32
    uint64_t size64 = ((uint64_t)opened_info.nFileSizeHigh << 32U) | opened_info.nFileSizeLow;
    int64_t file_size = size64 <= INT64_MAX ? (int64_t)size64 : -1;
    int64_t file_mtime_ns = 0;
    if (file_size < 0 || !sha256_windows_mtime_ns(&opened_info.ftLastWriteTime, &file_mtime_ns)) {
        (void)fclose(file);
        return -1;
    }
#else
    struct stat before;
    if (fstat(cbm_fileno(file), &before) != 0 || !S_ISREG(before.st_mode)) {
        (void)fclose(file);
        return -1;
    }
    int64_t file_size = (int64_t)before.st_size;
    int64_t file_mtime_ns = sha256_stat_mtime_ns(&before);
#endif
    if (out_mtime_ns) {
        *out_mtime_ns = file_mtime_ns;
    }
    if (out_size) {
        *out_size = file_size;
    }
    bool size_limit_skipped = max_bytes > 0 && file_size > 0 && (uintmax_t)file_size > max_bytes;
    char *owned_data = NULL;
    size_t expected_size = 0;
    if (out_data && !size_limit_skipped) {
        if (file_size < 0 || (uintmax_t)file_size > SIZE_MAX - 1) {
            (void)fclose(file);
            return -1;
        }
        expected_size = (size_t)file_size;
        owned_data = (char *)malloc(expected_size + 1);
        if (!owned_data) {
            (void)fclose(file);
            return CBM_SHA256_FILE_OUT_OF_MEMORY;
        }
    }
#ifdef _WIN32
    intptr_t os_handle = _get_osfhandle(cbm_fileno(file));
#endif
    cbm_sha256_ctx ctx;
    cbm_sha256_init(&ctx);
    unsigned char buffer[64 * 1024];
    size_t count = 0;
    size_t total = 0;
    bool limit_exceeded = false;
    bool size_changed = false;
    while (!size_limit_skipped && (count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (max_bytes > 0 && (count > max_bytes || total > max_bytes - count)) {
            limit_exceeded = true;
            break;
        }
        if (owned_data && (count > expected_size || total > expected_size - count)) {
            size_changed = true;
            break;
        }
        cbm_sha256_update(&ctx, buffer, count);
        if (owned_data) {
            memcpy(owned_data + total, buffer, count);
        }
        total += count;
    }
    bool failed = ferror(file) != 0 || limit_exceeded || size_changed ||
                  (owned_data && total != expected_size);
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION after_info = {0};
    if (!failed && (!GetFileInformationByHandle((HANDLE)os_handle, &after_info) ||
                    !sha256_windows_info_equal(&opened_info, &after_info))) {
        failed = true;
    }
#else
    struct stat after;
    if (fstat(cbm_fileno(file), &after) != 0 || !sha256_file_stat_equal(&before, &after)) {
        failed = true;
    }
#endif
    if (fclose(file) != 0) {
        failed = true;
    }
#ifdef _WIN32
    if (!failed && !sha256_windows_same_generation(&after_info, path)) {
        failed = true;
    }
#else
    /* fstat proves the opened inode was stable. Also ensure the path still
     * names that generation; otherwise an atomic replacement during hashing
     * could make callers compare a digest for an already-unlinked file.
     * Windows performs the equivalent UTF-8-safe handle identity check with
     * CreateFileW above. */
    struct stat live;
    if (!failed && (stat(path, &live) != 0 || !sha256_file_stat_equal(&after, &live))) {
        failed = true;
    }
#endif
    if (failed) {
        free(owned_data);
        return -1;
    }
    if (size_limit_skipped) {
        return CBM_SHA256_FILE_SKIPPED;
    }
    uint8_t digest[CBM_SHA256_DIGEST_LEN];
    cbm_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[CBM_SHA256_HEX_LEN] = '\0';
    if (owned_data) {
        owned_data[total] = '\0';
        *out_data = owned_data;
        *out_len = total;
    }
    return 0;
}

int cbm_sha256_file_hex_limited(const char *path, size_t max_bytes,
                                char out[CBM_SHA256_HEX_LEN + 1]) {
    return sha256_file_process(path, max_bytes, NULL, NULL, out, NULL, NULL) ==
                   CBM_SHA256_FILE_HASHED
               ? 0
               : -1;
}

int cbm_sha256_file_hex(const char *path, char out[CBM_SHA256_HEX_LEN + 1]) {
    return cbm_sha256_file_hex_limited(path, 0, out);
}

int cbm_sha256_file_read_hex(const char *path, size_t max_bytes, char **out_data, size_t *out_len,
                             char out[CBM_SHA256_HEX_LEN + 1]) {
    int rc = sha256_file_process(path, max_bytes, out_data, out_len, out, NULL, NULL);
    if (rc == CBM_SHA256_FILE_HASHED || rc == CBM_SHA256_FILE_OUT_OF_MEMORY) {
        return rc;
    }
    return CBM_SHA256_FILE_ERROR;
}

int cbm_sha256_file_version_hex(const char *path, size_t max_bytes,
                                char out[CBM_SHA256_HEX_LEN + 1], int64_t *out_mtime_ns,
                                int64_t *out_size) {
    if (!out_mtime_ns || !out_size) {
        return CBM_SHA256_FILE_ERROR;
    }
    return sha256_file_process(path, max_bytes, NULL, NULL, out, out_mtime_ns, out_size);
}
