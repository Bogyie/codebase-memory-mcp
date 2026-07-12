/*
 * http_server.c — Routing + endpoint handlers for the graph UI.
 *
 * Transport (sockets, parsing, limits) lives in httpd.c; this file owns
 * the routes and their handlers:
 *   GET /             → embedded index.html
 *   GET /assets/...   → embedded JS/CSS
 *   POST /rpc         → JSON-RPC dispatch via own cbm_mcp_server_t
 *   OPTIONS /rpc      → same-origin CORS preflight
 *   GET/POST /api/... → UI support endpoints (layout, index, browse, …)
 *   *                 → 404
 *
 * Runs in a background pthread. Binds to 127.0.0.1 only (see httpd.c).
 * Has its own cbm_mcp_server_t with a separate SQLite connection (WAL reader).
 */
#include "ui/http_server.h"
#include "ui/httpd.h"
#include "ui/embedded_assets.h"
#include "ui/layout3d.h"
#include "mcp/mcp.h"
#include "store/store.h"
#include "watcher/watcher.h"
#include "cli/cli.h"
#include "git/git_context.h"

#if defined(HAVE_LIBGIT2)
#include <git2.h> /* git_repository_open, git_remote_lookup, git_remote_url */
#endif
/* pipeline.h no longer needed — indexing runs as subprocess */
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/product.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/str_util.h"
#include "foundation/compat_thread.h"
#include "foundation/subprocess.h"
#include "foundation/win_utf8.h"

#include <sqlite3/sqlite3.h>
#include <yyjson/yyjson.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <psapi.h> /* GetProcessMemoryInfo */
#define ui_close_fd _close
#else
#include <sys/stat.h>
#include <unistd.h>
#define ui_close_fd close
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ── Constants ────────────────────────────────────────────────── */

/* Max JSON-RPC request body size (1 MB) — transport enforces the same cap. */
#define MAX_BODY_SIZE CBM_HTTP_MAX_BODY

/* ── Browser-origin and Host guards ───────────────────────────────────────── */

/* Per-request CORS header buffers. Updated at the start of each dispatch.
 * The server handles requests sequentially on one thread (see httpd.h),
 * which makes these statics safe. */
static char g_cors[256];      /* CORS headers only */
static char g_cors_json[512]; /* CORS + Content-Type: application/json */

static bool ascii_equal_nocase_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (ac >= 'A' && ac <= 'Z')
            ac = (unsigned char)(ac - 'A' + 'a');
        if (bc >= 'A' && bc <= 'Z')
            bc = (unsigned char)(bc - 'A' + 'a');
        if (ac != bc)
            return false;
    }
    return true;
}

static bool valid_port_suffix(const char *s) {
    if (!s || *s != ':')
        return false;
    s++;
    if (*s == '\0')
        return false;

    unsigned port = 0;
    size_t digits = 0;
    while (*s) {
        if (*s < '0' || *s > '9' || digits == 5)
            return false;
        port = port * 10u + (unsigned)(*s - '0');
        digits++;
        s++;
    }
    return port > 0 && port <= 65535u;
}

/* Accept only a syntactically complete loopback authority, not a prefix such
 * as "localhost:9749.evil". The latter used to pass the wildcard matcher. */
static bool authority_is_loopback(const char *authority) {
    static const char localhost[] = "localhost";
    size_t n = strlen(authority);
    size_t host_len = sizeof(localhost) - 1;
    if (n >= host_len && ascii_equal_nocase_n(authority, localhost, host_len) &&
        (n == host_len || valid_port_suffix(authority + host_len))) {
        return true;
    }

    static const char ipv4[] = "127.0.0.1";
    host_len = sizeof(ipv4) - 1;
    if (n >= host_len && memcmp(authority, ipv4, host_len) == 0 &&
        (n == host_len || valid_port_suffix(authority + host_len))) {
        return true;
    }

    static const char ipv6[] = "[::1]";
    host_len = sizeof(ipv6) - 1;
    return n >= host_len && memcmp(authority, ipv6, host_len) == 0 &&
           (n == host_len || valid_port_suffix(authority + host_len));
}

typedef struct {
    char host[16];
    unsigned port;
    bool has_port;
} loopback_authority_t;

static bool parse_port_value(const char *suffix, unsigned *port) {
    if (!valid_port_suffix(suffix))
        return false;
    unsigned value = 0;
    for (const char *p = suffix + 1; *p; p++)
        value = value * 10U + (unsigned)(*p - '0');
    *port = value;
    return true;
}

static bool parse_loopback_authority(const char *authority, loopback_authority_t *out) {
    if (!authority || !out || !authority_is_loopback(authority))
        return false;
    memset(out, 0, sizeof(*out));
    const char *suffix = NULL;
    if (ascii_equal_nocase_n(authority, "localhost", sizeof("localhost") - 1U)) {
        memcpy(out->host, "localhost", sizeof("localhost"));
        suffix = authority + sizeof("localhost") - 1U;
    } else if (strncmp(authority, "127.0.0.1", sizeof("127.0.0.1") - 1U) == 0) {
        memcpy(out->host, "127.0.0.1", sizeof("127.0.0.1"));
        suffix = authority + sizeof("127.0.0.1") - 1U;
    } else {
        memcpy(out->host, "[::1]", sizeof("[::1]"));
        suffix = authority + sizeof("[::1]") - 1U;
    }
    if (*suffix == '\0')
        return true;
    out->has_port = true;
    return parse_port_value(suffix, &out->port);
}

static bool host_matches_listener(const char *host, int server_port) {
    loopback_authority_t parsed;
    return parse_loopback_authority(host, &parsed) &&
           (!parsed.has_port || parsed.port == (unsigned)server_port);
}

static bool origin_is_same_server(const char *origin, const char *host, int server_port) {
    static const char scheme[] = "http://";
    size_t scheme_len = sizeof(scheme) - 1U;
    if (!origin || !host || strlen(origin) <= scheme_len ||
        !ascii_equal_nocase_n(origin, scheme, scheme_len)) {
        return false;
    }
    loopback_authority_t origin_auth;
    loopback_authority_t host_auth;
    if (!parse_loopback_authority(origin + scheme_len, &origin_auth) ||
        !parse_loopback_authority(host, &host_auth) ||
        !ascii_equal_nocase_n(origin_auth.host, host_auth.host, strlen(host_auth.host)) ||
        strlen(origin_auth.host) != strlen(host_auth.host)) {
        return false;
    }
    unsigned origin_port = origin_auth.has_port ? origin_auth.port : 80U;
    unsigned host_port = host_auth.has_port ? host_auth.port : (unsigned)server_port;
    return origin_port == (unsigned)server_port && host_port == (unsigned)server_port;
}

static bool http_token_char(unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
           ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
           ch == '`' || ch == '|' || ch == '~';
}

static const char *skip_http_ows(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Validate media-type parameters rather than accepting arbitrary bytes after
 * the first semicolon. This keeps the CSRF media-type gate syntactically strict
 * while allowing conventional values such as `charset=utf-8`. */
static bool json_content_type_parameters_valid(const char *p) {
    while (*p) {
        if (*p++ != ';')
            return false;
        p = skip_http_ows(p);

        const char *name = p;
        while (http_token_char((unsigned char)*p))
            p++;
        if (p == name)
            return false;
        p = skip_http_ows(p);
        if (*p++ != '=')
            return false;
        p = skip_http_ows(p);

        if (*p == '"') {
            p++;
            bool closed = false;
            while (*p) {
                unsigned char ch = (unsigned char)*p++;
                if (ch == '"') {
                    closed = true;
                    break;
                }
                if (ch == '\\') {
                    if (*p == '\0')
                        return false;
                    p++;
                } else if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                    return false;
                }
            }
            if (!closed)
                return false;
        } else {
            const char *value = p;
            while (http_token_char((unsigned char)*p))
                p++;
            if (p == value)
                return false;
        }
        p = skip_http_ows(p);
        if (*p && *p != ';')
            return false;
    }
    return true;
}

static bool content_type_is_json(const char *content_type) {
    static const char json_type[] = "application/json";
    if (!content_type)
        return false;
    const char *semi = strchr(content_type, ';');
    size_t n = semi ? (size_t)(semi - content_type) : strlen(content_type);
    while (n > 0 && (content_type[n - 1] == ' ' || content_type[n - 1] == '\t'))
        n--;
    if (n != sizeof(json_type) - 1 ||
        !ascii_equal_nocase_n(content_type, json_type, sizeof(json_type) - 1)) {
        return false;
    }
    return !semi || json_content_type_parameters_valid(semi);
}

/* Reflect only the exact HTTP origin serving this listener. Merely being on a
 * loopback address is insufficient: unrelated local web apps are cross-origin
 * and must not gain write authority over this service. */
static void update_cors(const cbm_http_req_t *req, int server_port) {
    if (req->origin[0] != '\0' && origin_is_same_server(req->origin, req->host, server_port)) {
        snprintf(g_cors, sizeof(g_cors),
                 "Access-Control-Allow-Origin: %s\r\n"
                 "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n",
                 req->origin);
    } else {
        /* No Access-Control-Allow-Origin → browser blocks cross-origin access */
        snprintf(g_cors, sizeof(g_cors),
                 "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n");
    }
    snprintf(g_cors_json, sizeof(g_cors_json), "%sContent-Type: application/json\r\n", g_cors);
}

static const char *detect_ui_lang(const char *accept_language) {
    if (accept_language && (strstr(accept_language, "zh-CN") || strstr(accept_language, "zh"))) {
        return "zh";
    }
    return "en";
}

static void handle_ui_config(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    const char *lang = NULL;
    char cache_dir[1024];
    cbm_config_t *cfg = NULL;
    const char *resolved_cache = cbm_resolve_cache_dir();
    int cache_len =
        resolved_cache ? snprintf(cache_dir, sizeof(cache_dir), "%s", resolved_cache) : -1;
    if (cache_len > 0 && (size_t)cache_len < sizeof(cache_dir))
        cfg = cbm_config_open(cache_dir);
    if (cfg) {
        const char *pinned = cbm_config_get(cfg, CBM_CONFIG_UI_LANG, "auto");
        if (strcmp(pinned, "zh") == 0 || strcmp(pinned, "en") == 0) {
            lang = pinned;
        }
    }

    char lang_buf[8];
    snprintf(lang_buf, sizeof(lang_buf), "%s", lang ? lang : detect_ui_lang(req->accept_language));
    if (cfg) {
        cbm_config_close(cfg);
    }
    /* upstream_issues_url: where the missed-coverage callout (#963) sends
     * edge-case reports. Served from the backend on purpose — the UI security
     * audit forbids hardcoded external URLs in graph-ui source (external
     * targets must come from an auditable backend response, same pattern as
     * the /api/repo-info deep-links). */
    cbm_http_replyf(c, 200, g_cors_json, "{\"lang\":\"%s\",\"upstream_issues_url\":\"%s\"}",
                    lang_buf, CBM_GITHUB_ISSUES_NEW_URL);
}

/* ── Server state ─────────────────────────────────────────────── */

struct cbm_http_server {
    cbm_httpd_t *listener;
    cbm_mcp_server_t *mcp;       /* own MCP server instance (read-only) */
    struct cbm_watcher *watcher; /* external watcher ref (not owned) */
    atomic_int stop_flag;
    int port;
    bool listener_ok;
};

/* ── Forward declarations for process-kill PID validation ──────── */

#define MAX_INDEX_JOBS 4

typedef struct {
    char root_path[1024];
    char project_name[256];
    atomic_int status; /* 0=idle, 1=running, 2=done, 3=error */
    char error_msg[256];
    _Atomic uint64_t job_id;          /* monotonically assigned per slot reuse */
    cbm_proc_control_t child_control; /* stable supervisor-owned kill channel */
} index_job_t;

static index_job_t g_index_jobs[MAX_INDEX_JOBS];
static _Atomic uint64_t g_next_index_job_id = 1;

static uint64_t next_index_job_id(void) {
    uint64_t id = atomic_fetch_add_explicit(&g_next_index_job_id, 1, memory_order_relaxed);
    if (id == 0)
        id = atomic_fetch_add_explicit(&g_next_index_job_id, 1, memory_order_relaxed);
    return id;
}

/* ── Serve embedded asset ─────────────────────────────────────── */

/* Content-Security-Policy for the served UI. No external host appears in any
 * directive, so the browser cannot load or connect to anything off-origin —
 * this ENFORCES the airgap (the code makes no external calls; this stops a
 * future dependency or injected content from doing so). connect-src 'self'
 * confines fetch/XHR/WebSocket to the local server. The 'self'/data:/blob:/
 * 'unsafe-inline'-style/'wasm-unsafe-eval' allowances cover the bundled app's
 * own needs (React inline styles, three.js textures/workers/WASM). */
#define CBM_UI_CSP                                                       \
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; "  \
    "img-src 'self' data: blob:; script-src 'self' 'wasm-unsafe-eval'; " \
    "style-src 'self' 'unsafe-inline'; font-src 'self' data:; "          \
    "worker-src 'self' blob:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"

static bool serve_embedded(cbm_http_conn_t *c, const char *path) {
    const cbm_embedded_file_t *f = cbm_embedded_lookup(path);
    if (!f)
        return false;

    /* Build headers with correct Content-Type for this asset */
    char hdrs[1024];
    snprintf(hdrs, sizeof(hdrs),
             "%sContent-Type: %s\r\n"
             "Cache-Control: public, max-age=31536000, immutable\r\n" CBM_UI_CSP,
             g_cors, f->content_type);

    cbm_http_reply_buf(c, 200, hdrs, f->data, (size_t)f->size);
    return true;
}

/* Build DB path for a project: <cache_dir>/<project>.db */
bool cbm_http_server_project_db_path(const char *project, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0)
        return false;
    buf[0] = '\0';
    if (!cbm_validate_project_name(project))
        return false;
    const char *dir = cbm_resolve_cache_dir();
    if (!dir)
        return false;
    int n = snprintf(buf, bufsz, "%s/%s.db", dir, project);
    if (n <= 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return false;
    }
    return true;
}

/* ── Git remote → GitHub deep-link base (/api/repo-info) ───────── */

/* Return a copy of `url` with any "user[:password]@" userinfo removed from the
 * scheme://authority form, so credentials are never echoed back to the client.
 * scp-style (git@host:path) is returned unchanged: "git" there is a login name,
 * not a secret. malloc'd copy, or NULL when url is NULL. Caller frees. */
char *cbm_ui_git_strip_credentials(const char *url) {
    if (!url)
        return NULL;
    const char *sep = strstr(url, "://");
    if (!sep)
        return strdup(url); /* scp-style / opaque — no scheme userinfo to strip */
    const char *authority = sep + 3;
    const char *slash = strchr(authority, '/');
    const char *at = strchr(authority, '@');
    if (!at || (slash && at > slash))
        return strdup(url); /* '@' is in the path, not the authority → no creds */
    size_t prefix = (size_t)(authority - url); /* "scheme://" */
    const char *rest = at + 1;
    size_t out_len = prefix + strlen(rest) + 1;
    char *out = malloc(out_len);
    if (!out)
        return NULL;
    memcpy(out, url, prefix);
    memcpy(out + prefix, rest, strlen(rest) + 1);
    return out;
}

/* Normalize a git remote URL (scp-style, ssh://, https://) to a canonical
 * "https://host/org/repo" web base with any trailing ".git" and any embedded
 * credentials removed. Returns a malloc'd string or NULL if the shape isn't
 * recognized. Caller frees. */
char *cbm_ui_git_web_base(const char *url) {
    if (!url || !url[0])
        return NULL;
    char host_path[1024] = {0}; /* "host/org/repo" */
    if (strncmp(url, "git@", 4) == 0) {
        const char *at = url + 4;
        const char *colon = strchr(at, ':');
        if (!colon)
            return NULL;
        snprintf(host_path, sizeof(host_path), "%.*s/%s", (int)(colon - at), at, colon + 1);
    } else {
        const char *p = strstr(url, "://");
        if (!p)
            return NULL;
        p += 3;
        const char *at = strchr(p, '@'); /* strip any embedded credentials */
        if (at)
            p = at + 1;
        snprintf(host_path, sizeof(host_path), "%s", p);
    }
    size_t l = strlen(host_path);
    if (l > 4 && strcmp(host_path + l - 4, ".git") == 0)
        host_path[l - 4] = '\0';
    l = strlen(host_path);
    if (l > 0 && host_path[l - 1] == '/')
        host_path[l - 1] = '\0';
    size_t out_sz = strlen(host_path) + 9; /* "https://" (8) + NUL */
    char *out = malloc(out_sz);
    if (!out)
        return NULL;
    /* Legitimate GitHub blob-URL construction, not a network call — the scheme
     * is https-forced here so the frontend deep-link can never be downgraded.
     * Allow-listed in scripts/security-allowlist.txt (URL:https://%s). */
    snprintf(out, out_sz, "https://%s", host_path);
    return out;
}

/* Read the "origin" remote URL for the repo at root_path. malloc'd or NULL.
 * libgit2 is initialized once at process start by cbm_alloc_init() (which also
 * binds its allocator to mimalloc) — do NOT git_libgit2_init()/shutdown() here:
 * a per-request shutdown could drop the global refcount and tear down that
 * allocator binding mid-process. */
static char *git_origin_remote_url(const char *root_path) {
#if defined(HAVE_LIBGIT2)
    git_repository *repo = NULL;
    char *out = NULL;
    if (git_repository_open(&repo, root_path) == 0) {
        git_remote *rem = NULL;
        if (git_remote_lookup(&rem, repo, "origin") == 0) {
            const char *u = git_remote_url(rem);
            if (u)
                out = strdup(u);
            git_remote_free(rem);
        }
        git_repository_free(repo);
    }
    return out;
#else
    (void)root_path;
    return NULL;
#endif
}

/* GET /api/repo-info?project=NAME → { root_path, branch, remote_url, web_base,
 * blob_base }. blob_base is "<web_base>/blob/<branch>" ready for the frontend to
 * append "/<file_path>#L<start>-L<end>". remote_url is credential-stripped;
 * fields are empty strings when unknown (e.g. no git remote). */
static void handle_repo_info(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char project[256] = {0};
    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        project[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project parameter\"}");
        return;
    }

    char db_path[1024];
    cbm_http_server_project_db_path(project, db_path, sizeof(db_path));
    if (db_path[0] == '\0' || !cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    char root_path[1024] = {0};
    cbm_project_t proj;
    memset(&proj, 0, sizeof(proj));
    if (cbm_store_get_project(store, project, &proj) == CBM_STORE_OK && proj.root_path) {
        snprintf(root_path, sizeof(root_path), "%s", proj.root_path);
    }
    cbm_project_free_fields(&proj);
    cbm_store_close(store);

    char branch[256] = {0};
    if (root_path[0]) {
        cbm_git_context_t gctx;
        memset(&gctx, 0, sizeof(gctx));
        if (cbm_git_context_resolve(root_path, &gctx) == 0 && gctx.branch) {
            snprintf(branch, sizeof(branch), "%s", gctx.branch);
        }
        cbm_git_context_free(&gctx);
    }

    char *remote = root_path[0] ? git_origin_remote_url(root_path) : NULL;
    char *remote_safe = cbm_ui_git_strip_credentials(remote); /* never echo secrets */
    char *web_base = cbm_ui_git_web_base(remote);

    char blob_base[1152] = {0};
    if (web_base && web_base[0] && branch[0]) {
        snprintf(blob_base, sizeof(blob_base), "%s/blob/%s", web_base, branch);
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    char *json = NULL;
    if (doc && root) {
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strcpy(doc, root, "root_path", root_path);
        yyjson_mut_obj_add_strcpy(doc, root, "branch", branch);
        yyjson_mut_obj_add_strcpy(doc, root, "remote_url", remote_safe ? remote_safe : "");
        yyjson_mut_obj_add_strcpy(doc, root, "web_base", web_base ? web_base : "");
        yyjson_mut_obj_add_strcpy(doc, root, "blob_base", blob_base);
        json = yyjson_mut_write(doc, 0, NULL);
    }
    if (json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", json);
        free(json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
    }
    if (doc)
        yyjson_mut_doc_free(doc);

    free(remote);
    free(remote_safe);
    free(web_base);
}

/* ── Log ring buffer ──────────────────────────────────────────── */

#define LOG_RING_SIZE 500
#define LOG_LINE_MAX 512

static char g_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static int g_log_head = 0;
static int g_log_count = 0;
static cbm_mutex_t g_log_mutex;

enum { CBM_LOG_MUTEX_UNINIT = 0, CBM_LOG_MUTEX_INITING = 1, CBM_LOG_MUTEX_INITED = 2 };
static atomic_int g_log_mutex_init = CBM_LOG_MUTEX_UNINIT;

/* Safe for concurrent callers: only publishes INITED after cbm_mutex_init()
 * has completed. Callers that lose the CAS race spin until init finishes. */
void cbm_ui_log_init(void) {
    int state = atomic_load(&g_log_mutex_init);
    if (state == CBM_LOG_MUTEX_INITED)
        return;

    state = CBM_LOG_MUTEX_UNINIT;
    if (atomic_compare_exchange_strong(&g_log_mutex_init, &state, CBM_LOG_MUTEX_INITING)) {
        cbm_mutex_init(&g_log_mutex);
        atomic_store(&g_log_mutex_init, CBM_LOG_MUTEX_INITED);
        return;
    }

    /* Another thread is initializing — spin until done */
    while (atomic_load(&g_log_mutex_init) != CBM_LOG_MUTEX_INITED) {
        cbm_usleep(1000); /* 1ms */
    }
}

/* Called from a log hook — appends a line to the ring buffer (thread-safe) */
void cbm_ui_log_append(const char *line) {
    if (!line)
        return;
    /* Ensure mutex is initialized (safe for early single-threaded logging
     * and concurrent calls via atomic_exchange once-init pattern). */
    cbm_ui_log_init();
    cbm_mutex_lock(&g_log_mutex);
    snprintf(g_log_ring[g_log_head], LOG_LINE_MAX, "%s", line);
    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    if (g_log_count < LOG_RING_SIZE)
        g_log_count++;
    cbm_mutex_unlock(&g_log_mutex);
}

/* Append a printf-formatted fragment at *pos within a bufsz buffer, never
 * advancing *pos past bufsz. snprintf returns the length it WOULD have written,
 * so `pos += snprintf(...)` runs pos past the end on truncation and the next
 * call computes a wrapped (huge) remaining size and writes out of bounds. This
 * clamps: on truncation *pos is pinned at bufsz and further appends are no-ops. */
/* GET /api/logs?lines=N — returns last N log lines */
static void handle_logs(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    cbm_ui_log_init();
    char lines_str[16] = {0};
    int max_lines = 100;
    if (cbm_http_query_param(req->query, "lines", lines_str, (int)sizeof(lines_str))) {
        int v = atoi(lines_str);
        if (v > 0 && v <= LOG_RING_SIZE)
            max_lines = v;
    }

    cbm_mutex_lock(&g_log_mutex);
    int count = g_log_count < max_lines ? g_log_count : max_lines;
    int start = (g_log_head - count + LOG_RING_SIZE) % LOG_RING_SIZE;
    int total = g_log_count;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *lines = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !lines) {
        cbm_mutex_unlock(&g_log_mutex);
        if (doc)
            yyjson_mut_doc_free(doc);
        cbm_http_replyf(c, 500, g_cors, "oom");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        if (!yyjson_mut_arr_add_strcpy(doc, lines, g_log_ring[idx]))
            break;
    }
    cbm_mutex_unlock(&g_log_mutex);
    yyjson_mut_obj_add_val(doc, root, "lines", lines);
    yyjson_mut_obj_add_int(doc, root, "total", total);

    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", json);
        free(json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
    }
    yyjson_mut_doc_free(doc);
}

/* ── Process monitoring ───────────────────────────────────────── */

#ifndef _WIN32
#include <sys/resource.h>

typedef struct {
    int pid;
    double cpu;
    long rss_kb;
    char elapsed[64];
    char command[256];
} ui_process_info_t;

/* Use only fixed, absolute system binaries. Running `ps | grep` through a
 * shell let an inherited PATH select attacker-controlled executables. */
static const char *ui_system_binary(const char *first, const char *second) {
    if (first && access(first, X_OK) == 0)
        return first;
    if (second && access(second, X_OK) == 0)
        return second;
    return NULL;
}

static size_t collect_ui_processes(ui_process_info_t *out, size_t cap) {
    const char *env_bin = ui_system_binary("/usr/bin/env", "/bin/env");
    const char *ps_bin = ui_system_binary("/bin/ps", "/usr/bin/ps");
    if (!env_bin || !ps_bin || !out || cap == 0)
        return 0;

    const char *argv[] = {env_bin, "LC_ALL=C", ps_bin, "-eo", "pid=,pcpu=,rss=,etime=,comm=", NULL};
    cbm_proc_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bin = env_bin;
    opts.argv = argv;
    opts.discard_stderr = true;

    char *data = NULL;
    size_t data_len = 0;
    char output_hash[65];
    cbm_proc_result_t result;
    if (cbm_subprocess_capture(&opts, 1024U * 1024U, &data, &data_len, output_hash, &result) != 0 ||
        !data) {
        free(data);
        return 0;
    }

    size_t count = 0;
    char *line = data;
    char *end = data + data_len;
    while (line < end && count < cap) {
        char *newline = memchr(line, '\n', (size_t)(end - line));
        char *line_end = newline ? newline : end;
        size_t line_len = (size_t)(line_end - line);
        char line_buf[1024];
        if (line_len >= sizeof(line_buf)) {
            line = newline ? newline + 1 : end;
            continue;
        }
        memcpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        ui_process_info_t item;
        memset(&item, 0, sizeof(item));
        if (sscanf(line_buf, "%d %lf %ld %63s %255s", &item.pid, &item.cpu, &item.rss_kb,
                   item.elapsed, item.command) == 5 &&
            /* Linux TASK_COMM_LEN truncates the product name to
             * "codebase-memory"; match that stable prefix. */
            strstr(item.command, "codebase-memory") != NULL) {
            out[count++] = item;
        }

        line = newline ? newline + 1 : end;
    }
    free(data);
    return count;
}
#endif

static bool owned_index_job_for_pid(uint64_t pid, uint64_t *job_id) {
    if (job_id)
        *job_id = 0;
    if (pid == 0)
        return false;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load_explicit(&g_index_jobs[i].status, memory_order_acquire) != 1)
            continue;
        if (cbm_proc_control_pid(&g_index_jobs[i].child_control) != pid)
            continue;
        uint64_t id = atomic_load_explicit(&g_index_jobs[i].job_id, memory_order_acquire);
        if (id == 0)
            return false;
        if (job_id)
            *job_id = id;
        return true;
    }
    return false;
}

/* GET /api/processes — list codebase-memory-mcp processes without a shell. */
static void handle_processes(cbm_http_conn_t *c) {
    double self_rss_mb = 0.0;
    double user_s = 0.0;
    double sys_s = 0.0;

#ifndef _WIN32
    ui_process_info_t processes[64];
    size_t process_count =
        collect_ui_processes(processes, sizeof(processes) / sizeof(processes[0]));
#endif

#ifdef _WIN32
    /* Windows: GetProcessMemoryInfo + GetProcessTimes */
    PROCESS_MEMORY_COUNTERS pmc;
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    size_t rss_bytes = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        rss_bytes = pmc.WorkingSetSize;
    if (GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        ULARGE_INTEGER u, k;
        u.LowPart = ft_user.dwLowDateTime;
        u.HighPart = ft_user.dwHighDateTime;
        k.LowPart = ft_kernel.dwLowDateTime;
        k.HighPart = ft_kernel.dwHighDateTime;
        user_s = (double)u.QuadPart / 1e7;
        sys_s = (double)k.QuadPart / 1e7;
    }
    self_rss_mb = (double)rss_bytes / (1024.0 * 1024.0);
#else
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    (void)getrusage(RUSAGE_SELF, &ru);
    long rss_kb = ru.ru_maxrss;
#ifdef __APPLE__
    rss_kb /= 1024;
#endif
    self_rss_mb = (double)rss_kb / 1024.0; /* fallback: peak RSS */
    user_s = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
    sys_s = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
    for (size_t i = 0; i < process_count; i++) {
        if (processes[i].pid == (int)getpid()) {
            self_rss_mb = (double)processes[i].rss_kb / 1024.0; /* current RSS */
            break;
        }
    }
#endif

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *arr = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !arr) {
        if (doc)
            yyjson_mut_doc_free(doc);
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"oom\"}");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
#ifdef _WIN32
    yyjson_mut_obj_add_int(doc, root, "self_pid", (int)_getpid());
#else
    yyjson_mut_obj_add_int(doc, root, "self_pid", (int)getpid());
#endif
    yyjson_mut_obj_add_real(doc, root, "self_rss_mb", self_rss_mb);
    yyjson_mut_obj_add_real(doc, root, "self_user_cpu_s", user_s);
    yyjson_mut_obj_add_real(doc, root, "self_sys_cpu_s", sys_s);
    yyjson_mut_obj_add_val(doc, root, "processes", arr);

#ifndef _WIN32
    for (size_t i = 0; i < process_count; i++) {
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        if (!entry)
            break;
        yyjson_mut_obj_add_int(doc, entry, "pid", processes[i].pid);
        yyjson_mut_obj_add_real(doc, entry, "cpu", processes[i].cpu);
        yyjson_mut_obj_add_real(doc, entry, "rss_mb", (double)processes[i].rss_kb / 1024.0);
        yyjson_mut_obj_add_strcpy(doc, entry, "elapsed", processes[i].elapsed);
        yyjson_mut_obj_add_strcpy(doc, entry, "command", processes[i].command);
        yyjson_mut_obj_add_bool(doc, entry, "is_self", processes[i].pid == (int)getpid());
        uint64_t job_id = 0;
        bool killable =
            processes[i].pid > 0 && owned_index_job_for_pid((uint64_t)processes[i].pid, &job_id);
        yyjson_mut_obj_add_bool(doc, entry, "killable", killable);
        char job_id_text[32];
        snprintf(job_id_text, sizeof(job_id_text), "%llu", (unsigned long long)job_id);
        yyjson_mut_obj_add_strcpy(doc, entry, "job_id", job_id_text);
        yyjson_mut_arr_add_val(arr, entry);
    }
#else
    /* Windows has no `ps` parser above. Expose only subprocesses whose live
     * control objects this server owns; these are exactly the PIDs the kill
     * endpoint can safely target without a stale-PID race. */
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load_explicit(&g_index_jobs[i].status, memory_order_acquire) != 1)
            continue;
        uint64_t pid = cbm_proc_control_pid(&g_index_jobs[i].child_control);
        uint64_t job_id = atomic_load_explicit(&g_index_jobs[i].job_id, memory_order_acquire);
        if (pid == 0 || pid > UINT32_MAX || job_id == 0)
            continue;
        HANDLE process =
            OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
        if (!process)
            continue;
        PROCESS_MEMORY_COUNTERS child_mem;
        FILETIME child_create, child_exit, child_kernel, child_user;
        double cpu = 0.0;
        double rss_mb = 0.0;
        if (GetProcessMemoryInfo(process, &child_mem, sizeof(child_mem)))
            rss_mb = (double)child_mem.WorkingSetSize / (1024.0 * 1024.0);
        if (GetProcessTimes(process, &child_create, &child_exit, &child_kernel, &child_user)) {
            ULARGE_INTEGER kernel_ticks, user_ticks;
            kernel_ticks.LowPart = child_kernel.dwLowDateTime;
            kernel_ticks.HighPart = child_kernel.dwHighDateTime;
            user_ticks.LowPart = child_user.dwLowDateTime;
            user_ticks.HighPart = child_user.dwHighDateTime;
            cpu = (double)(kernel_ticks.QuadPart + user_ticks.QuadPart) / 1e7;
        }
        CloseHandle(process);

        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        if (!entry)
            break;
        yyjson_mut_obj_add_uint(doc, entry, "pid", pid);
        yyjson_mut_obj_add_real(doc, entry, "cpu", cpu);
        yyjson_mut_obj_add_real(doc, entry, "rss_mb", rss_mb);
        yyjson_mut_obj_add_str(doc, entry, "elapsed", "");
        yyjson_mut_obj_add_str(doc, entry, "command", "codebase-memory-mcp index worker");
        yyjson_mut_obj_add_bool(doc, entry, "is_self", false);
        yyjson_mut_obj_add_bool(doc, entry, "killable", true);
        char job_id_text[32];
        snprintf(job_id_text, sizeof(job_id_text), "%llu", (unsigned long long)job_id);
        yyjson_mut_obj_add_strcpy(doc, entry, "job_id", job_id_text);
        yyjson_mut_arr_add_val(arr, entry);
    }
#endif

    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", json);
        free(json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
    }
    yyjson_mut_doc_free(doc);
}

static bool json_positive_u64(yyjson_val *value, uint64_t *out) {
    if (!value || !out)
        return false;
    if (yyjson_is_str(value)) {
        const char *text = yyjson_get_str(value);
        size_t len = yyjson_get_len(value);
        if (!text || len == 0 || len != strlen(text))
            return false;
        uint64_t number = 0;
        for (size_t i = 0; i < len; i++) {
            if (text[i] < '0' || text[i] > '9')
                return false;
            unsigned digit = (unsigned)(text[i] - '0');
            if (number > (UINT64_MAX - digit) / 10U)
                return false;
            number = number * 10U + digit;
        }
        if (number == 0)
            return false;
        *out = number;
        return true;
    }
    if (!yyjson_is_int(value))
        return false;
    if (yyjson_is_uint(value)) {
        uint64_t number = yyjson_get_uint(value);
        if (number == 0)
            return false;
        *out = number;
        return true;
    }
    int64_t number = yyjson_get_sint(value);
    if (number <= 0)
        return false;
    *out = (uint64_t)number;
    return true;
}

/* POST /api/process-kill — request termination by PID + per-run job identity. */
static void handle_process_kill(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 256) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_pid = yyjson_is_obj(root) ? yyjson_obj_get(root, "pid") : NULL;
    yyjson_val *v_job_id = yyjson_is_obj(root) ? yyjson_obj_get(root, "job_id") : NULL;
    uint64_t target_pid = 0;
    uint64_t target_job_id = 0;
    if (!json_positive_u64(v_pid, &target_pid) || !json_positive_u64(v_job_id, &target_job_id)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"valid pid and job_id required\"}");
        return;
    }
    yyjson_doc_free(doc);

#ifdef _WIN32
    const uint64_t max_pid = UINT32_MAX;
#else
    const uint64_t max_pid = INT_MAX;
#endif
    if (target_pid == 0 || target_pid > max_pid) {
        cbm_http_replyf(c, 400, g_cors_json, "%s", "{\"error\":\"invalid pid\"}");
        return;
    }

    /* Keep the conditional compilation outside the `if` expression. Raw C
     * parsers see every preprocessor branch at once; splitting one control
     * statement across branches makes the file syntactically unbalanced and
     * can hide this security-sensitive handler from the code graph. */
#ifdef _WIN32
    uint64_t self_pid = (uint64_t)_getpid();
#else
    uint64_t self_pid = (uint64_t)getpid();
#endif
    if (target_pid == self_pid) {
        cbm_http_replyf(c, 400, g_cors_json,
                        "{\"error\":\"cannot kill self (use the UI server's own shutdown)\"}");
        return;
    }

    /* Never signal a numeric PID here. The supervisor still owns the native
     * child relationship/HANDLE and consumes this atomic request, so an exited
     * child's reused PID can never target an unrelated process. */
    bool requested = false;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        if (atomic_load_explicit(&g_index_jobs[i].status, memory_order_acquire) == 1 &&
            atomic_load_explicit(&g_index_jobs[i].job_id, memory_order_acquire) == target_job_id &&
            cbm_proc_control_request_kill(&g_index_jobs[i].child_control, target_pid)) {
            requested = true;
            break;
        }
    }
    if (!requested) {
        cbm_http_replyf(c, 403, g_cors_json,
                        "{\"error\":\"can only kill a live server-spawned process\"}");
        return;
    }

    cbm_http_replyf(c, 202, g_cors_json, "{\"kill_requested\":%llu,\"job_id\":\"%llu\"}",
                    (unsigned long long)target_pid, (unsigned long long)target_job_id);
}

/* ── Directory browser ────────────────────────────────────────── */

#include <dirent.h>

/* GET /api/browse?path=/some/dir — list subdirectories for file picker */
static void handle_browse(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char path[1024] = {0};
    const char *home = cbm_get_home_dir();
    if (!cbm_http_query_param(req->query, "path", path, (int)sizeof(path)) || path[0] == '\0') {
        /* Default to home directory */
        if (home)
            snprintf(path, sizeof(path), "%s", home);
        else
            snprintf(path, sizeof(path), "/");
    }

    /* The browser UI may send Windows backslash separators (e.g.
     * "D:\projects\demo"). Normalize to forward slashes before the cbm_is_dir
     * gate, exactly as the MCP repo_path handler and cbm_project_name_from_path
     * already do — otherwise a real D:/ directory is rejected (#548). */
    cbm_normalize_path_sep(path);

    if (!cbm_is_dir(path)) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"not a directory\"}");
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        cbm_http_replyf(c, 403, g_cors_json, "{\"error\":\"cannot open directory\"}");
        return;
    }

    /* Build with yyjson rather than a fixed buffer. Besides making quotes and
     * control bytes safe, this avoids the old overflow path where a direct
     * comma write replaced vsnprintf's final NUL after a wide directory. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *dirs = doc ? yyjson_mut_arr(doc) : NULL;
    yyjson_mut_val *roots = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !dirs || !roots) {
        if (doc)
            yyjson_mut_doc_free(doc);
        closedir(dir);
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"oom\"}");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "path", path);
    yyjson_mut_obj_add_val(doc, root, "dirs", dirs);

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        /* Skip hidden dirs and . / .. */
        if (ent->d_name[0] == '.')
            continue;

        /* Check if it's actually a directory */
        char full[2048];
        int full_len = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (full_len <= 0 || (size_t)full_len >= sizeof(full))
            continue;
        if (!cbm_is_dir(full))
            continue;

        if (!yyjson_mut_arr_add_strcpy(doc, dirs, ent->d_name))
            break;
        count++;

        if (count >= 200)
            break; /* safety limit */
    }
    closedir(dir);

    /* Parent path — escape to prevent injection */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", path);
    char *last_slash = strrchr(parent, '/');
    /* A Windows drive root "X:/" is its own parent (like POSIX "/"): truncating
     * at the slash would yield the bare drive spec "X:", which the next browse
     * resolves to the wrong directory and strands the user at the root (#548). */
    size_t parent_len = strlen(parent);
    bool is_drive_root = parent_len == 3 && parent[1] == ':' && parent[2] == '/';
    if (is_drive_root) {
        /* leave "X:/" unchanged */
    } else if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        snprintf(parent, sizeof(parent), "/");
    }

    yyjson_mut_obj_add_strcpy(doc, root, "parent", parent);
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (drives & (1u << i)) {
            char drive[4] = {(char)('A' + i), ':', '/', '\0'};
            if (!yyjson_mut_arr_add_strcpy(doc, roots, drive))
                break;
        }
    }
#else
    yyjson_mut_arr_add_strcpy(doc, roots, "/");
#endif
    yyjson_mut_obj_add_val(doc, root, "roots", roots);

    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", json);
        free(json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
    }
    yyjson_mut_doc_free(doc);
}

/* ── ADR endpoints ────────────────────────────────────────────── */

/* GET /api/adr?project=X — get ADR content for a project */
static void handle_adr_get(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "project", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project\"}");
        return;
    }

    char db_path[1024];
    cbm_http_server_project_db_path(name, db_path, sizeof(db_path));

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"has_adr\":false}");
        return;
    }

    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    if (cbm_store_adr_get(store, name, &adr) == CBM_STORE_OK && adr.content) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
        if (doc && root) {
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_bool(doc, root, "has_adr", true);
            yyjson_mut_obj_add_strcpy(doc, root, "content", adr.content);
            yyjson_mut_obj_add_strcpy(doc, root, "updated_at",
                                      adr.updated_at ? adr.updated_at : "");
            char *json = yyjson_mut_write(doc, 0, NULL);
            if (json) {
                cbm_http_replyf(c, 200, g_cors_json, "%s", json);
                free(json);
            } else {
                cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
            }
            yyjson_mut_doc_free(doc);
        } else {
            if (doc)
                yyjson_mut_doc_free(doc);
            cbm_http_replyf(c, 500, g_cors, "oom");
        }
        cbm_store_adr_free(&adr);
    } else {
        cbm_http_replyf(c, 200, g_cors_json, "{\"has_adr\":false}");
    }
    cbm_store_close(store);
}

/* POST /api/adr — save ADR content. Body: {"project":"...","content":"..."} */
static void handle_adr_save(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 16384) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_proj = yyjson_obj_get(root, "project");
    yyjson_val *v_content = yyjson_obj_get(root, "content");
    if (!v_proj || !yyjson_is_str(v_proj) || !v_content || !yyjson_is_str(v_content)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project or content\"}");
        return;
    }

    const char *proj = yyjson_get_str(v_proj);
    const char *content = yyjson_get_str(v_content);
    if (proj[0] == '\0' || yyjson_get_len(v_proj) != strlen(proj) ||
        yyjson_get_len(v_content) != strlen(content)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "%s", "{\"error\":\"invalid project or content\"}");
        return;
    }

    char db_path[1024];
    cbm_http_server_project_db_path(proj, db_path, sizeof(db_path));
    if (db_path[0] == '\0') {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 404, g_cors_json, "%s", "{\"error\":\"project not found\"}");
        return;
    }

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    int rc = cbm_store_adr_store(store, proj, content);
    cbm_store_close(store);
    yyjson_doc_free(doc);

    if (rc == CBM_STORE_OK) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"saved\":true}");
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"save failed\"}");
    }
}

/* ── Background indexing ──────────────────────────────────────── */

static char g_binary_path[1024] = {0};

static bool copy_path(char *out, size_t outsz, const char *path) {
    if (!out || outsz == 0 || !path || !path[0]) {
        return false;
    }
    int n = snprintf(out, outsz, "%s", path);
    return n > 0 && (size_t)n < outsz;
}

static bool canonical_executable(const char *path, char *out, size_t outsz) {
    if (!path || !path[0] || !out || outsz == 0)
        return false;
    char canonical[4096];
    if (!cbm_canonical_path(path, canonical, sizeof(canonical)))
        return false;
#ifndef _WIN32
    struct stat st;
    if (stat(canonical, &st) != 0 || !S_ISREG(st.st_mode) || access(canonical, X_OK) != 0)
        return false;
#else
    if (cbm_file_size(canonical) < 0)
        return false;
#endif
    return copy_path(out, outsz, canonical);
}

static bool ui_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return false;
#ifdef _WIN32
    return ((path[0] == '\\' && path[1] == '\\') ||
            (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
             path[1] == ':' && (path[2] == '\\' || path[2] == '/')));
#else
    return path[0] == '/';
#endif
}

#ifndef _WIN32
static bool resolve_self_executable(char *out, size_t outsz) {
#if defined(__APPLE__)
    char probe[1];
    uint32_t size = sizeof(probe);
    if (_NSGetExecutablePath(probe, &size) == 0)
        return canonical_executable(probe, out, outsz);
    if (size == 0 || size > 1024U * 1024U)
        return false;
    char *buf = malloc(size);
    if (!buf)
        return false;
    bool ok = _NSGetExecutablePath(buf, &size) == 0 && canonical_executable(buf, out, outsz);
    free(buf);
    return ok;
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0 && (size_t)len < sizeof(buf) - 1U) {
        buf[len] = '\0';
        return canonical_executable(buf, out, outsz);
    }
    return false;
#endif
}
#else
static bool resolve_self_executable(char *out, size_t outsz) {
    wchar_t wide[32768];
    DWORD n = GetModuleFileNameW(NULL, wide, (DWORD)(sizeof(wide) / sizeof(wide[0])));
    if (n == 0 || n >= (DWORD)(sizeof(wide) / sizeof(wide[0])))
        return false;
    char *utf8 = cbm_wide_to_utf8(wide);
    bool ok = utf8 && canonical_executable(utf8, out, outsz);
    free(utf8);
    return ok;
}
#endif

bool cbm_http_server_resolve_binary_path(const char *argv0, char *out, size_t outsz) {
    if (!out || outsz == 0) {
        return false;
    }
    out[0] = '\0';

    /* Honor only an explicit path. A bare argv[0] must never re-resolve through
     * a mutable PATH/current directory after process startup. */
    if (ui_path_is_absolute(argv0) && canonical_executable(argv0, out, outsz)) {
        return true;
    }
    return resolve_self_executable(out, outsz);
}

void cbm_http_server_set_binary_path(const char *path) {
    if (path) {
        if (!cbm_http_server_resolve_binary_path(path, g_binary_path, sizeof(g_binary_path))) {
            g_binary_path[0] = '\0';
        }
    }
}

static void index_log_line(const char *line, void *ud) {
    (void)ud;
    cbm_ui_log_append(line);
}

/* Index via the shared cross-platform supervisor. The old local fork/wait loop
 * reaped the child once, discarded that status, then interpreted ECHILD plus a
 * zero-initialized status as success. One private descriptor now carries both
 * output and progress; no predictable pathname is reopened by the child. */
static void *index_thread_fn(void *arg) {
    index_job_t *job = arg;
    cbm_log_info("ui.index.start", "path", job->root_path);

    char bin[1024];
    if (g_binary_path[0]) {
        if (!copy_path(bin, sizeof(bin), g_binary_path)) {
            snprintf(job->error_msg, sizeof(job->error_msg), "index binary path is too long");
            atomic_store(&job->status, 3);
            return NULL;
        }
    } else if (!cbm_http_server_resolve_binary_path(NULL, bin, sizeof(bin))) {
        snprintf(job->error_msg, sizeof(job->error_msg), "cannot resolve index binary");
        atomic_store(&job->status, 3);
        return NULL;
    }

    yyjson_mut_doc *arg_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arg_root = arg_doc ? yyjson_mut_obj(arg_doc) : NULL;
    char *json_arg = NULL;
    if (arg_doc && arg_root) {
        yyjson_mut_doc_set_root(arg_doc, arg_root);
        yyjson_mut_obj_add_strcpy(arg_doc, arg_root, "repo_path", job->root_path);
        if (job->project_name[0])
            yyjson_mut_obj_add_strcpy(arg_doc, arg_root, "name", job->project_name);
        json_arg = yyjson_mut_write(arg_doc, 0, NULL);
    }
    if (arg_doc)
        yyjson_mut_doc_free(arg_doc);
    if (!json_arg) {
        snprintf(job->error_msg, sizeof(job->error_msg), "cannot serialize index request");
        atomic_store(&job->status, 3);
        return NULL;
    }

    char temp_path[1024];
    const char *tmp_dir = cbm_tmpdir();
    int temp_len = tmp_dir && tmp_dir[0]
                       ? snprintf(temp_path, sizeof(temp_path), "%s/cbm-index-log-XXXXXX", tmp_dir)
                       : -1;
    int output_fd =
        temp_len > 0 && (size_t)temp_len < sizeof(temp_path) ? cbm_mkstemp(temp_path) : -1;
    if (output_fd < 0) {
        free(json_arg);
        snprintf(job->error_msg, sizeof(job->error_msg), "cannot create private index log");
        atomic_store(&job->status, 3);
        return NULL;
    }
#ifndef _WIN32
    /* The supervisor and worker retain this exact descriptor. */
    if (cbm_unlink(temp_path) != 0) {
        (void)ui_close_fd(output_fd);
        free(json_arg);
        snprintf(job->error_msg, sizeof(job->error_msg), "cannot privatize index log");
        atomic_store(&job->status, 3);
        return NULL;
    }
#endif

    const char *argv[] = {bin, "cli", "--index-worker", "index_repository", json_arg, NULL};
    cbm_proc_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bin = bin;
    opts.argv = argv;
    opts.use_output_fd = true;
    opts.output_fd = output_fd;
    opts.max_output_bytes = 64U * 1024U * 1024U;
    opts.on_log_line = index_log_line;
    opts.control = &job->child_control;

    cbm_proc_result_t result;
    int run_rc = cbm_subprocess_run(&opts, &result);
    (void)ui_close_fd(output_fd);
#ifdef _WIN32
    (void)cbm_unlink(temp_path);
#endif
    free(json_arg);

    bool clean = run_rc == 0 && result.outcome == CBM_PROC_CLEAN;
    if (!clean) {
        snprintf(job->error_msg, sizeof(job->error_msg), "indexing failed (%s, exit=%d, signal=%d)",
                 cbm_proc_outcome_str(result.outcome), result.exit_code, result.term_signal);
    }
    cbm_log_info("ui.index.done", "path", job->root_path, "rc", clean ? "ok" : "err");
    /* Publish terminal state only after the final access to mutable job fields;
     * the single-threaded dispatcher may immediately reuse a terminal slot. */
    atomic_store(&job->status, clean ? 2 : 3);
    return NULL;
}

/* POST /api/index — body: {"root_path": "/abs/path", "project_name": "..."} */
static void handle_index_start(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    if (req->body_len == 0 || req->body_len > 4096) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid body\"}");
        return;
    }

    yyjson_doc *doc = yyjson_read(req->body, req->body_len, 0);
    if (!doc) {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"invalid json\"}");
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v_path = yyjson_obj_get(root, "root_path");
    if (!v_path || !yyjson_is_str(v_path)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing root_path\"}");
        return;
    }
    const char *rpath = yyjson_get_str(v_path);
    yyjson_val *v_project_name = yyjson_obj_get(root, "project_name");
    const char *project_name = yyjson_is_str(v_project_name) ? yyjson_get_str(v_project_name) : "";

    if (yyjson_get_len(v_path) != strlen(rpath) ||
        (yyjson_is_str(v_project_name) && yyjson_get_len(v_project_name) != strlen(project_name)) ||
        strlen(rpath) >= sizeof(g_index_jobs[0].root_path) ||
        strlen(project_name) >= sizeof(g_index_jobs[0].project_name)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "%s", "{\"error\":\"path or name too long\"}");
        return;
    }

    /* Check path exists */
    if (!cbm_is_dir(rpath)) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"directory not found\"}");
        return;
    }

    /* Find free job slot */
    int slot = -1;
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        int st = atomic_load(&g_index_jobs[i].status);
        if (st == 0 || st == 2 || st == 3) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        yyjson_doc_free(doc);
        cbm_http_replyf(c, 429, g_cors_json, "{\"error\":\"all index slots busy\"}");
        return;
    }

    index_job_t *job = &g_index_jobs[slot];
    memcpy(job->root_path, rpath, strlen(rpath) + 1U);
    memcpy(job->project_name, project_name, strlen(project_name) + 1U);
    job->error_msg[0] = '\0';
    cbm_proc_control_init(&job->child_control);
    uint64_t job_id = next_index_job_id();
    char job_id_text[32];
    snprintf(job_id_text, sizeof(job_id_text), "%llu", (unsigned long long)job_id);
    atomic_store_explicit(&job->job_id, job_id, memory_order_relaxed);
    atomic_store_explicit(&job->status, 1, memory_order_release);
    yyjson_doc_free(doc);

    /* Spawn background thread */
    cbm_thread_t tid;
    if (cbm_thread_create(&tid, 0, index_thread_fn, job) != 0) {
        snprintf(job->error_msg, sizeof(job->error_msg), "thread creation failed");
        atomic_store(&job->status, 3);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"thread creation failed\"}");
        return;
    }
    cbm_thread_detach(&tid); /* Don't leak thread handle */

    yyjson_mut_doc *reply_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *reply = reply_doc ? yyjson_mut_obj(reply_doc) : NULL;
    if (!reply_doc || !reply) {
        if (reply_doc)
            yyjson_mut_doc_free(reply_doc);
        cbm_http_replyf(c, 202, g_cors_json, "{\"status\":\"indexing\",\"job_id\":\"%s\"}",
                        job_id_text);
        return;
    }
    yyjson_mut_doc_set_root(reply_doc, reply);
    yyjson_mut_obj_add_str(reply_doc, reply, "status", "indexing");
    yyjson_mut_obj_add_int(reply_doc, reply, "slot", slot);
    yyjson_mut_obj_add_strcpy(reply_doc, reply, "job_id", job_id_text);
    yyjson_mut_obj_add_strcpy(reply_doc, reply, "path", job->root_path);
    char *reply_json = yyjson_mut_write(reply_doc, 0, NULL);
    if (reply_json) {
        cbm_http_replyf(c, 202, g_cors_json, "%s", reply_json);
        free(reply_json);
    } else {
        cbm_http_replyf(c, 202, g_cors_json, "{\"status\":\"indexing\",\"job_id\":\"%s\"}",
                        job_id_text);
    }
    yyjson_mut_doc_free(reply_doc);
}

/* GET /api/index-status — returns status of all index jobs */
static void handle_index_status(cbm_http_conn_t *c) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *jobs = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !jobs) {
        if (doc)
            yyjson_mut_doc_free(doc);
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"oom\"}");
        return;
    }
    yyjson_mut_doc_set_root(doc, jobs);
    for (int i = 0; i < MAX_INDEX_JOBS; i++) {
        int st = atomic_load(&g_index_jobs[i].status);
        if (st == 0)
            continue;
        const char *ss = st == 1 ? "indexing" : st == 2 ? "done" : "error";
        yyjson_mut_val *job = yyjson_mut_obj(doc);
        if (!job)
            break;
        yyjson_mut_obj_add_int(doc, job, "slot", i);
        uint64_t job_id = atomic_load_explicit(&g_index_jobs[i].job_id, memory_order_acquire);
        char job_id_text[32];
        snprintf(job_id_text, sizeof(job_id_text), "%llu", (unsigned long long)job_id);
        yyjson_mut_obj_add_strcpy(doc, job, "job_id", job_id_text);
        yyjson_mut_obj_add_strcpy(doc, job, "status", ss);
        yyjson_mut_obj_add_uint(doc, job, "pid",
                                cbm_proc_control_pid(&g_index_jobs[i].child_control));
        yyjson_mut_obj_add_strcpy(doc, job, "path", g_index_jobs[i].root_path);
        yyjson_mut_obj_add_strcpy(doc, job, "error", st == 3 ? g_index_jobs[i].error_msg : "");
        yyjson_mut_arr_add_val(jobs, job);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", json);
        free(json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "%s", "{\"error\":\"serialization failed\"}");
    }
    yyjson_mut_doc_free(doc);
}

static void unwatch_project(cbm_http_server_t *srv, const char *name) {
    if (srv && srv->watcher) {
        cbm_watcher_unwatch(srv->watcher, name);
    }
}

/* DELETE /api/project?name=X — deletes the .db file */
static void handle_delete_project(cbm_http_server_t *srv, cbm_http_conn_t *c,
                                  const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "name", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing name\"}");
        return;
    }

    char db_path[1024];
    cbm_http_server_project_db_path(name, db_path, sizeof(db_path));
    if (db_path[0] == '\0') {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }

    if (unlink(db_path) != 0) {
        if (errno == ENOENT) {
            unwatch_project(srv, name);
            cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
            return;
        }
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"failed to delete\"}");
        return;
    }

    /* Also remove WAL and SHM files if they exist */
    char wal_path[1040], shm_path[1040];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    (void)unlink(wal_path);
    (void)unlink(shm_path);

    unwatch_project(srv, name);
    cbm_log_info("ui.project.deleted", "name", name);
    cbm_http_replyf(c, 200, g_cors_json, "{\"deleted\":true}");
}

/* GET /api/project-health?name=X — checks db integrity */
static void handle_project_health(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char name[256] = {0};
    if (!cbm_http_query_param(req->query, "name", name, (int)sizeof(name)) || name[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing name\"}");
        return;
    }

    char db_path[1024];
    cbm_http_server_project_db_path(name, db_path, sizeof(db_path));

    if (!cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"missing\"}");
        return;
    }

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 200, g_cors_json, "{\"status\":\"corrupt\",\"reason\":\"cannot open\"}");
        return;
    }

    int node_count = cbm_store_count_nodes(store, name);
    int edge_count = cbm_store_count_edges(store, name);
    cbm_store_close(store);

    int64_t size = cbm_file_size(db_path);

    cbm_http_replyf(c, 200, g_cors_json,
                    "{\"status\":\"healthy\",\"nodes\":%d,\"edges\":%d,\"size_bytes\":%lld}",
                    node_count, edge_count, (long long)size);
}

/* ── Handle GET /api/layout ───────────────────────────────────── */

/* Find distinct target_project values from CROSS_* edges in a store.
 * Writes up to max_out project names (heap-allocated). Returns count. */
static int find_cross_repo_targets(cbm_store_t *store, const char *project, char **out,
                                   int max_out) {
    struct sqlite3 *db = cbm_store_get_db(store);
    if (!db) {
        return 0;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT DISTINCT json_extract(properties, '$.target_project') FROM edges "
            "WHERE project = ?1 AND type LIKE 'CROSS_%' "
            "AND json_extract(properties, '$.target_project') IS NOT NULL",
            -1, &s, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(s, 1, project, -1, SQLITE_STATIC);
    int count = 0;
    while (sqlite3_step(s) == SQLITE_ROW && count < max_out) {
        const char *tp = (const char *)sqlite3_column_text(s, 0);
        if (tp && tp[0]) {
            size_t len = strlen(tp);
            char *copy = malloc(len + 1);
            if (!copy)
                break;
            memcpy(copy, tp, len + 1);
            out[count] = copy;
            count++;
        }
    }
    sqlite3_finalize(s);
    return count;
}

enum { LAYOUT_MAX_LINKED = 16 };
#define LAYOUT_GALAXY_SPACING 600.0
#define LAYOUT_GALAXY_PAD 400.0

static void free_linked_targets(char **linked, int count) {
    for (int i = 0; i < count; i++) {
        free(linked[i]);
        linked[i] = NULL;
    }
}

/* Bounding-radius of a layout result: max distance from origin across all
 * nodes. Used to size galaxy spacing so satellites don't overlap the primary
 * cluster. Layouts with a 1000-node cluster have radius ~1500; the previous
 * fixed 600 spacing buried satellites inside the primary mass. */
static double layout_radius(const cbm_layout_result_t *r) {
    if (!r || r->node_count == 0)
        return 0.0;
    double max_r2 = 0.0;
    for (int i = 0; i < r->node_count; i++) {
        double x = (double)r->nodes[i].x;
        double y = (double)r->nodes[i].y;
        double z = (double)r->nodes[i].z;
        if (!isfinite(x) || !isfinite(y) || !isfinite(z))
            continue;
        double r2 = x * x + y * y + z * z;
        if (r2 > max_r2)
            max_r2 = r2;
    }
    return sqrt(max_r2);
}

/* Attach the missed-graph skeleton (#963) to the primary layout doc as
 *   "missed_graph": {"nodes":[...], "edges":[...], "offset":{x,y,z}}
 * — the file structure of files the indexer could not fully cover, laid out
 * as a satellite cluster beside the code galaxy (the UI renders it as a white
 * skeleton; clicking it re-centers the camera there). The offset sits on the
 * -Y side: linked-project satellites spread counter-clockwise from +X, so
 * this slot collides last. Returns true when a non-empty skeleton was
 * attached; no-op when the project has no missed files. */
static bool attach_missed_graph(yyjson_mut_doc *mdoc, yyjson_mut_val *mroot, cbm_store_t *store,
                                const char *project, double primary_radius) {
    char covproj[512];
    cbm_store_coverage_shadow_project(covproj, sizeof(covproj), project);
    cbm_layout_result_t *ml = cbm_layout_compute(store, covproj, CBM_LAYOUT_OVERVIEW, NULL, 0, 0);
    if (!ml) {
        return false;
    }
    if (ml->node_count == 0) {
        cbm_layout_free(ml);
        return false;
    }
    double miss_radius = layout_radius(ml);
    char *mjson = cbm_layout_to_json(ml);
    cbm_layout_free(ml);
    if (!mjson) {
        return false;
    }
    yyjson_doc *mldoc = yyjson_read(mjson, strlen(mjson), 0);
    free(mjson);
    if (!mldoc) {
        return false;
    }
    yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
    yyjson_val *mlroot = yyjson_doc_get_root(mldoc);
    yyjson_val *mn = yyjson_is_obj(mlroot) ? yyjson_obj_get(mlroot, "nodes") : NULL;
    yyjson_val *me = yyjson_is_obj(mlroot) ? yyjson_obj_get(mlroot, "edges") : NULL;
    yyjson_mut_val *mn_copy = mn ? yyjson_val_mut_copy(mdoc, mn) : NULL;
    yyjson_mut_val *me_copy = me ? yyjson_val_mut_copy(mdoc, me) : NULL;
    bool copied = entry && mn_copy && me_copy &&
                  yyjson_mut_obj_add_val(mdoc, entry, "nodes", mn_copy) &&
                  yyjson_mut_obj_add_val(mdoc, entry, "edges", me_copy);
    yyjson_doc_free(mldoc);
    if (!copied)
        return false;

    double dist = primary_radius + miss_radius + LAYOUT_GALAXY_PAD;
    if (dist < LAYOUT_GALAXY_SPACING) {
        dist = LAYOUT_GALAXY_SPACING;
    }
    yyjson_mut_val *offset = yyjson_mut_obj(mdoc);
    return offset && yyjson_mut_obj_add_real(mdoc, offset, "x", 0.0) &&
           yyjson_mut_obj_add_real(mdoc, offset, "y", -dist) &&
           yyjson_mut_obj_add_real(mdoc, offset, "z", 0.0) &&
           yyjson_mut_obj_add_val(mdoc, entry, "offset", offset) &&
           yyjson_mut_obj_add_val(mdoc, mroot, "missed_graph", entry);
}

static void handle_layout(cbm_http_conn_t *c, const cbm_http_req_t *req) {
    char project[256] = {0};
    char max_str[32] = {0};
    char graph_str[32] = {0};

    if (!cbm_http_query_param(req->query, "project", project, (int)sizeof(project)) ||
        project[0] == '\0') {
        cbm_http_replyf(c, 400, g_cors_json, "{\"error\":\"missing project parameter\"}");
        return;
    }

    int max_nodes = 0; /* 0 → layout default budget */
    if (cbm_http_query_param(req->query, "max_nodes", max_str, (int)sizeof(max_str))) {
        int v = atoi(max_str);
        if (v > 0)
            max_nodes = v;
    }

    /* graph=missed (#963): lay out the derived miss graph (shadow project
     * "<name>::missed" inside the SAME db file) instead of the code graph —
     * only files the indexer could not fully cover, as their file structure.
     * The db file still resolves from the validated base project name. */
    bool missed_graph = false;
    if (cbm_http_query_param(req->query, "graph", graph_str, (int)sizeof(graph_str))) {
        missed_graph = strcmp(graph_str, "missed") == 0;
    }
    char scoped_project[320];
    if (missed_graph) {
        cbm_store_coverage_shadow_project(scoped_project, sizeof(scoped_project), project);
    } else {
        snprintf(scoped_project, sizeof(scoped_project), "%s", project);
    }

    char db_path[1024];
    cbm_http_server_project_db_path(project, db_path, sizeof(db_path));

    if (!cbm_file_exists(db_path)) {
        cbm_http_replyf(c, 404, g_cors_json, "{\"error\":\"project not found\"}");
        return;
    }

    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"cannot open store\"}");
        return;
    }

    cbm_layout_result_t *layout =
        cbm_layout_compute(store, scoped_project, CBM_LAYOUT_OVERVIEW, NULL, 0, max_nodes);

    if (!layout) {
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"layout computation failed\"}");
        return;
    }

    /* Find linked projects from CROSS_* edges. Keep `store` open through the
     * linked-projects loop below so we can resolve target Route QNs against
     * the linked stores when populating cross_edges. */
    char *linked[LAYOUT_MAX_LINKED] = {0};
    int linked_count = find_cross_repo_targets(store, project, linked, LAYOUT_MAX_LINKED);

    /* Capture primary cluster radius before freeing the layout. */
    double primary_radius = layout_radius(layout);

    /* Build JSON: primary layout + linked_projects */
    char *primary_json = cbm_layout_to_json(layout);
    cbm_layout_free(layout);
    if (!primary_json) {
        free_linked_targets(linked, linked_count);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON serialization failed\"}");
        return;
    }

    /* Fast path: no satellites to attach. The missed skeleton only decorates
     * the CODE graph — a graph=missed request already IS the miss graph. */
    if (linked_count == 0 && missed_graph) {
        cbm_store_close(store);
        cbm_http_replyf(c, 200, g_cors_json, "%s", primary_json);
        free(primary_json);
        return;
    }

    /* Parse primary JSON and append missed_graph + linked_projects */
    yyjson_doc *pdoc = yyjson_read(primary_json, strlen(primary_json), 0);
    free(primary_json);
    if (!pdoc) {
        free_linked_targets(linked, linked_count);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON parse failed\"}");
        return;
    }

    yyjson_mut_doc *mdoc = yyjson_doc_mut_copy(pdoc, NULL);
    yyjson_doc_free(pdoc);
    yyjson_mut_val *mroot = mdoc ? yyjson_mut_doc_get_root(mdoc) : NULL;
    if (!mdoc || !mroot || !yyjson_mut_is_obj(mroot)) {
        if (mdoc)
            yyjson_mut_doc_free(mdoc);
        free_linked_targets(linked, linked_count);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON copy failed\"}");
        return;
    }

    if (!missed_graph) {
        (void)attach_missed_graph(mdoc, mroot, store, project, primary_radius);
    }

    yyjson_mut_val *lp_arr = yyjson_mut_arr(mdoc);
    if (!lp_arr) {
        yyjson_mut_doc_free(mdoc);
        free_linked_targets(linked, linked_count);
        cbm_store_close(store);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON allocation failed\"}");
        return;
    }

    for (int li = 0; li < linked_count; li++) {
        char lp_path[1024];
        cbm_http_server_project_db_path(linked[li], lp_path, sizeof(lp_path));
        if (!cbm_file_exists(lp_path)) {
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        cbm_store_t *lp_store = cbm_store_open_path(lp_path);
        if (!lp_store) {
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        /* Keep lp_store open through cross_edges resolution below. */
        cbm_layout_result_t *lp_layout =
            cbm_layout_compute(lp_store, linked[li], CBM_LAYOUT_OVERVIEW, NULL, 0, max_nodes);

        if (!lp_layout) {
            cbm_store_close(lp_store);
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        double sat_radius = layout_radius(lp_layout);
        char *lp_json = cbm_layout_to_json(lp_layout);
        cbm_layout_free(lp_layout);
        if (!lp_json) {
            cbm_store_close(lp_store);
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        /* Parse linked project layout */
        yyjson_doc *lpdoc = yyjson_read(lp_json, strlen(lp_json), 0);
        free(lp_json);
        if (!lpdoc) {
            cbm_store_close(lp_store);
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        yyjson_mut_doc *lm = yyjson_doc_mut_copy(lpdoc, NULL);
        yyjson_doc_free(lpdoc);
        yyjson_mut_val *lmroot = lm ? yyjson_mut_doc_get_root(lm) : NULL;
        if (!lm || !lmroot || !yyjson_mut_is_obj(lmroot)) {
            if (lm)
                yyjson_mut_doc_free(lm);
            cbm_store_close(lp_store);
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }

        /* Build linked project entry */
        yyjson_mut_val *entry = yyjson_mut_obj(mdoc);
        if (!entry) {
            yyjson_mut_doc_free(lm);
            cbm_store_close(lp_store);
            free(linked[li]);
            linked[li] = NULL;
            continue;
        }
        bool entry_ok = yyjson_mut_obj_add_strcpy(mdoc, entry, "project", linked[li]);

        /* Copy nodes and edges from linked layout */
        yyjson_mut_val *ln = yyjson_mut_obj_get(lmroot, "nodes");
        yyjson_mut_val *le = yyjson_mut_obj_get(lmroot, "edges");
        yyjson_mut_val *ln_copy = ln ? yyjson_mut_val_mut_copy(mdoc, ln) : NULL;
        yyjson_mut_val *le_copy = le ? yyjson_mut_val_mut_copy(mdoc, le) : NULL;
        entry_ok = entry_ok && ln_copy && le_copy &&
                   yyjson_mut_obj_add_val(mdoc, entry, "nodes", ln_copy) &&
                   yyjson_mut_obj_add_val(mdoc, entry, "edges", le_copy);

        /* Compute galaxy offset: evenly spaced around primary, far enough out
         * that the primary cluster (radius primary_radius) and the satellite
         * cluster (radius sat_radius) don't overlap. Bounded below by
         * LAYOUT_GALAXY_SPACING for trivially small projects. */
        double angle = (2.0 * 3.14159265358979) * (double)li / (double)linked_count;
        double dist = primary_radius + sat_radius + LAYOUT_GALAXY_PAD;
        if (dist < LAYOUT_GALAXY_SPACING) {
            dist = LAYOUT_GALAXY_SPACING;
        }
        yyjson_mut_val *offset = yyjson_mut_obj(mdoc);
        entry_ok = entry_ok && offset &&
                   yyjson_mut_obj_add_real(mdoc, offset, "x", cos(angle) * dist) &&
                   yyjson_mut_obj_add_real(mdoc, offset, "y", sin(angle) * dist) &&
                   yyjson_mut_obj_add_real(mdoc, offset, "z", 0.0) &&
                   yyjson_mut_obj_add_val(mdoc, entry, "offset", offset);

        /* Populate cross_edges connecting primary→this linked galaxy. Each
         * entry: {source: <primary node id>, target: <linked node id>, type}.
         *
         * A CROSS_* edge in the source store points caller_id → local_route_id
         * (a Route node in the source store). The Route's qualified_name is
         * canonical and the same Route exists in the linked store too — that's
         * the cross-repo matching contract. Join edges → nodes in source to
         * pull the QN, then look it up in the linked store. */
        yyjson_mut_val *cross_arr = yyjson_mut_arr(mdoc);
        entry_ok = entry_ok && cross_arr;
        struct sqlite3 *src_db = cbm_store_get_db(store);
        struct sqlite3 *lp_db = cbm_store_get_db(lp_store);
        if (entry_ok && src_db && lp_db) {
            sqlite3_stmt *eq = NULL;
            if (sqlite3_prepare_v2(src_db,
                                   "SELECT e.source_id, e.type, n.qualified_name "
                                   "FROM edges e JOIN nodes n "
                                   "  ON n.id = e.target_id AND n.project = e.project "
                                   "WHERE e.project = ?1 AND e.type LIKE 'CROSS_%' "
                                   "  AND json_extract(e.properties, '$.target_project') = ?2 "
                                   "  AND n.qualified_name IS NOT NULL",
                                   -1, &eq, NULL) == SQLITE_OK) {
                sqlite3_bind_text(eq, 1, project, -1, SQLITE_STATIC);
                sqlite3_bind_text(eq, 2, linked[li], -1, SQLITE_STATIC);

                sqlite3_stmt *lookup = NULL;
                sqlite3_prepare_v2(lp_db, "SELECT id FROM nodes WHERE qualified_name = ?1 LIMIT 1",
                                   -1, &lookup, NULL);

                while (sqlite3_step(eq) == SQLITE_ROW) {
                    int64_t src_id = sqlite3_column_int64(eq, 0);
                    const char *etype = (const char *)sqlite3_column_text(eq, 1);
                    const char *qn = (const char *)sqlite3_column_text(eq, 2);
                    if (!qn || !etype || !lookup) {
                        continue;
                    }
                    sqlite3_reset(lookup);
                    sqlite3_clear_bindings(lookup);
                    sqlite3_bind_text(lookup, 1, qn, -1, SQLITE_STATIC);
                    if (sqlite3_step(lookup) != SQLITE_ROW) {
                        continue;
                    }
                    int64_t tgt_id = sqlite3_column_int64(lookup, 0);
                    yyjson_mut_val *ce = yyjson_mut_obj(mdoc);
                    if (!ce || !yyjson_mut_obj_add_int(mdoc, ce, "source", src_id) ||
                        !yyjson_mut_obj_add_int(mdoc, ce, "target", tgt_id) ||
                        !yyjson_mut_obj_add_strcpy(mdoc, ce, "type", etype) ||
                        !yyjson_mut_arr_append(cross_arr, ce)) {
                        entry_ok = false;
                        break;
                    }
                }
                if (lookup)
                    sqlite3_finalize(lookup);
                sqlite3_finalize(eq);
            }
        }
        entry_ok = entry_ok && yyjson_mut_obj_add_val(mdoc, entry, "cross_edges", cross_arr);

        cbm_store_close(lp_store);
        if (entry_ok)
            (void)yyjson_mut_arr_append(lp_arr, entry);
        yyjson_mut_doc_free(lm);
        free(linked[li]);
        linked[li] = NULL;
    }

    free_linked_targets(linked, linked_count);
    cbm_store_close(store);
    if (!yyjson_mut_obj_add_val(mdoc, mroot, "linked_projects", lp_arr)) {
        yyjson_mut_doc_free(mdoc);
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON allocation failed\"}");
        return;
    }

    size_t len = 0;
    char *final_json = yyjson_mut_write(mdoc, 0, &len);
    yyjson_mut_doc_free(mdoc);

    if (final_json) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", final_json);
        free(final_json);
    } else {
        cbm_http_replyf(c, 500, g_cors_json, "{\"error\":\"JSON write failed\"}");
    }
}

/* ── Handle JSON-RPC request ──────────────────────────────────── */

static void handle_rpc(cbm_http_conn_t *c, const cbm_http_req_t *req, cbm_mcp_server_t *mcp) {
    if (req->body_len == 0 || req->body_len > MAX_BODY_SIZE || !req->body) {
        cbm_http_replyf(c, 400, g_cors_json,
                        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
                        "\"message\":\"invalid request size\"},\"id\":null}");
        return;
    }

    /* req->body is NUL-terminated by the transport */
    char *response = cbm_mcp_server_handle(mcp, req->body);

    if (response) {
        cbm_http_replyf(c, 200, g_cors_json, "%s", response);
        free(response);
    } else {
        cbm_http_replyf(c, 204, g_cors, "%s", "");
    }
}

/* ── Request dispatch ─────────────────────────────────────────── */

/* True when the Host header names the loopback interface the server binds to
 * (with or without a port). Anything else means the request reached us under a
 * name that is not loopback — a rebinding DNS host or a proxy pointed at the
 * local port — which is the DNS-rebinding / cross-site vector against a
 * localhost-only service. */
static void dispatch_request(cbm_http_server_t *srv, cbm_http_conn_t *c,
                             const cbm_http_req_t *req) {
    /* Reflect only the exact origin serving this request. Another service on a
     * different loopback port is cross-origin and receives no authority. */
    update_cors(req, srv->port);

    /* DNS-rebinding / cross-site guard: the server binds to loopback only, so a
     * request carrying any non-loopback Host was routed here under a foreign
     * name (a rebinding DNS record, a proxy) and must be refused before it can
     * reach a state-changing endpoint. A bare request with no Host header
     * (HTTP/1.0 local tooling) is still allowed. */
    if (req->host[0] != '\0' && !host_matches_listener(req->host, srv->port)) {
        cbm_http_replyf(c, 403, g_cors_json, "%s", "{\"error\":\"forbidden host\"}");
        return;
    }

    /* CORS governs whether browser JavaScript may read a response; it does not
     * stop a cross-site form/fetch from sending the request. Refuse every
     * explicit foreign Origin before routing, including OPTIONS probes. */
    if (req->origin[0] != '\0' && !origin_is_same_server(req->origin, req->host, srv->port)) {
        cbm_http_replyf(c, 403, g_cors_json, "%s", "{\"error\":\"forbidden origin\"}");
        return;
    }

    bool is_get = strcmp(req->method, "GET") == 0;
    bool is_post = strcmp(req->method, "POST") == 0;
    bool is_delete = strcmp(req->method, "DELETE") == 0;

    /* OPTIONS preflight for CORS */
    if (strcmp(req->method, "OPTIONS") == 0) {
        cbm_http_replyf(c, 204, g_cors, "%s", "");
        return;
    }

    /* All POST routes in this server consume JSON. Requiring the non-simple
     * media type prevents a cross-site HTML form or sendBeacon from mutating
     * localhost when an older client omits Origin. */
    if (is_post && !content_type_is_json(req->content_type)) {
        cbm_http_replyf(c, 415, g_cors_json, "%s", "{\"error\":\"application/json required\"}");
        return;
    }

    /* POST /rpc → JSON-RPC dispatch (reuses existing MCP tools) */
    if (is_post && cbm_http_path_match(req->path, "/rpc")) {
        handle_rpc(c, req, srv->mcp);
        return;
    }

    /* GET /api/layout → 3D graph layout */
    if (is_get && cbm_http_path_match(req->path, "/api/layout*")) {
        handle_layout(c, req);
        return;
    }

    /* GET /api/repo-info → git remote / branch for GitHub deep-links */
    if (is_get && cbm_http_path_match(req->path, "/api/repo-info*")) {
        handle_repo_info(c, req);
        return;
    }

    /* POST /api/index → start background indexing */
    if (is_post && cbm_http_path_match(req->path, "/api/index")) {
        handle_index_start(c, req);
        return;
    }

    /* GET /api/index-status → check indexing progress */
    if (is_get && cbm_http_path_match(req->path, "/api/index-status")) {
        handle_index_status(c);
        return;
    }

    /* GET /api/ui-config → language and local UI preferences */
    if (is_get && cbm_http_path_match(req->path, "/api/ui-config")) {
        handle_ui_config(c, req);
        return;
    }

    /* DELETE /api/project → delete a project's .db file */
    if (is_delete && cbm_http_path_match(req->path, "/api/project*")) {
        handle_delete_project(srv, c, req);
        return;
    }

    /* GET /api/browse → directory browser for file picker */
    if (is_get && cbm_http_path_match(req->path, "/api/browse*")) {
        handle_browse(c, req);
        return;
    }

    /* GET /api/adr → get ADR for project */
    if (is_get && cbm_http_path_match(req->path, "/api/adr*")) {
        handle_adr_get(c, req);
        return;
    }

    /* POST /api/adr → save ADR for project */
    if (is_post && cbm_http_path_match(req->path, "/api/adr")) {
        handle_adr_save(c, req);
        return;
    }

    /* GET /api/project-health → check db integrity */
    if (is_get && cbm_http_path_match(req->path, "/api/project-health*")) {
        handle_project_health(c, req);
        return;
    }

    /* GET /api/processes → list running codebase-memory-mcp processes */
    if (is_get && cbm_http_path_match(req->path, "/api/processes")) {
        handle_processes(c);
        return;
    }

    /* GET /api/logs → recent log lines */
    if (is_get && cbm_http_path_match(req->path, "/api/logs*")) {
        handle_logs(c, req);
        return;
    }

    /* POST /api/process-kill → kill a process */
    if (is_post && cbm_http_path_match(req->path, "/api/process-kill")) {
        handle_process_kill(c, req);
        return;
    }

    /* GET / → index.html (no-cache so browser always gets latest) */
    if (cbm_http_path_match(req->path, "/")) {
        const cbm_embedded_file_t *f = cbm_embedded_lookup("/index.html");
        if (f) {
            char html_hdrs[1024];
            snprintf(html_hdrs, sizeof(html_hdrs),
                     "%sContent-Type: text/html\r\nCache-Control: no-cache\r\n" CBM_UI_CSP, g_cors);
            cbm_http_reply_buf(c, 200, html_hdrs, f->data, (size_t)f->size);
            return;
        }
        cbm_http_replyf(c, 404, g_cors, "no frontend embedded");
        return;
    }

    /* GET /assets/... → embedded assets, then generic embedded fallback */
    if (serve_embedded(c, req->path))
        return;

    cbm_http_replyf(c, 404, g_cors, "not found");
}

/* ── Public API ───────────────────────────────────────────────── */

cbm_http_server_t *cbm_http_server_new(int port) {
    cbm_http_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;

    srv->port = port;
    atomic_store(&srv->stop_flag, 0);

    /* Create a dedicated MCP server for HTTP (own SQLite connection) */
    srv->mcp = cbm_mcp_server_new(NULL);
    if (!srv->mcp) {
        cbm_log_error("ui.http.mcp_fail", "reason", "cannot create MCP instance");
        free(srv);
        return NULL;
    }

    /* Bind to localhost only (httpd refuses anything else by construction) */
    srv->listener = cbm_httpd_listen(port);
    if (!srv->listener) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        cbm_log_warn("ui.unavailable", "port", port_str, "reason", "in_use", "hint",
                     "use --port=N to override");
        cbm_mcp_server_free(srv->mcp);
        free(srv);
        return NULL;
    }

    srv->port = cbm_httpd_port(srv->listener);
    srv->listener_ok = true;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", srv->port);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", srv->port);
    cbm_log_info("ui.serving", "url", url, "port", port_str);

    return srv;
}

void cbm_http_server_free(cbm_http_server_t *srv) {
    if (!srv)
        return;
    cbm_httpd_close(srv->listener);
    cbm_mcp_server_free(srv->mcp);
    free(srv);
}

void cbm_http_server_stop(cbm_http_server_t *srv) {
    if (srv) {
        atomic_store(&srv->stop_flag, 1);
    }
}

void cbm_http_server_run(cbm_http_server_t *srv) {
    if (!srv || !srv->listener_ok)
        return;

    while (!atomic_load(&srv->stop_flag)) {
        cbm_http_conn_t *conn = cbm_httpd_accept(srv->listener, 200);
        if (!conn)
            continue; /* timeout — re-check stop flag */

        uint64_t request_start_ms = cbm_now_ms();
        cbm_http_req_t req;
        int rc = cbm_httpd_read_request(conn, &req);
        if (rc == 0) {
            dispatch_request(srv, conn, &req);
            cbm_log_http_request("graph_ui", req.method, req.path, cbm_http_conn_status(conn),
                                 (int64_t)(cbm_now_ms() - request_start_ms), req.body_len,
                                 cbm_http_conn_response_bytes(conn));
            cbm_http_req_free(&req);
        } else if (rc > 0) {
            /* Parse/transport error with a known HTTP status (400/408/411/413/431).
             * No CORS reflection here — the request was never parsed. */
            cbm_http_replyf(conn, rc, "", "bad request");
            cbm_log_http_request("graph_ui", "", "", cbm_http_conn_status(conn),
                                 (int64_t)(cbm_now_ms() - request_start_ms), 0,
                                 cbm_http_conn_response_bytes(conn));
        }
        cbm_httpd_conn_close(conn);
    }
}

bool cbm_http_server_is_running(const cbm_http_server_t *srv) {
    return srv && srv->listener_ok;
}

int cbm_http_server_port(const cbm_http_server_t *srv) {
    return (srv && srv->listener_ok) ? srv->port : -1;
}

void cbm_http_server_set_recv_deadline_ms(cbm_http_server_t *srv, int ms) {
    if (srv && srv->listener_ok) {
        cbm_httpd_set_recv_deadline_ms(srv->listener, ms);
    }
}

void cbm_http_server_set_send_deadline_ms(cbm_http_server_t *srv, int ms) {
    if (srv && srv->listener_ok) {
        cbm_httpd_set_send_deadline_ms(srv->listener, ms);
    }
}

void cbm_http_server_set_watcher(cbm_http_server_t *srv, struct cbm_watcher *watcher) {
    if (srv) {
        srv->watcher = watcher;
    }
}
