// zstd_store.c — Thin C wrappers around Zstandard.

#include "vendored/zstd/zstd.h"

#include "zstd_store.h"
#include "foundation/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int cbm_zstd_compress(const char *src, int srcLen, char *dst, int dstCap, int level) {
    if (!src || !dst || srcLen < 0 || dstCap <= 0) {
        return 0;
    }
    size_t rc = ZSTD_compress(dst, (size_t)dstCap, src, (size_t)srcLen, level);
    if (ZSTD_isError(rc) || rc > INT_MAX) {
        return 0;
    }
    return (int)rc;
}

int64_t cbm_zstd_decompress(const char *src, size_t srcLen, char *dst, size_t dstCap) {
    if (!src || !dst || srcLen == 0 || dstCap == 0) {
        return 0;
    }
    size_t rc = ZSTD_decompress(dst, dstCap, src, srcLen);
    if (ZSTD_isError(rc) || rc > INT64_MAX) {
        return 0;
    }
    return (int64_t)rc;
}

size_t cbm_zstd_frame_content_size(const char *src, size_t srcLen) {
    unsigned long long n = ZSTD_getFrameContentSize(src, srcLen);
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR) {
        return 0;
    }
    if (n > SIZE_MAX) {
        return 0;
    }
    return (size_t)n;
}

size_t cbm_zstd_compress_bound(int inputSize) {
    return inputSize < 0 ? 0 : cbm_zstd_compress_bound_size((size_t)inputSize);
}

size_t cbm_zstd_compress_bound_size(size_t input_size) {
    size_t bound = ZSTD_compressBound(input_size);
    return ZSTD_isError(bound) ? 0 : bound;
}

static bool zstd_add_with_cap(uint64_t *total, size_t amount, uint64_t cap) {
    if ((uint64_t)amount > UINT64_MAX - *total) {
        return false;
    }
    uint64_t next = *total + (uint64_t)amount;
    if (cap > 0 && next > cap) {
        return false;
    }
    *total = next;
    return true;
}

static bool zstd_write_all(FILE *dst, const void *data, size_t len, uint64_t *total, uint64_t cap) {
    if (!zstd_add_with_cap(total, len, cap)) {
        return false;
    }
    return len == 0 || fwrite(data, 1, len, dst) == len;
}

int cbm_zstd_compress_file(FILE *src, FILE *dst, int level, uint64_t max_input, uint64_t max_output,
                           uint64_t *out_input_size, uint64_t *out_compressed_size,
                           char out_compressed_sha256[65]) {
    if (out_input_size) {
        *out_input_size = 0;
    }
    if (out_compressed_size) {
        *out_compressed_size = 0;
    }
    if (out_compressed_sha256) {
        out_compressed_sha256[0] = '\0';
    }
    if (!src || !dst || !out_input_size || !out_compressed_size || !out_compressed_sha256) {
        return -1;
    }
    size_t input_cap = ZSTD_CStreamInSize();
    size_t output_cap = ZSTD_CStreamOutSize();
    unsigned char *input_data = input_cap ? malloc(input_cap) : NULL;
    unsigned char *output_data = output_cap ? malloc(output_cap) : NULL;
    ZSTD_CStream *stream = ZSTD_createCStream();
    if (!input_data || !output_data || !stream || ZSTD_isError(ZSTD_initCStream(stream, level))) {
        free(input_data);
        free(output_data);
        ZSTD_freeCStream(stream);
        return -1;
    }

    uint64_t input_total = 0;
    uint64_t output_total = 0;
    cbm_sha256_ctx compressed_hash;
    cbm_sha256_init(&compressed_hash);
    int result = -1;
    bool finished = false;
    while (!finished) {
        size_t count = fread(input_data, 1, input_cap, src);
        if (!zstd_add_with_cap(&input_total, count, max_input) || ferror(src)) {
            break;
        }
        bool eof = count < input_cap;
        if (eof && !feof(src)) {
            break;
        }
        ZSTD_inBuffer input = {input_data, count, 0};
        ZSTD_EndDirective directive = eof ? ZSTD_e_end : ZSTD_e_continue;
        size_t remaining = 1;
        while (input.pos < input.size || (eof && remaining != 0)) {
            ZSTD_outBuffer output = {output_data, output_cap, 0};
            remaining = ZSTD_compressStream2(stream, &output, &input, directive);
            if (ZSTD_isError(remaining) ||
                !zstd_write_all(dst, output_data, output.pos, &output_total, max_output)) {
                goto done;
            }
            cbm_sha256_update(&compressed_hash, output_data, output.pos);
        }
        if (eof) {
            finished = true;
        }
    }
    if (finished && fflush(dst) == 0) {
        *out_input_size = input_total;
        *out_compressed_size = output_total;
        uint8_t digest[CBM_SHA256_DIGEST_LEN];
        cbm_sha256_final(&compressed_hash, digest);
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
            out_compressed_sha256[i * 2] = hex[digest[i] >> 4U];
            out_compressed_sha256[i * 2 + 1] = hex[digest[i] & 0x0fU];
        }
        out_compressed_sha256[64] = '\0';
        result = 0;
    }

done:
    ZSTD_freeCStream(stream);
    free(input_data);
    free(output_data);
    return result;
}

int cbm_zstd_decompress_file(FILE *src, FILE *dst, uint64_t max_compressed,
                             uint64_t max_decompressed, uint64_t *out_compressed_size,
                             uint64_t *out_decompressed_size, char out_compressed_sha256[65]) {
    if (out_compressed_size) {
        *out_compressed_size = 0;
    }
    if (out_decompressed_size) {
        *out_decompressed_size = 0;
    }
    if (out_compressed_sha256) {
        out_compressed_sha256[0] = '\0';
    }
    if (!src || !dst || !out_compressed_size || !out_decompressed_size || !out_compressed_sha256) {
        return -1;
    }
    size_t input_cap = ZSTD_DStreamInSize();
    size_t output_cap = ZSTD_DStreamOutSize();
    unsigned char *input_data = input_cap ? malloc(input_cap) : NULL;
    unsigned char *output_data = output_cap ? malloc(output_cap) : NULL;
    ZSTD_DStream *stream = ZSTD_createDStream();
    if (!input_data || !output_data || !stream || ZSTD_isError(ZSTD_initDStream(stream))) {
        free(input_data);
        free(output_data);
        ZSTD_freeDStream(stream);
        return -1;
    }

    uint64_t input_total = 0;
    uint64_t output_total = 0;
    cbm_sha256_ctx compressed_hash;
    cbm_sha256_init(&compressed_hash);
    int result = -1;
    bool frame_done = false;
    while (!frame_done) {
        size_t count = fread(input_data, 1, input_cap, src);
        if (!zstd_add_with_cap(&input_total, count, max_compressed) || ferror(src) || count == 0) {
            break;
        }
        cbm_sha256_update(&compressed_hash, input_data, count);
        ZSTD_inBuffer input = {input_data, count, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output = {output_data, output_cap, 0};
            size_t remaining = ZSTD_decompressStream(stream, &output, &input);
            if (ZSTD_isError(remaining) ||
                !zstd_write_all(dst, output_data, output.pos, &output_total, max_decompressed)) {
                goto decompress_done;
            }
            if (remaining == 0) {
                if (input.pos != input.size) {
                    goto decompress_done; /* trailing bytes or concatenated frame */
                }
                int trailing = fgetc(src);
                if (trailing != EOF || ferror(src)) {
                    goto decompress_done;
                }
                frame_done = true;
                break;
            }
        }
    }
    if (frame_done && fflush(dst) == 0) {
        *out_compressed_size = input_total;
        *out_decompressed_size = output_total;
        uint8_t digest[CBM_SHA256_DIGEST_LEN];
        cbm_sha256_final(&compressed_hash, digest);
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < CBM_SHA256_DIGEST_LEN; i++) {
            out_compressed_sha256[i * 2] = hex[digest[i] >> 4U];
            out_compressed_sha256[i * 2 + 1] = hex[digest[i] & 0x0fU];
        }
        out_compressed_sha256[64] = '\0';
        result = 0;
    }

decompress_done:
    ZSTD_freeDStream(stream);
    free(input_data);
    free(output_data);
    return result;
}
