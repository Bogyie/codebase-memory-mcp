/*
 * config.c — Persistent UI configuration (JSON via yyjson).
 *
 * Config file: ~/.cache/codebase-memory-mcp/config.json
 * Format: {"ui_enabled": false, "ui_port": 9749}
 */
#include "foundation/constants.h"
#include "ui/config.h"
#include "ui/embedded_assets.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/compat_fs.h"
#include "foundation/compat.h"

#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define ui_config_close_fd _close
#define ui_config_fdopen _fdopen
#define ui_config_sync_fd _commit
#else
#include <unistd.h>
#define ui_config_close_fd close
#define ui_config_fdopen fdopen
#define ui_config_sync_fd fsync
#endif

/* ── Path ────────────────────────────────────────────────────── */

bool cbm_ui_config_path(char *buf, int bufsz) {
    if (!buf || bufsz <= 0)
        return false;
    buf[0] = '\0';
    const char *dir = cbm_resolve_cache_dir();
    if (!dir)
        return false;
    int n = snprintf(buf, (size_t)bufsz, "%s/config.json", dir);
    if (n <= 0 || n >= bufsz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* ── Load ────────────────────────────────────────────────────── */

void cbm_ui_config_load(cbm_ui_config_t *cfg) {
    if (!cfg)
        return;
    cfg->ui_enabled = CBM_UI_DEFAULT_ENABLED;
    cfg->ui_port = CBM_UI_DEFAULT_PORT;

    char path[CBM_SZ_1K];
    if (!cbm_ui_config_path(path, (int)sizeof(path))) {
        if (CBM_EMBEDDED_FILE_COUNT > 0)
            cfg->ui_enabled = true;
        return;
    }

    FILE *f = cbm_fopen(path, "rb");
    if (!f) {
        /* No config file — auto-enable UI if binary has embedded assets */
        if (CBM_EMBEDDED_FILE_COUNT > 0) {
            cfg->ui_enabled = true;
        }
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 4096) {
        fclose(f);
        return; /* empty or suspiciously large → defaults */
    }

    char *buf = malloc((size_t)len + SKIP_ONE);
    if (!buf) {
        fclose(f);
        return;
    }

    size_t nread = fread(buf, SKIP_ONE, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';

    yyjson_doc *doc = yyjson_read(buf, nread, 0);
    free(buf);
    if (!doc) {
        cbm_log_warn("ui.config.corrupt", "path", path);
        return; /* corrupt JSON → defaults */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return;
    }

    yyjson_val *v_enabled = yyjson_obj_get(root, "ui_enabled");
    if (yyjson_is_bool(v_enabled)) {
        cfg->ui_enabled = yyjson_get_bool(v_enabled);
    }

    yyjson_val *v_port = yyjson_obj_get(root, "ui_port");
    if (yyjson_is_int(v_port)) {
        int64_t port = yyjson_get_sint(v_port);
        if (port > 0 && port <= 65535)
            cfg->ui_port = (int)port;
    }

    yyjson_doc_free(doc);
}

/* ── Save ────────────────────────────────────────────────────── */

void cbm_ui_config_save(const cbm_ui_config_t *cfg) {
    if (!cfg)
        return;
    char path[CBM_SZ_1K];
    if (!cbm_ui_config_path(path, (int)sizeof(path))) {
        cbm_log_error("ui.config.write_fail", "reason", "cache_dir_unavailable");
        return;
    }

    /* Ensure directory exists (recursive) */
    char dir[CBM_SZ_1K];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!cbm_mkdir_p(dir, 0750)) {
            cbm_log_error("ui.config.write_fail", "reason", "cache_dir_create", "path", dir);
            return;
        }
    } else {
        cbm_log_error("ui.config.write_fail", "reason", "invalid_path");
        return;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        if (doc)
            yyjson_mut_doc_free(doc);
        cbm_log_error("ui.config.write_fail", "reason", "serialize_oom");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);

    if (!yyjson_mut_obj_add_bool(doc, root, "ui_enabled", cfg->ui_enabled) ||
        !yyjson_mut_obj_add_int(doc, root, "ui_port", cfg->ui_port)) {
        yyjson_mut_doc_free(doc);
        cbm_log_error("ui.config.write_fail", "reason", "serialize_oom");
        return;
    }

    size_t json_len = 0;
    char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &json_len);
    yyjson_mut_doc_free(doc);

    if (!json) {
        cbm_log_error("ui.config.write_fail", "reason", "serialize");
        return;
    }

    char temp_path[CBM_SZ_1K + 32];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path);
    int fd = temp_len > 0 && (size_t)temp_len < sizeof(temp_path) ? cbm_mkstemp(temp_path) : -1;
    if (fd < 0) {
        cbm_log_error("ui.config.write_fail", "reason", "temp_create", "path", path);
        free(json);
        return;
    }
    FILE *f = ui_config_fdopen(fd, "wb");
    if (!f) {
        (void)ui_config_close_fd(fd);
        (void)cbm_unlink(temp_path);
        cbm_log_error("ui.config.write_fail", "reason", "temp_open", "path", path);
        free(json);
        return;
    }

    bool wrote = fwrite(json, SKIP_ONE, json_len, f) == json_len && fflush(f) == 0 &&
                 ui_config_sync_fd(cbm_fileno(f)) == 0;
    if (fclose(f) != 0)
        wrote = false;
    free(json);
    if (!wrote || cbm_rename_replace(temp_path, path) != 0) {
        (void)cbm_unlink(temp_path);
        cbm_log_error("ui.config.write_fail", "reason", "atomic_replace", "path", path);
        return;
    }

    cbm_log_debug("ui.config.saved", "path", path);
}
