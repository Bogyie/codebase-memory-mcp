/* Security and generation-coherence tests for foundation/rooted_file. */

#include "test_framework.h"
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/rooted_file.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

TEST(rooted_file_reads_empty_and_bounds_regular_files) {
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_rooted_basic_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(root)) {
        FAIL("cbm_mkdtemp failed");
    }
    char empty_path[700];
    char small_path[700];
    snprintf(empty_path, sizeof(empty_path), "%s/empty.c", root);
    snprintf(small_path, sizeof(small_path), "%s/small.c", root);
    FILE *fp = fopen(empty_path, "wb");
    ASSERT_NOT_NULL(fp);
    ASSERT_EQ(fclose(fp), 0);
    fp = fopen(small_path, "wb");
    ASSERT_NOT_NULL(fp);
    ASSERT_EQ(fwrite("123456789", 1, 9, fp), 9);
    ASSERT_EQ(fclose(fp), 0);

    cbm_rooted_file_t file = {0};
    ASSERT_EQ(cbm_rooted_file_read(root, "empty.c", 16, &file), CBM_ROOTED_FILE_OK);
    ASSERT_TRUE(file.metadata_valid);
    ASSERT_EQ(file.size, 0);
    ASSERT_EQ(file.len, 0);
    ASSERT_NOT_NULL(file.data);
    ASSERT_STR_EQ(file.data, "");
    char empty_hash[CBM_SHA256_HEX_LEN + 1];
    cbm_sha256_hex("", 0, empty_hash);
    ASSERT_STR_EQ(file.sha256, empty_hash);
    cbm_rooted_file_free(&file);

    ASSERT_EQ(cbm_rooted_file_read(root, "small.c", 8, &file), CBM_ROOTED_FILE_TOO_LARGE);
    ASSERT_TRUE(file.metadata_valid);
    ASSERT_EQ(file.size, 9);
    ASSERT_NULL(file.data);
    cbm_rooted_file_free(&file);

    ASSERT_EQ(cbm_unlink(empty_path), 0);
    ASSERT_EQ(cbm_unlink(small_path), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    PASS();
}

TEST(rooted_file_rejects_nonrelative_paths) {
    ASSERT_FALSE(cbm_rooted_relative_path_valid(NULL));
    ASSERT_FALSE(cbm_rooted_relative_path_valid(""));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("/etc/hosts"));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("."));
    ASSERT_FALSE(cbm_rooted_relative_path_valid(".."));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("a/../b.c"));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("a/./b.c"));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("a//b.c"));
    ASSERT_FALSE(cbm_rooted_relative_path_valid("a/"));
    ASSERT_TRUE(cbm_rooted_relative_path_valid("a/b.c"));

    cbm_rooted_file_t file = {0};
    ASSERT_EQ(cbm_rooted_file_read("/", "../etc/hosts", 1024, &file), CBM_ROOTED_FILE_INVALID);
    ASSERT_NULL(file.data);
    ASSERT_EQ(cbm_rooted_file_read("/", "/etc/hosts", 1024, &file), CBM_ROOTED_FILE_INVALID);
    ASSERT_NULL(file.data);
    PASS();
}

#ifndef _WIN32
typedef struct {
    const char *inside;
    const char *backup;
    const char *outside;
    int rename_result;
    int symlink_result;
} rooted_swap_context_t;

static void rooted_swap_to_outside_symlink(void *opaque) {
    rooted_swap_context_t *context = (rooted_swap_context_t *)opaque;
    context->rename_result = rename(context->inside, context->backup);
    context->symlink_result =
        context->rename_result == 0 ? symlink(context->outside, context->inside) : -1;
}
#endif

TEST(rooted_file_rejects_symlinks_fifo_and_swap_aba) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX openat/O_NOFOLLOW regression; Windows HANDLE containment is separate");
#else
    char root[512];
    snprintf(root, sizeof(root), "%s/cbm_rooted_security_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(root)) {
        FAIL("cbm_mkdtemp failed");
    }
    char outside_dir[512];
    snprintf(outside_dir, sizeof(outside_dir), "%s_outside", root);
    ASSERT_EQ(cbm_mkdir(outside_dir), 0);
    char outside[700];
    snprintf(outside, sizeof(outside), "%s/secret.c", outside_dir);
    FILE *fp = fopen(outside, "wb");
    ASSERT_NOT_NULL(fp);
    ASSERT_TRUE(fputs("OUTSIDE_SECRET\n", fp) >= 0);
    ASSERT_EQ(fclose(fp), 0);

    char final_link[700];
    char intermediate_link[700];
    char fifo_path[700];
    char victim[700];
    char backup[700];
    snprintf(final_link, sizeof(final_link), "%s/final.c", root);
    snprintf(intermediate_link, sizeof(intermediate_link), "%s/via", root);
    snprintf(fifo_path, sizeof(fifo_path), "%s/source.fifo", root);
    snprintf(victim, sizeof(victim), "%s/victim.c", root);
    snprintf(backup, sizeof(backup), "%s/victim.original", root);
    ASSERT_EQ(symlink(outside, final_link), 0);
    ASSERT_EQ(symlink(outside_dir, intermediate_link), 0);
    ASSERT_EQ(mkfifo(fifo_path, 0600), 0);
    fp = fopen(victim, "wb");
    ASSERT_NOT_NULL(fp);
    ASSERT_TRUE(fputs("INSIDE_SOURCE\n", fp) >= 0);
    ASSERT_EQ(fclose(fp), 0);

    cbm_rooted_file_t file = {0};
    ASSERT_EQ(cbm_rooted_file_read(root, "final.c", 1024, &file), CBM_ROOTED_FILE_UNAVAILABLE);
    ASSERT_NULL(file.data);
    ASSERT_EQ(cbm_rooted_file_read(root, "via/secret.c", 1024, &file), CBM_ROOTED_FILE_UNAVAILABLE);
    ASSERT_NULL(file.data);
    ASSERT_EQ(cbm_rooted_file_read(root, "source.fifo", 1024, &file), CBM_ROOTED_FILE_UNAVAILABLE);
    ASSERT_NULL(file.data);

    rooted_swap_context_t swap = {.inside = victim,
                                  .backup = backup,
                                  .outside = outside,
                                  .rename_result = -1,
                                  .symlink_result = -1};
    cbm_rooted_file_set_test_hook(rooted_swap_to_outside_symlink, &swap);
    cbm_rooted_file_status_t status = cbm_rooted_file_read(root, "victim.c", 1024, &file);
    cbm_rooted_file_set_test_hook(NULL, NULL);
    ASSERT_EQ(swap.rename_result, 0);
    ASSERT_EQ(swap.symlink_result, 0);
    ASSERT_EQ(status, CBM_ROOTED_FILE_CHANGED);
    ASSERT_NULL(file.data);
    ASSERT_TRUE(file.metadata_valid);

    ASSERT_EQ(cbm_unlink(final_link), 0);
    ASSERT_EQ(cbm_unlink(intermediate_link), 0);
    ASSERT_EQ(cbm_unlink(fifo_path), 0);
    ASSERT_EQ(cbm_unlink(victim), 0);
    ASSERT_EQ(cbm_unlink(backup), 0);
    ASSERT_EQ(cbm_unlink(outside), 0);
    ASSERT_EQ(cbm_rmdir(outside_dir), 0);
    ASSERT_EQ(cbm_rmdir(root), 0);
    PASS();
#endif
}

SUITE(rooted_file) {
    RUN_TEST(rooted_file_reads_empty_and_bounds_regular_files);
    RUN_TEST(rooted_file_rejects_nonrelative_paths);
    RUN_TEST(rooted_file_rejects_symlinks_fifo_and_swap_aba);
}
