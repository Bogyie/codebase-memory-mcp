/*
 * compat.c — Implementations for Windows-only shims.
 *
 * On POSIX, these functions are provided by the standard library via
 * macros in compat.h. On Windows, we implement them here.
 */
#include "foundation/compat.h"
#include "foundation/constants.h"

#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "foundation/win_utf8.h"

#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#endif

/* ── strndup (Windows lacks it) ───────────────────────────────── */

#ifdef _WIN32
char *cbm_strndup(const char *s, size_t n) {
    if (!s) {
        return NULL;
    }
    size_t len = 0;
    while (len < n && s[len]) {
        len++;
    }
    char *d = (char *)malloc(len + SKIP_ONE);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}
#endif

/* ── strcasestr (Windows lacks it) ────────────────────────────── */

#ifdef _WIN32
char *cbm_strcasestr(const char *haystack, const char *needle) {
    if (!needle[0])
        return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}
#endif

/* ── mkdtemp (Windows lacks it) ───────────────────────────────── */

#ifdef _WIN32
#include <direct.h>
char *cbm_mkdtemp(char *tmpl);
#endif

/* ── mkstemp (Windows lacks it) ───────────────────────────────── */

#ifdef _WIN32
int cbm_mkstemp(char *tmpl);
#endif

#ifdef _WIN32

typedef LONG(WINAPI *cbm_bcrypt_gen_random_fn)(void *, unsigned char *, ULONG, ULONG);
typedef BOOLEAN(APIENTRY *cbm_rtl_gen_random_fn)(PVOID, ULONG);

const char *cbm_tmpdir(void) {
    static CBM_TLS char result[CBM_PATH_MAX];
    result[0] = '\0';

    DWORD needed = GetTempPathW(0, NULL);
    if (needed == 0 || needed > 32768U) {
        return NULL;
    }
    wchar_t *wide = malloc(((size_t)needed + 1U) * sizeof(*wide));
    if (!wide) {
        return NULL;
    }
    DWORD written = GetTempPathW(needed + 1U, wide);
    if (written == 0 || written > needed || wide[0] == L'\0') {
        free(wide);
        return NULL;
    }
    char *utf8 = cbm_wide_to_utf8(wide);
    free(wide);
    if (!utf8) {
        return NULL;
    }
    size_t len = strlen(utf8);
    if (len == 0 || len >= sizeof(result)) {
        free(utf8);
        return NULL;
    }
    memcpy(result, utf8, len + 1U);
    free(utf8);
    for (char *p = result; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return result;
}

/* Fill from the OS CSPRNG without imposing a bcrypt link dependency.
 * SystemFunction036 is the supported legacy primitive used by rand_s. */
static bool cbm_secure_random(void *bytes, size_t len) {
    if (!bytes || len == 0 || len > (size_t)ULONG_MAX) {
        return false;
    }
    HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
    if (bcrypt) {
        cbm_bcrypt_gen_random_fn generate =
            (cbm_bcrypt_gen_random_fn)(void *)GetProcAddress(bcrypt, "BCryptGenRandom");
        bool ok = generate && generate(NULL, (unsigned char *)bytes, (ULONG)len,
                                       0x00000002UL /* system-preferred RNG */) >= 0;
        FreeLibrary(bcrypt);
        if (ok) {
            return true;
        }
    }
    HMODULE advapi = LoadLibraryW(L"advapi32.dll");
    if (!advapi) {
        return false;
    }
    cbm_rtl_gen_random_fn generate =
        (cbm_rtl_gen_random_fn)(void *)GetProcAddress(advapi, "SystemFunction036");
    bool ok = generate && generate(bytes, (ULONG)len) != FALSE;
    FreeLibrary(advapi);
    return ok;
}

static wchar_t *cbm_temp_template_wide(const char *tmpl, bool *expanded_tmp) {
    if (expanded_tmp) {
        *expanded_tmp = false;
    }
    if (!tmpl) {
        return NULL;
    }
    if (strncmp(tmpl, "/tmp/", 5) != 0) {
        return cbm_utf8_to_wide(tmpl);
    }
    DWORD need = GetTempPathW(0, NULL);
    if (need == 0 || need > 32768U) {
        return NULL;
    }
    wchar_t *temp = (wchar_t *)malloc(((size_t)need + 1U) * sizeof(*temp));
    if (!temp) {
        return NULL;
    }
    DWORD written = GetTempPathW(need + 1U, temp);
    wchar_t *tail = cbm_utf8_to_wide(tmpl + 5);
    if (written == 0 || written > need || !tail) {
        free(tail);
        free(temp);
        return NULL;
    }
    size_t temp_len = wcslen(temp);
    size_t tail_len = wcslen(tail);
    bool separator = temp_len > 0 && temp[temp_len - 1] != L'\\' && temp[temp_len - 1] != L'/';
    if (temp_len > SIZE_MAX - tail_len - (separator ? 2U : 1U)) {
        free(tail);
        free(temp);
        return NULL;
    }
    size_t joined_len = temp_len + tail_len + (separator ? 1U : 0U);
    wchar_t *joined = (wchar_t *)malloc((joined_len + 1U) * sizeof(*joined));
    if (!joined) {
        free(tail);
        free(temp);
        return NULL;
    }
    memcpy(joined, temp, temp_len * sizeof(*joined));
    size_t pos = temp_len;
    if (separator) {
        joined[pos++] = L'\\';
    }
    memcpy(joined + pos, tail, (tail_len + 1U) * sizeof(*joined));
    free(tail);
    free(temp);
    if (expanded_tmp) {
        *expanded_tmp = true;
    }
    return joined;
}

static wchar_t *cbm_temp_x_suffix(wchar_t *path) {
    if (!path) {
        return NULL;
    }
    size_t len = wcslen(path);
    if (len < 6U) {
        return NULL;
    }
    wchar_t *suffix = path + len - 6U;
    for (size_t i = 0; i < 6U; i++) {
        if (suffix[i] != L'X') {
            return NULL;
        }
    }
    return suffix;
}

static bool cbm_temp_publish_path(char *tmpl, const wchar_t *created, bool expanded_tmp) {
    char *utf8 = cbm_wide_to_utf8(created);
    if (!utf8) {
        return false;
    }
    for (char *p = utf8; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    size_t result_len = strlen(utf8);
    size_t original_len = strlen(tmpl);
    /* /tmp expansion retains the documented >=256-byte buffer contract.  All
     * other results have exactly the input length and cannot overrun tmpl. */
    bool fits = expanded_tmp ? result_len < CBM_SZ_256 : result_len == original_len;
    if (fits) {
        memcpy(tmpl, utf8, result_len + 1U);
    }
    free(utf8);
    return fits;
}

static bool cbm_temp_randomize_suffix(wchar_t suffix[6]) {
    static const wchar_t hex[] = L"0123456789abcdef";
    unsigned char random[3];
    if (!cbm_secure_random(random, sizeof(random))) {
        return false;
    }
    uint32_t value =
        ((uint32_t)random[0] << 16U) | ((uint32_t)random[1] << 8U) | (uint32_t)random[2];
    for (size_t i = 0; i < 6U; i++) {
        unsigned int shift = (unsigned int)((5U - i) * 4U);
        suffix[i] = hex[(value >> shift) & 0x0fU];
    }
    return true;
}

char *cbm_mkdtemp(char *tmpl) {
    bool expanded_tmp = false;
    wchar_t *path = cbm_temp_template_wide(tmpl, &expanded_tmp);
    wchar_t *suffix = cbm_temp_x_suffix(path);
    if (!suffix) {
        free(path);
        return NULL;
    }
    for (unsigned int attempt = 0; attempt < 256U; attempt++) {
        if (!cbm_temp_randomize_suffix(suffix)) {
            break;
        }
        if (CreateDirectoryW(path, NULL)) {
            if (cbm_temp_publish_path(tmpl, path, expanded_tmp)) {
                free(path);
                return tmpl;
            }
            (void)RemoveDirectoryW(path);
            break;
        }
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            break;
        }
    }
    free(path);
    return NULL;
}

int cbm_mkstemp(char *tmpl) {
    bool expanded_tmp = false;
    wchar_t *path = cbm_temp_template_wide(tmpl, &expanded_tmp);
    wchar_t *suffix = cbm_temp_x_suffix(path);
    if (!suffix) {
        free(path);
        return CBM_NOT_FOUND;
    }
    for (unsigned int attempt = 0; attempt < 256U; attempt++) {
        if (!cbm_temp_randomize_suffix(suffix)) {
            break;
        }
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            if (!cbm_temp_publish_path(tmpl, path, expanded_tmp)) {
                CloseHandle(file);
                (void)DeleteFileW(path);
                free(path);
                return CBM_NOT_FOUND;
            }
            int fd = _open_osfhandle((intptr_t)file, _O_RDWR | _O_BINARY);
            if (fd < 0) {
                CloseHandle(file);
                (void)DeleteFileW(path);
                free(path);
                return CBM_NOT_FOUND;
            }
            free(path);
            return fd;
        }
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            break;
        }
    }
    free(path);
    return CBM_NOT_FOUND;
}
#endif

/* ── clock_gettime (Windows lacks it) ─────────────────────────── */

#ifdef _WIN32
int cbm_clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    tp->tv_sec = (time_t)(count.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)((count.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
    return 0;
}
#endif

/* ── getline (Windows lacks it) ───────────────────────────────── */

#ifdef _WIN32
ssize_t cbm_getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) {
        return CBM_NOT_FOUND;
    }
    if (!*lineptr || *n == 0) {
        *n = CBM_SZ_128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) {
            return CBM_NOT_FOUND;
        }
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t new_n = *n * PAIR_LEN;
            char *tmp = (char *)realloc(*lineptr, new_n);
            if (!tmp) {
                return CBM_NOT_FOUND;
            }
            *lineptr = tmp;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (pos == 0 && c == EOF) {
        return CBM_NOT_FOUND;
    }
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
#endif
