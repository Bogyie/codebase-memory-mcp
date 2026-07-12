/*
 * test_zstd.c — Tests for zstd compression wrappers.
 */
#include "test_framework.h"
#include "zstd_store.h"

#include <limits.h>
#include <stdint.h>

TEST(zstd_roundtrip) {
    const char *data = "Hello, zstd compression roundtrip test!";
    int len = (int)strlen(data);

    size_t bound = cbm_zstd_compress_bound(len);
    ASSERT_GT((int)bound, 0);

    char *cbuf = malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int clen = cbm_zstd_compress(data, len, cbuf, (int)bound, 3);
    ASSERT_GT(clen, 0);

    char *dbuf = malloc(len);
    ASSERT_NOT_NULL(dbuf);

    int64_t dlen = cbm_zstd_decompress(cbuf, (size_t)clen, dbuf, (size_t)len);
    ASSERT_EQ(dlen, len);
    ASSERT_MEM_EQ(dbuf, data, len);

    free(cbuf);
    free(dbuf);
    PASS();
}

TEST(zstd_roundtrip_large) {
    int len = 100000;
    char *data = malloc(len);
    ASSERT_NOT_NULL(data);

    /* Repetitive data — should compress well */
    for (int i = 0; i < len; i++) {
        data[i] = "function_name_pattern_abcdef"[i % 28];
    }

    size_t bound = cbm_zstd_compress_bound(len);
    char *cbuf = malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int clen = cbm_zstd_compress(data, len, cbuf, (int)bound, 9);
    ASSERT_GT(clen, 0);
    /* Repetitive data should compress at least 2:1 */
    ASSERT_LT(clen, len / 2);

    char *dbuf = malloc(len);
    ASSERT_NOT_NULL(dbuf);

    int64_t dlen = cbm_zstd_decompress(cbuf, (size_t)clen, dbuf, (size_t)len);
    ASSERT_EQ(dlen, len);
    ASSERT_MEM_EQ(dbuf, data, len);

    free(data);
    free(cbuf);
    free(dbuf);
    PASS();
}

TEST(zstd_compress_levels) {
    const char *data = "test data for different compression levels";
    int len = (int)strlen(data);
    size_t bound = cbm_zstd_compress_bound(len);
    char *cbuf = malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    /* Both level 3 (fast) and level 9 (best) should produce valid output */
    int clen3 = cbm_zstd_compress(data, len, cbuf, (int)bound, 3);
    ASSERT_GT(clen3, 0);

    int clen9 = cbm_zstd_compress(data, len, cbuf, (int)bound, 9);
    ASSERT_GT(clen9, 0);

    free(cbuf);
    PASS();
}

TEST(zstd_decompress_too_small_output) {
    const char *data = "this is test data that will be compressed";
    int len = (int)strlen(data);
    size_t bound = cbm_zstd_compress_bound(len);
    char *cbuf = malloc(bound);
    ASSERT_NOT_NULL(cbuf);

    int clen = cbm_zstd_compress(data, len, cbuf, (int)bound, 3);
    ASSERT_GT(clen, 0);

    /* Try decompressing with too-small output buffer — should return 0 (error) */
    char small[4];
    int64_t dlen = cbm_zstd_decompress(cbuf, (size_t)clen, small, sizeof(small));
    ASSERT_EQ(dlen, 0);

    free(cbuf);
    PASS();
}

TEST(zstd_bound_positive) {
    ASSERT_GT((int)cbm_zstd_compress_bound(1), 0);
    ASSERT_GT((int)cbm_zstd_compress_bound(100), 0);
    ASSERT_GT((int)cbm_zstd_compress_bound(1000000), 0);
    PASS();
}

TEST(zstd_streaming_roundtrip_and_exact_sizes) {
    enum { DATA_SIZE = 1024 * 1024 + 37 };
    FILE *source = tmpfile();
    FILE *compressed = tmpfile();
    FILE *decoded = tmpfile();
    ASSERT_NOT_NULL(source);
    ASSERT_NOT_NULL(compressed);
    ASSERT_NOT_NULL(decoded);
    for (int i = 0; i < DATA_SIZE; i++) {
        ASSERT_NEQ(fputc((i * 131 + 17) & 0xff, source), EOF);
    }
    rewind(source);
    uint64_t input_size = 0;
    uint64_t compressed_size = 0;
    char compressed_hash[65];
    ASSERT_EQ(cbm_zstd_compress_file(source, compressed, 3, DATA_SIZE, DATA_SIZE + 65536,
                                     &input_size, &compressed_size, compressed_hash),
              0);
    ASSERT_EQ(input_size, DATA_SIZE);
    ASSERT_GT(compressed_size, 0);
    ASSERT_EQ(strlen(compressed_hash), 64);

    rewind(compressed);
    uint64_t consumed = 0;
    uint64_t produced = 0;
    char decoded_hash[65];
    ASSERT_EQ(cbm_zstd_decompress_file(compressed, decoded, compressed_size, DATA_SIZE, &consumed,
                                       &produced, decoded_hash),
              0);
    ASSERT_EQ(consumed, compressed_size);
    ASSERT_EQ(produced, DATA_SIZE);
    ASSERT_STR_EQ(decoded_hash, compressed_hash);
    rewind(source);
    rewind(decoded);
    for (int i = 0; i < DATA_SIZE; i++) {
        ASSERT_EQ(fgetc(decoded), fgetc(source));
    }
    ASSERT_EQ(fgetc(decoded), EOF);
    fclose(source);
    fclose(compressed);
    fclose(decoded);
    PASS();
}

TEST(zstd_streaming_rejects_trailing_and_concatenated_frames) {
    static const char payload[] = "one exact frame";
    FILE *source = tmpfile();
    FILE *frame = tmpfile();
    ASSERT_NOT_NULL(source);
    ASSERT_NOT_NULL(frame);
    ASSERT_EQ(fwrite(payload, 1, sizeof(payload), source), sizeof(payload));
    rewind(source);
    uint64_t in_size = 0;
    uint64_t frame_size = 0;
    char hash[65];
    ASSERT_EQ(cbm_zstd_compress_file(source, frame, 3, 1024, 1024, &in_size, &frame_size, hash), 0);

    ASSERT_EQ(fputc('X', frame), 'X');
    ASSERT_EQ(fflush(frame), 0);
    rewind(frame);
    FILE *decoded = tmpfile();
    uint64_t consumed = 0;
    uint64_t produced = 0;
    char actual[65];
    ASSERT_NEQ(cbm_zstd_decompress_file(frame, decoded, 2048, 2048, &consumed, &produced, actual),
               0);
    fclose(decoded);

    FILE *joined = tmpfile();
    ASSERT_NOT_NULL(joined);
    rewind(frame);
    /* Copy only the valid frame, twice. */
    for (uint64_t copy = 0; copy < 2; copy++) {
        rewind(frame);
        for (uint64_t i = 0; i < frame_size; i++) {
            int byte = fgetc(frame);
            ASSERT_NEQ(byte, EOF);
            ASSERT_EQ(fputc(byte, joined), byte);
        }
    }
    rewind(joined);
    decoded = tmpfile();
    ASSERT_NEQ(cbm_zstd_decompress_file(joined, decoded, frame_size * 2U, 2048, &consumed,
                                        &produced, actual),
               0);
    fclose(source);
    fclose(frame);
    fclose(joined);
    fclose(decoded);
    PASS();
}

TEST(zstd_streaming_enforces_input_output_and_decode_caps) {
    FILE *source = tmpfile();
    FILE *compressed = tmpfile();
    ASSERT_NOT_NULL(source);
    ASSERT_NOT_NULL(compressed);
    for (int i = 0; i < 4096; i++) {
        ASSERT_NEQ(fputc(i & 0xff, source), EOF);
    }
    rewind(source);
    uint64_t in_size = 0;
    uint64_t compressed_size = 0;
    char hash[65];
    ASSERT_NEQ(
        cbm_zstd_compress_file(source, compressed, 3, 4095, 8192, &in_size, &compressed_size, hash),
        0);

    rewind(source);
    fclose(compressed);
    compressed = tmpfile();
    ASSERT_NEQ(
        cbm_zstd_compress_file(source, compressed, 3, 4096, 1, &in_size, &compressed_size, hash),
        0);

    rewind(source);
    fclose(compressed);
    compressed = tmpfile();
    ASSERT_EQ(
        cbm_zstd_compress_file(source, compressed, 3, 4096, 8192, &in_size, &compressed_size, hash),
        0);
    FILE *decoded = tmpfile();
    uint64_t consumed = 0;
    uint64_t produced = 0;
    char actual[65];
    rewind(compressed);
    ASSERT_NEQ(cbm_zstd_decompress_file(compressed, decoded, compressed_size - 1U, 4096, &consumed,
                                        &produced, actual),
               0);
    fclose(decoded);
    decoded = tmpfile();
    rewind(compressed);
    ASSERT_NEQ(cbm_zstd_decompress_file(compressed, decoded, compressed_size, 4095, &consumed,
                                        &produced, actual),
               0);
    fclose(source);
    fclose(compressed);
    fclose(decoded);
    PASS();
}

TEST(zstd_size_safe_bound_exceeds_int_range_without_allocation) {
    ASSERT_EQ(cbm_zstd_compress_bound(-1), 0);
#if SIZE_MAX > INT_MAX
    size_t large = (size_t)INT_MAX + 1U;
    ASSERT_GT(cbm_zstd_compress_bound_size(large), large);
#endif
    PASS();
}

SUITE(zstd) {
    RUN_TEST(zstd_roundtrip);
    RUN_TEST(zstd_roundtrip_large);
    RUN_TEST(zstd_compress_levels);
    RUN_TEST(zstd_decompress_too_small_output);
    RUN_TEST(zstd_bound_positive);
    RUN_TEST(zstd_streaming_roundtrip_and_exact_sizes);
    RUN_TEST(zstd_streaming_rejects_trailing_and_concatenated_frames);
    RUN_TEST(zstd_streaming_enforces_input_output_and_decode_caps);
    RUN_TEST(zstd_size_safe_bound_exceeds_int_range_without_allocation);
}
