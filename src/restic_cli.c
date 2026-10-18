#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "restic_cli.h"
#include "fsplugin.h"
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "cJSON.h"

/* 100-nanosecond FILETIME ticks per minute (timezone offset conversion) */
#define FILETIME_TICKS_PER_MINUTE 600000000ULL
/* Maximum accepted WFX path length in UTF-16 units (Total Commander allows 1023) */
#define MAX_WFX_PATH 1024
/* Worst-case UTF-8 size of a MAX_WFX_PATH UTF-16 path, plus NUL */
#define MAX_UTF8_PATH (MAX_WFX_PATH * 3 + 1)
/* CreateProcessW command line limit in UTF-16 units */
#define MAX_CMDLINE_CHARS 32767
/* Poll interval for the cancellation callback while Restic is running */
#define CANCEL_POLL_MS 100

/* ------------------------------------------------------------------------------------------ */
/* Small helpers                                                                              */
/* ------------------------------------------------------------------------------------------ */

static BOOL is_ascii_digit(char c) {
    return (c >= '0' && c <= '9');
}

static BOOL is_hex_string(const char* s) {
    if (!s || s[0] == '\0') return FALSE;
    for (; *s != '\0'; ++s) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Converts UTF-8 to UTF-16, truncating (never failing) when dst is too small */
static void utf8_to_wide_trunc(const char* src, WCHAR* dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = L'\0';
    if (!src) return;

    size_t len = strlen(src);
    if (len > cap - 1) len = cap - 1;
    int n = MultiByteToWideChar(CP_UTF8, 0, src, (int)len, dst, (int)(cap - 1));
    dst[n > 0 ? n : 0] = L'\0';
}

/* ------------------------------------------------------------------------------------------ */
/* Time, naming and path helpers                                                              */
/* ------------------------------------------------------------------------------------------ */

static int parse_fixed_digits(const char* s, size_t count) {
    int val = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!is_ascii_digit(s[i])) return -1;
        val = (val * 10) + (s[i] - '0');
    }
    return val;
}

BOOL restic_parse_iso8601_time(const char* iso_str, FILETIME* out_ft) {
    if (!iso_str || !out_ft) return FALSE;
    memset(out_ft, 0, sizeof(FILETIME));

    if (strlen(iso_str) < 19) return FALSE;
    if (iso_str[4] != '-' || iso_str[7] != '-' ||
        (iso_str[10] != 'T' && iso_str[10] != 't' && iso_str[10] != ' ') ||
        iso_str[13] != ':' || iso_str[16] != ':') {
        return FALSE;
    }

    int year   = parse_fixed_digits(iso_str + 0, 4);
    int month  = parse_fixed_digits(iso_str + 5, 2);
    int day    = parse_fixed_digits(iso_str + 8, 2);
    int hour   = parse_fixed_digits(iso_str + 11, 2);
    int minute = parse_fixed_digits(iso_str + 14, 2);
    int second = parse_fixed_digits(iso_str + 17, 2);

    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return FALSE;
    }

    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear   = (WORD)year;
    st.wMonth  = (WORD)month;
    st.wDay    = (WORD)day;
    st.wHour   = (WORD)hour;
    st.wMinute = (WORD)minute;
    st.wSecond = (WORD)(second == 60 ? 59 : second); /* Leap second -> last valid second */

    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) { /* Also rejects e.g. February 30 */
        return FALSE;
    }

    /* Optional fractional seconds, kept up to 100 ns precision (7 digits) */
    const char* p = iso_str + 19;
    unsigned long long frac_ticks = 0;
    if (*p == '.') {
        ++p;
        unsigned long long multiplier = 1000000ULL;
        while (is_ascii_digit(*p)) {
            if (multiplier > 0) {
                frac_ticks += (unsigned long long)(*p - '0') * multiplier;
                multiplier /= 10ULL;
            }
            ++p;
        }
    }

    /* Optional timezone designator: 'Z', '+HH:MM', '-HH:MM', '+HHMM', '-HHMM', '+HH' */
    long long tz_offset_minutes = 0;
    if (*p == '+' || *p == '-') {
        int sign = (*p == '+') ? 1 : -1;
        ++p;
        int tz_h = parse_fixed_digits(p, 2);
        if (tz_h < 0 || tz_h > 23) return FALSE;
        p += 2;
        if (*p == ':') ++p;
        int tz_m = 0;
        if (is_ascii_digit(p[0]) && is_ascii_digit(p[1])) {
            tz_m = parse_fixed_digits(p, 2);
            if (tz_m < 0 || tz_m > 59) return FALSE;
        }
        tz_offset_minutes = (long long)sign * ((long long)tz_h * 60LL + (long long)tz_m);
    }

    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += frac_ticks;

    /* Local time with offset -> UTC: UTC = local - offset */
    if (tz_offset_minutes > 0) {
        unsigned long long delta = (unsigned long long)tz_offset_minutes * FILETIME_TICKS_PER_MINUTE;
        if (uli.QuadPart < delta) return FALSE; /* Would precede the FILETIME epoch */
        uli.QuadPart -= delta;
    } else if (tz_offset_minutes < 0) {
        uli.QuadPart += (unsigned long long)(-tz_offset_minutes) * FILETIME_TICKS_PER_MINUTE;
    }

    out_ft->dwLowDateTime  = uli.LowPart;
    out_ft->dwHighDateTime = uli.HighPart;
    return TRUE;
}

BOOL restic_format_snapshot_dirname(
    const char* iso_time,
    const char* snapshot_id,
    WCHAR* out_dirname,
    size_t out_capacity
) {
    if (!iso_time || !snapshot_id || !out_dirname || out_capacity == 0) {
        return FALSE;
    }
    out_dirname[0] = L'\0';

    FILETIME ignored;
    if (!restic_parse_iso8601_time(iso_time, &ignored) || !is_hex_string(snapshot_id)) {
        return FALSE;
    }

    char short_id[9] = {0};
    strncpy_s(short_id, sizeof(short_id), snapshot_id, 8);

    char formatted[64] = {0};
    int written = snprintf(
        formatted, sizeof(formatted), "%.4s-%.2s-%.2s_%.2s-%.2s-%.2s_[%s]",
        iso_time + 0, iso_time + 5, iso_time + 8,
        iso_time + 11, iso_time + 14, iso_time + 17, short_id
    );
    if (written <= 0 || (size_t)written >= sizeof(formatted)) {
        return FALSE;
    }

    return MultiByteToWideChar(CP_UTF8, 0, formatted, -1, out_dirname, (int)out_capacity) > 0;
}

BOOL restic_extract_snapshot_id(
    const WCHAR* snapshot_dirname,
    char* out_id,
    size_t out_capacity
) {
    if (!snapshot_dirname || !out_id || out_capacity < 2) {
        return FALSE;
    }
    out_id[0] = '\0';

    const WCHAR* open_bracket  = wcsrchr(snapshot_dirname, L'[');
    const WCHAR* close_bracket = wcsrchr(snapshot_dirname, L']');
    if (!open_bracket || !close_bracket || close_bracket <= open_bracket + 1) {
        return FALSE;
    }

    size_t len = (size_t)(close_bracket - (open_bracket + 1));
    if (len >= out_capacity) {
        return FALSE;
    }

    for (size_t i = 0; i < len; ++i) {
        WCHAR wc = open_bracket[1 + i];
        if (!((wc >= L'0' && wc <= L'9') ||
              (wc >= L'a' && wc <= L'f') ||
              (wc >= L'A' && wc <= L'F'))) {
            return FALSE;
        }
        out_id[i] = (char)wc;
    }
    out_id[len] = '\0';
    return TRUE;
}

BOOL restic_normalize_filter_path(
    const WCHAR* subpath_w,
    char* out_utf8,
    size_t out_capacity
) {
    if (!out_utf8 || out_capacity < 2) return FALSE;
    out_utf8[0] = '\0';

    if (!subpath_w || subpath_w[0] == L'\0') {
        strcpy_s(out_utf8, out_capacity, "/");
        return TRUE;
    }

    char raw_utf8[MAX_UTF8_PATH] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, subpath_w, -1, raw_utf8, (int)sizeof(raw_utf8), NULL, NULL) <= 0) {
        return FALSE;
    }

    size_t out_idx = 0;
    out_utf8[out_idx++] = '/';

    for (const char* p = raw_utf8; *p != '\0'; ++p) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && out_utf8[out_idx - 1] == '/') {
            continue; /* Collapse consecutive separators */
        }
        if (out_idx + 1 >= out_capacity) return FALSE;
        out_utf8[out_idx++] = c;
    }

    while (out_idx > 1 && out_utf8[out_idx - 1] == '/') {
        out_idx--;
    }
    out_utf8[out_idx] = '\0';
    return TRUE;
}

WCHAR* restic_quote_arg(const WCHAR* arg) {
    if (!arg) return NULL;

    /* Worst case: every character doubled (quotes/backslashes) plus 2 quotes and NUL */
    WCHAR* out = (WCHAR*)malloc((wcslen(arg) * 2 + 3) * sizeof(WCHAR));
    if (!out) return NULL;

    size_t o = 0;
    out[o++] = L'"';
    for (size_t i = 0;;) {
        size_t backslashes = 0;
        while (arg[i] == L'\\') {
            backslashes++;
            i++;
        }
        if (arg[i] == L'\0') { /* Backslashes before the closing quote must be doubled */
            for (size_t k = 0; k < backslashes * 2; ++k) out[o++] = L'\\';
            break;
        }
        if (arg[i] == L'"') { /* Backslashes before an embedded quote are doubled, quote escaped */
            for (size_t k = 0; k < backslashes * 2 + 1; ++k) out[o++] = L'\\';
        } else {
            for (size_t k = 0; k < backslashes; ++k) out[o++] = L'\\';
        }
        out[o++] = arg[i++];
    }
    out[o++] = L'"';
    out[o] = L'\0';
    return out;
}

/* ------------------------------------------------------------------------------------------ */
/* JSON / NDJSON parsing                                                                      */
/* ------------------------------------------------------------------------------------------ */

static BOOL append_find_data(
    WIN32_FIND_DATAW** items,
    size_t* count,
    size_t* capacity,
    const WIN32_FIND_DATAW* entry
) {
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 16 : (*capacity * 2);
        WIN32_FIND_DATAW* new_buf = (WIN32_FIND_DATAW*)realloc(*items, new_cap * sizeof(WIN32_FIND_DATAW));
        if (!new_buf) return FALSE;
        *items = new_buf;
        *capacity = new_cap;
    }
    (*items)[(*count)++] = *entry;
    return TRUE;
}

ResticResult restic_parse_snapshots_json(
    const char* json_text,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
) {
    if (!json_text || !out_items || !out_count) {
        return RESTIC_ERR_INVALID_ARG;
    }
    *out_items = NULL;
    *out_count = 0;

    cJSON* root = cJSON_Parse(json_text);
    if (!root) {
        return RESTIC_ERR_JSON_PARSE;
    }
    if (cJSON_IsNull(root)) { /* Older Restic versions print "null" for an empty repository */
        cJSON_Delete(root);
        return RESTIC_OK;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return RESTIC_ERR_JSON_PARSE;
    }

    size_t capacity = 0;
    cJSON* snap = NULL;
    cJSON_ArrayForEach(snap, root) {
        if (!cJSON_IsObject(snap)) continue;

        const cJSON* j_time     = cJSON_GetObjectItemCaseSensitive(snap, "time");
        const cJSON* j_short_id = cJSON_GetObjectItemCaseSensitive(snap, "short_id");
        const cJSON* j_id       = cJSON_GetObjectItemCaseSensitive(snap, "id");

        const char* time_str = cJSON_IsString(j_time) ? j_time->valuestring : NULL;
        const char* id_str   = NULL;
        if (cJSON_IsString(j_short_id) && j_short_id->valuestring[0] != '\0') {
            id_str = j_short_id->valuestring;
        } else if (cJSON_IsString(j_id) && j_id->valuestring[0] != '\0') {
            id_str = j_id->valuestring;
        }
        if (!time_str || !id_str) continue;

        WIN32_FIND_DATAW fd;
        memset(&fd, 0, sizeof(fd));
        FILETIME ft;
        if (!restic_format_snapshot_dirname(time_str, id_str, fd.cFileName, MAX_PATH) ||
            !restic_parse_iso8601_time(time_str, &ft)) {
            continue;
        }

        fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        fd.ftLastWriteTime  = ft;
        fd.ftCreationTime   = ft;
        fd.ftLastAccessTime = ft;

        if (!append_find_data(out_items, out_count, &capacity, &fd)) {
            free(*out_items);
            *out_items = NULL;
            *out_count = 0;
            cJSON_Delete(root);
            return RESTIC_ERR_OUT_OF_MEMORY;
        }
    }

    cJSON_Delete(root);
    return RESTIC_OK;
}

/* Checks whether node_path is an immediate child of filter_path */
static BOOL is_direct_child_path(const char* node_path, const char* filter_path) {
    if (!node_path || !filter_path || node_path[0] != '/') return FALSE;

    if (strcmp(filter_path, "/") == 0) {
        return node_path[1] != '\0' && strchr(node_path + 1, '/') == NULL;
    }

    size_t filter_len = strlen(filter_path);
    if (strncmp(node_path, filter_path, filter_len) != 0) {
        return FALSE;
    }
    if (node_path[filter_len] != '/' || node_path[filter_len + 1] == '\0') {
        return FALSE; /* Either filter_path itself or a sibling sharing the same prefix */
    }
    return strchr(node_path + filter_len + 1, '/') == NULL;
}

/* Fills a WIN32_FIND_DATAW from one `restic ls --json` node object. Returns FALSE to skip it. */
static BOOL node_to_find_data(const cJSON* obj, WIN32_FIND_DATAW* fd) {
    const cJSON* j_name  = cJSON_GetObjectItemCaseSensitive(obj, "name");
    const cJSON* j_type  = cJSON_GetObjectItemCaseSensitive(obj, "type");
    const cJSON* j_size  = cJSON_GetObjectItemCaseSensitive(obj, "size");
    const cJSON* j_mtime = cJSON_GetObjectItemCaseSensitive(obj, "mtime");
    const cJSON* j_ctime = cJSON_GetObjectItemCaseSensitive(obj, "ctime");
    const cJSON* j_atime = cJSON_GetObjectItemCaseSensitive(obj, "atime");

    memset(fd, 0, sizeof(*fd));
    if (!cJSON_IsString(j_name) ||
        MultiByteToWideChar(CP_UTF8, 0, j_name->valuestring, -1, fd->cFileName, MAX_PATH) <= 0) {
        return FALSE;
    }

    if (cJSON_IsString(j_type) && strcmp(j_type->valuestring, "dir") == 0) {
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    } else { /* Files, symlinks, devices... (the WFX API wants links reported as plain files) */
        fd->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        if (cJSON_IsNumber(j_size) && j_size->valuedouble >= 0.0) {
            unsigned long long sz = (unsigned long long)j_size->valuedouble;
            fd->nFileSizeLow  = (DWORD)(sz & 0xFFFFFFFFULL);
            fd->nFileSizeHigh = (DWORD)(sz >> 32);
        }
    }

    FILETIME ft;
    FS_SET_TIME_UNKNOWN(fd->ftLastWriteTime);
    if (cJSON_IsString(j_mtime) && restic_parse_iso8601_time(j_mtime->valuestring, &ft)) {
        fd->ftLastWriteTime  = ft;
        fd->ftCreationTime   = ft;
        fd->ftLastAccessTime = ft;
    }
    if (cJSON_IsString(j_ctime) && restic_parse_iso8601_time(j_ctime->valuestring, &ft)) {
        fd->ftCreationTime = ft;
    }
    if (cJSON_IsString(j_atime) && restic_parse_iso8601_time(j_atime->valuestring, &ft)) {
        fd->ftLastAccessTime = ft;
    }
    return TRUE;
}

ResticResult restic_parse_ls_ndjson(
    const char* ndjson_text,
    const char* filter_path,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count
) {
    if (!ndjson_text || !filter_path || !out_items || !out_count) {
        return RESTIC_ERR_INVALID_ARG;
    }
    *out_items = NULL;
    *out_count = 0;

    ResticResult result = RESTIC_OK;
    size_t capacity = 0;
    size_t parsed_objects = 0;
    BOOL self_found = FALSE; /* Restic emits the filtered directory node itself before its children */
    BOOL self_is_dir = FALSE;
    const char* line_start = ndjson_text;

    while (*line_start != '\0' && result == RESTIC_OK) {
        const char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        while (line_len > 0 && (line_start[line_len - 1] == '\r' || line_start[line_len - 1] == ' ' ||
                                line_start[line_len - 1] == '\t')) {
            line_len--;
        }
        while (line_len > 0 && (*line_start == ' ' || *line_start == '\t')) {
            line_start++;
            line_len--;
        }

        if (line_len > 0) {
            cJSON* obj = cJSON_ParseWithLength(line_start, line_len);
            if (!obj || !cJSON_IsObject(obj)) {
                result = RESTIC_ERR_JSON_PARSE;
            } else {
                parsed_objects++;
                const cJSON* j_msg  = cJSON_GetObjectItemCaseSensitive(obj, "message_type");
                const cJSON* j_path = cJSON_GetObjectItemCaseSensitive(obj, "path");

                if (cJSON_IsString(j_msg) && strcmp(j_msg->valuestring, "node") == 0 &&
                    cJSON_IsString(j_path)) {
                    const char* path = j_path->valuestring;
                    WIN32_FIND_DATAW fd;

                    if (strcmp(path, filter_path) == 0) {
                        const cJSON* j_type = cJSON_GetObjectItemCaseSensitive(obj, "type");
                        self_found = TRUE;
                        self_is_dir = cJSON_IsString(j_type) && strcmp(j_type->valuestring, "dir") == 0;
                    } else if (is_direct_child_path(path, filter_path) && node_to_find_data(obj, &fd)) {
                        if (!append_find_data(out_items, out_count, &capacity, &fd)) {
                            result = RESTIC_ERR_OUT_OF_MEMORY;
                        }
                    }
                }
            }
            cJSON_Delete(obj);
        }

        if (!line_end) break;
        line_start = line_end + 1;
    }

    if (result == RESTIC_OK && parsed_objects == 0 && ndjson_text[0] != '\0') {
        result = RESTIC_ERR_JSON_PARSE;
    }
    /* A missing (or non-directory) filter target yields an empty stream: report it explicitly */
    if (result == RESTIC_OK && strcmp(filter_path, "/") != 0 && (!self_found || !self_is_dir)) {
        result = RESTIC_ERR_PATH_NOT_FOUND;
    }
    if (result != RESTIC_OK) {
        free(*out_items);
        *out_items = NULL;
        *out_count = 0;
    }
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Child process execution                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Growable NUL-terminated byte buffer used to capture the child's output streams */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} ByteBuf;

static BOOL bytebuf_append(ByteBuf* b, const char* src, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > new_cap) new_cap *= 2;
        char* p = (char*)realloc(b->data, new_cap);
        if (!p) return FALSE;
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return TRUE;
}

/* Moves every byte currently available in a pipe into buf. Sets *eof once the pipe is closed. */
static BOOL drain_pipe(HANDLE pipe, ByteBuf* buf, BOOL* eof) {
    char chunk[4096];
    while (!*eof) {
        DWORD avail = 0;
        DWORD got = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            *eof = TRUE; /* ERROR_BROKEN_PIPE: writer closed */
            break;
        }
        if (avail == 0) break;
        if (!ReadFile(pipe, chunk, avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk), &got, NULL) ||
            got == 0) {
            *eof = TRUE;
            break;
        }
        if (!bytebuf_append(buf, chunk, got)) return FALSE;
    }
    return TRUE;
}

static BOOL starts_with_env_key(const WCHAR* entry, const WCHAR* prefix) {
    return _wcsnicmp(entry, prefix, wcslen(prefix)) == 0;
}

/* Inherited variables that would conflict with the credentials injected by the plugin */
static const WCHAR* const kStrippedEnvKeys[] = {
    L"RESTIC_REPOSITORY=", L"RESTIC_REPOSITORY_FILE=",
    L"RESTIC_PASSWORD=",   L"RESTIC_PASSWORD_FILE=",   L"RESTIC_PASSWORD_COMMAND="
};

/*
 * Builds a UTF-16 environment block: the parent environment (Go needs SystemRoot, APPDATA, TEMP...)
 * minus conflicting RESTIC_* credential variables, plus RESTIC_REPOSITORY and RESTIC_PASSWORD.
 * The caller must wipe the block with SecureZeroMemory before releasing it.
 */
static WCHAR* build_child_environment(const WCHAR* repo_target, const WCHAR* password, size_t* out_chars) {
    *out_chars = 0;
    LPWCH parent_env = GetEnvironmentStringsW();
    size_t parent_chars = 0;

    for (const WCHAR* p = parent_env; p && *p != L'\0'; p += wcslen(p) + 1) {
        parent_chars += wcslen(p) + 1;
    }

    size_t total_cap = parent_chars + wcslen(L"RESTIC_REPOSITORY=") + wcslen(repo_target) + 1 +
                       wcslen(L"RESTIC_PASSWORD=") + wcslen(password) + 1 + 1;
    WCHAR* block = (WCHAR*)calloc(total_cap, sizeof(WCHAR));
    if (!block) {
        if (parent_env) FreeEnvironmentStringsW(parent_env);
        return NULL;
    }

    size_t offset = 0;
    for (const WCHAR* p = parent_env; p && *p != L'\0'; p += wcslen(p) + 1) {
        BOOL stripped = FALSE;
        for (size_t i = 0; i < _countof(kStrippedEnvKeys); ++i) {
            if (starts_with_env_key(p, kStrippedEnvKeys[i])) {
                stripped = TRUE;
                break;
            }
        }
        if (!stripped) {
            size_t len = wcslen(p) + 1;
            wmemcpy(block + offset, p, len);
            offset += len;
        }
    }
    if (parent_env) FreeEnvironmentStringsW(parent_env);

    int n = swprintf_s(block + offset, total_cap - offset, L"RESTIC_REPOSITORY=%ls", repo_target);
    if (n < 0) goto fail;
    offset += (size_t)n + 1;
    n = swprintf_s(block + offset, total_cap - offset, L"RESTIC_PASSWORD=%ls", password);
    if (n < 0) goto fail;
    offset += (size_t)n + 1;

    block[offset++] = L'\0'; /* Block terminator */
    *out_chars = offset;
    return block;

fail:
    SecureZeroMemory(block, total_cap * sizeof(WCHAR));
    free(block);
    return NULL;
}

/*
 * Starts cmd_line hidden with stdin=NUL and stdout/stderr redirected to two anonymous pipes.
 * Only these three handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): other threads of the
 * host process spawning children concurrently can therefore neither keep our pipes open nor leak
 * unrelated inheritable handles into Restic.
 */
static BOOL spawn_hidden(
    WCHAR* cmd_line,
    WCHAR* env_block,
    HANDLE* out_read,
    HANDLE* err_read,
    PROCESS_INFORMATION* pi
) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    HANDLE in_nul = INVALID_HANDLE_VALUE;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    BOOL attrs_ready = FALSE;
    BOOL ok = FALSE;

    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) goto done;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    in_nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in_nul == INVALID_HANDLE_VALUE) goto done;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size); /* Fails by design: size query */
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) goto done;
    attrs_ready = TRUE;

    HANDLE inherit[3] = {in_nul, out_w, err_w};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   sizeof(inherit), NULL, NULL)) {
        goto done;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb          = sizeof(si);
    si.StartupInfo.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput   = in_nul;
    si.StartupInfo.hStdOutput  = out_w;
    si.StartupInfo.hStdError   = err_w;
    si.lpAttributeList         = attrs;

    memset(pi, 0, sizeof(*pi));
    ok = CreateProcessW(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        env_block, NULL, &si.StartupInfo, pi);

done:
    if (attrs_ready) DeleteProcThreadAttributeList(attrs);
    free(attrs);
    /* Child-side ends must be closed in the parent so EOF is observable */
    if (out_w) CloseHandle(out_w);
    if (err_w) CloseHandle(err_w);
    if (in_nul != INVALID_HANDLE_VALUE) CloseHandle(in_nul);
    if (ok) {
        *out_read = out_r;
        *err_read = err_r;
    } else {
        if (out_r) CloseHandle(out_r);
        if (err_r) CloseHandle(err_r);
    }
    return ok;
}

/*
 * Reads both pipes until the child exits (no deadlock if one stream fills up), honouring the
 * optional timeout and cancellation callback. The child is NOT terminated here.
 */
static ResticResult collect_output(
    HANDLE process,
    HANDLE out_pipe,
    HANDLE err_pipe,
    const ResticExecOptions* options,
    ByteBuf* out,
    ByteBuf* err
) {
    ULONGLONG start = GetTickCount64();
    ULONGLONG last_poll = start;
    BOOL out_eof = FALSE, err_eof = FALSE;

    for (;;) {
        size_t before = out->len + err->len;
        if (!drain_pipe(out_pipe, out, &out_eof) || !drain_pipe(err_pipe, err, &err_eof)) {
            return RESTIC_ERR_OUT_OF_MEMORY;
        }

        /* Only sleep when idle; wake immediately when the child exits */
        if (out->len + err->len == before && WaitForSingleObject(process, 20) == WAIT_OBJECT_0) {
            if (!drain_pipe(out_pipe, out, &out_eof) || !drain_pipe(err_pipe, err, &err_eof)) {
                return RESTIC_ERR_OUT_OF_MEMORY;
            }
            return RESTIC_OK;
        }

        if (options) {
            ULONGLONG now = GetTickCount64();
            if (options->timeout_ms != 0 && now - start >= options->timeout_ms) {
                return RESTIC_ERR_TIMEOUT;
            }
            if (options->cancel_proc && now - last_poll >= CANCEL_POLL_MS) {
                last_poll = now;
                if (options->cancel_proc(options->cancel_ctx)) return RESTIC_ERR_CANCELLED;
            }
        }
    }
}

/* Copies the "message" field of the first JSON line found in text. Returns TRUE if found. */
static BOOL find_json_message(const char* text, WCHAR* dst, size_t cap) {
    for (const char* line = text; line && *line != '\0';) {
        const char* next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len > 0 && *line == '{') {
            cJSON* obj = cJSON_ParseWithLength(line, len);
            if (obj) {
                const cJSON* j_msg = cJSON_GetObjectItemCaseSensitive(obj, "message");
                if (j_msg != NULL && cJSON_IsString(j_msg) && j_msg->valuestring != NULL &&
                    j_msg->valuestring[0] != '\0') {
                    utf8_to_wide_trunc(j_msg->valuestring, dst, cap);
                    cJSON_Delete(obj);
                    return TRUE;
                }
                cJSON_Delete(obj);
            }
        }
        line = next ? next + 1 : NULL;
    }
    return FALSE;
}

/* Builds a human-readable failure reason from Restic's streams (JSON message preferred) */
static void build_error_message(const ByteBuf* out, const ByteBuf* err, WCHAR* dst, size_t cap) {
    if (find_json_message(err->data, dst, cap) || find_json_message(out->data, dst, cap)) return;
    utf8_to_wide_trunc(err->len > 0 ? err->data : out->data, dst, cap);
    for (size_t n = wcslen(dst); n > 0 && (dst[n - 1] == L'\r' || dst[n - 1] == L'\n'); --n) {
        dst[n - 1] = L'\0';
    }
}

ResticResult restic_exec_command(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const WCHAR* args,
    const ResticExecOptions* options,
    ResticCommandOutput* out_result
) {
    if (!restic_exe || !repo_target || !password || !args || !out_result) {
        return RESTIC_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(ResticCommandOutput));

    /* A bare file name would make CreateProcess search the current directory: refuse it */
    if (restic_exe[0] == L'\0' || wcspbrk(restic_exe, L"\\/:") == NULL) {
        return RESTIC_ERR_SPAWN_FAILED;
    }

    size_t cmd_chars = wcslen(restic_exe) + wcslen(args) + 4; /* quotes, space, NUL */
    if (cmd_chars > MAX_CMDLINE_CHARS) {
        return RESTIC_ERR_INVALID_ARG;
    }
    WCHAR* cmd_line = (WCHAR*)malloc(cmd_chars * sizeof(WCHAR));
    if (!cmd_line) {
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    swprintf_s(cmd_line, cmd_chars, L"\"%ls\" %ls", restic_exe, args);

    size_t env_chars = 0;
    WCHAR* env_block = build_child_environment(repo_target, password, &env_chars);
    if (!env_block) {
        free(cmd_line);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }

    HANDLE out_pipe = NULL, err_pipe = NULL;
    PROCESS_INFORMATION pi;
    BOOL spawned = spawn_hidden(cmd_line, env_block, &out_pipe, &err_pipe, &pi);

    /* Credentials must not outlive the CreateProcess call in this process' memory */
    SecureZeroMemory(env_block, env_chars * sizeof(WCHAR));
    free(env_block);
    free(cmd_line);
    if (!spawned) {
        return RESTIC_ERR_SPAWN_FAILED;
    }
    CloseHandle(pi.hThread);

    ByteBuf out = {0}, err = {0};
    ResticResult result = RESTIC_ERR_OUT_OF_MEMORY;
    if (bytebuf_append(&out, "", 0) && bytebuf_append(&err, "", 0)) {
        result = collect_output(pi.hProcess, out_pipe, err_pipe, options, &out, &err);
    }

    if (result == RESTIC_OK) {
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        out_result->exit_code = exit_code;
        if (exit_code != 0) {
            build_error_message(&out, &err, out_result->error_message, _countof(out_result->error_message));
            switch (exit_code) {
                case 10: result = RESTIC_ERR_REPO_NOT_FOUND; break;
                case 11: result = RESTIC_ERR_REPO_LOCKED; break;
                case 12: result = RESTIC_ERR_WRONG_PASSWORD; break;
                default: result = RESTIC_ERR_CLI_FAILED; break;
            }
        }
    } else {
        TerminateProcess(pi.hProcess, 1); /* Timeout, cancellation or memory failure */
        WaitForSingleObject(pi.hProcess, 5000);
    }

    CloseHandle(out_pipe);
    CloseHandle(err_pipe);
    CloseHandle(pi.hProcess);

    out_result->output       = out.data;
    out_result->length       = out.len;
    out_result->error_output = err.data;
    out_result->error_length = err.len;
    return result;
}

void restic_free_command_output(ResticCommandOutput* result) {
    if (!result) return;
    free(result->output);
    free(result->error_output);
    memset(result, 0, sizeof(ResticCommandOutput));
}

/* ------------------------------------------------------------------------------------------ */
/* High-level helpers                                                                         */
/* ------------------------------------------------------------------------------------------ */

/* Runs a Restic command; on failure copies the reason into out_error_msg and frees the output */
static ResticResult run_and_check(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const WCHAR* args,
    const ResticExecOptions* options,
    ResticCommandOutput* cmd_out,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    ResticResult res = restic_exec_command(restic_exe, repo_target, password, args, options, cmd_out);
    if (res != RESTIC_OK) {
        if (out_error_msg && err_capacity > 0) {
            wcsncpy_s(out_error_msg, err_capacity, cmd_out->error_message, _TRUNCATE);
        }
        restic_free_command_output(cmd_out);
    }
    return res;
}

ResticResult restic_list_snapshots(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const ResticExecOptions* options,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    if (!restic_exe || !repo_target || !password || !out_items || !out_count) {
        return RESTIC_ERR_INVALID_ARG;
    }

    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo_target, password, L"snapshots --json --no-lock",
                                     options, &cmd_out, out_error_msg, err_capacity);
    if (res != RESTIC_OK) return res;

    res = restic_parse_snapshots_json(cmd_out.output, out_items, out_count);
    restic_free_command_output(&cmd_out);
    return res;
}

ResticResult restic_list_directory(
    const WCHAR* restic_exe,
    const WCHAR* repo_target,
    const WCHAR* password,
    const char* snapshot_id,
    const WCHAR* subpath_w,
    const ResticExecOptions* options,
    WIN32_FIND_DATAW** out_items,
    size_t* out_count,
    WCHAR* out_error_msg,
    size_t err_capacity
) {
    if (out_error_msg && err_capacity > 0) out_error_msg[0] = L'\0';
    /* The snapshot ID goes on the command line unquoted: only hexadecimal IDs are accepted */
    if (!restic_exe || !repo_target || !password || !out_items || !out_count ||
        !is_hex_string(snapshot_id) || strlen(snapshot_id) > 64) {
        return RESTIC_ERR_INVALID_ARG;
    }

    char filter_utf8[MAX_UTF8_PATH] = {0};
    WCHAR filter_w[MAX_UTF8_PATH] = {0};
    WCHAR snap_w[65] = {0};
    if (!restic_normalize_filter_path(subpath_w, filter_utf8, sizeof(filter_utf8)) ||
        MultiByteToWideChar(CP_UTF8, 0, filter_utf8, -1, filter_w, (int)_countof(filter_w)) <= 0) {
        return RESTIC_ERR_INVALID_ARG;
    }
    MultiByteToWideChar(CP_UTF8, 0, snapshot_id, -1, snap_w, (int)_countof(snap_w));

    WCHAR* quoted_filter = restic_quote_arg(filter_w);
    if (!quoted_filter) return RESTIC_ERR_OUT_OF_MEMORY;

    size_t args_chars = wcslen(L"ls --json --no-lock  ") + wcslen(snap_w) + wcslen(quoted_filter) + 1;
    WCHAR* args = (WCHAR*)malloc(args_chars * sizeof(WCHAR));
    if (!args) {
        free(quoted_filter);
        return RESTIC_ERR_OUT_OF_MEMORY;
    }
    swprintf_s(args, args_chars, L"ls --json --no-lock %ls %ls", snap_w, quoted_filter);
    free(quoted_filter);

    ResticCommandOutput cmd_out;
    ResticResult res = run_and_check(restic_exe, repo_target, password, args, options, &cmd_out,
                                     out_error_msg, err_capacity);
    free(args);
    if (res != RESTIC_OK) return res;

    res = restic_parse_ls_ndjson(cmd_out.output, filter_utf8, out_items, out_count);
    restic_free_command_output(&cmd_out);
    return res;
}
