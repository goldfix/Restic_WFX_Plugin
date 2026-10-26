#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <string.h>
#include <stdio.h>
#include "restic_stats.h"
#include "wfx_path.h"
#include "cJSON.h"

#define SNAPSHOT_CACHE_SLOTS 8 /* One slot per repository alias seen in the two panels */

/* ------------------------------------------------------------------------------------------ */
/* JSON helpers                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* Copies a UTF-8 string member; empty or missing members leave *out empty */
static void json_get_string(const cJSON* obj, const char* name, char* out, size_t capacity) {
    if (out == NULL || capacity == 0) return;
    out[0] = '\0';
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy_s(out, capacity, item->valuestring, _TRUNCATE);
    }
}

/*
 * Reads a counter member; missing, negative or non-numeric values leave `*out` at 0.
 * Values are saturated at RESTIC_STATS_COUNTER_MAX (LLONG_MAX): converting a double beyond the
 * range of the target type is undefined behaviour, and Total Commander only understands signed
 * 64-bit numbers anyway.
 */
static void json_get_u64(const cJSON* obj, const char* name, unsigned long long* out) {
    *out = 0;
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item) || !(item->valuedouble > 0.0)) { /* Also rejects NaN */
        return;
    }
    if (item->valuedouble >= 9223372036854775807.0) {
        *out = RESTIC_STATS_COUNTER_MAX;
        return;
    }
    *out = (unsigned long long)item->valuedouble;
}

/* TRUE for a non-empty string made only of hexadecimal digits (what a snapshot ID looks like) */
static BOOL is_hex_id(const char* text) {
    if (text == NULL || text[0] == '\0') return FALSE;
    for (const char* p = text; *p != '\0'; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Joins a JSON array of strings into `out` using `separator`; returns the element count */
static unsigned int json_join_string_array(
    const cJSON* array,
    const char* separator,
    char* out,
    size_t capacity
) {
    if (out == NULL || capacity == 0) return 0;
    out[0] = '\0';
    if (!cJSON_IsArray(array)) return 0;

    unsigned int count = 0;
    size_t used        = 0;
    const cJSON* item  = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) continue;
        ++count;
        const char* text    = item->valuestring;
        size_t text_len     = strlen(text);
        size_t sep_len      = (used > 0) ? strlen(separator) : 0;
        size_t remaining    = capacity - used - 1; /* Keep room for the NUL terminator */
        if (remaining <= sep_len) continue;        /* Buffer full: keep counting, stop appending */

        if (sep_len > 0) {
            memcpy(out + used, separator, sep_len);
            used += sep_len;
        }
        size_t copy_len = text_len;
        if (copy_len > capacity - used - 1) {
            copy_len = capacity - used - 1;
            /* Never cut in the middle of a multi-byte UTF-8 sequence: drop the partial character
               entirely, otherwise the column would end with a broken character. */
            while (copy_len > 0 && ((unsigned char)text[copy_len] & 0xC0) == 0x80) {
                --copy_len;
            }
        }
        memcpy(out + used, text, copy_len);
        used += copy_len;
        out[used] = '\0';
    }
    return count;
}

/* Difference of two FILETIMEs in whole seconds; 0 when `end` is not after `start` */
static unsigned long long filetime_diff_seconds(const FILETIME* start, const FILETIME* end) {
    ULARGE_INTEGER a;
    ULARGE_INTEGER b;
    a.LowPart   = start->dwLowDateTime;
    a.HighPart  = start->dwHighDateTime;
    b.LowPart   = end->dwLowDateTime;
    b.HighPart  = end->dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return 0;
    /* 10,000,000 * 100 ns ticks per second */
    return (unsigned long long)((b.QuadPart - a.QuadPart) / 10000000ULL);
}

/* ------------------------------------------------------------------------------------------ */
/* Snapshot record parser                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* Parses one JSON snapshot object into `*info`. Returns FALSE when the entry must be skipped. */
static BOOL parse_snapshot_object(const cJSON* snap, ResticSnapshotInfo* info) {
    memset(info, 0, sizeof(*info));

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
    /* is_hex_id(): the directory listing parser skips non-hexadecimal IDs, so this one must too
     * (both results have to stay index-aligned). */
    if (time_str == NULL || id_str == NULL || !is_hex_id(id_str)) {
        return FALSE;
    }
    if (!restic_parse_iso8601_time(time_str, &info->time)) {
        return FALSE;
    }

    strncpy_s(info->short_id, sizeof(info->short_id), id_str, _TRUNCATE);
    json_get_string(snap, "id", info->id, sizeof(info->id));
    if (info->id[0] == '\0') { /* Fall back to the short ID when the full one is absent */
        strncpy_s(info->id, sizeof(info->id), info->short_id, _TRUNCATE);
    }
    json_get_string(snap, "parent", info->parent, sizeof(info->parent));
    json_get_string(snap, "hostname", info->hostname, sizeof(info->hostname));
    json_get_string(snap, "username", info->username, sizeof(info->username));
    json_get_string(snap, "program_version", info->program_version, sizeof(info->program_version));

    info->tag_count = json_join_string_array(
        cJSON_GetObjectItemCaseSensitive(snap, "tags"), ", ", info->tags, sizeof(info->tags)
    );
    info->path_count = json_join_string_array(
        cJSON_GetObjectItemCaseSensitive(snap, "paths"), "; ", info->paths, sizeof(info->paths)
    );

    const cJSON* summary = cJSON_GetObjectItemCaseSensitive(snap, "summary");
    if (cJSON_IsObject(summary)) {
        const cJSON* j_start = cJSON_GetObjectItemCaseSensitive(summary, "backup_start");
        const cJSON* j_end   = cJSON_GetObjectItemCaseSensitive(summary, "backup_end");
        BOOL have_start      = cJSON_IsString(j_start) && j_start->valuestring != NULL &&
                               restic_parse_iso8601_time(j_start->valuestring, &info->backup_start);
        BOOL have_end        = cJSON_IsString(j_end) && j_end->valuestring != NULL &&
                               restic_parse_iso8601_time(j_end->valuestring, &info->backup_end);
        if (have_start && have_end) {
            info->duration_seconds = filetime_diff_seconds(&info->backup_start, &info->backup_end);
        }
        if (!have_start) {
            memset(&info->backup_start, 0, sizeof(info->backup_start));
        }
        if (!have_end) {
            memset(&info->backup_end, 0, sizeof(info->backup_end));
        }

        json_get_u64(summary, "files_new", &info->files_new);
        json_get_u64(summary, "files_changed", &info->files_changed);
        json_get_u64(summary, "files_unmodified", &info->files_unmodified);
        json_get_u64(summary, "dirs_new", &info->dirs_new);
        json_get_u64(summary, "dirs_changed", &info->dirs_changed);
        json_get_u64(summary, "dirs_unmodified", &info->dirs_unmodified);
        json_get_u64(summary, "data_blobs", &info->data_blobs);
        json_get_u64(summary, "tree_blobs", &info->tree_blobs);
        json_get_u64(summary, "data_added", &info->data_added);
        json_get_u64(summary, "data_added_packed", &info->data_added_packed);
        json_get_u64(summary, "total_files_processed", &info->total_files_processed);
        json_get_u64(summary, "total_bytes_processed", &info->total_bytes_processed);
        info->has_summary = TRUE;
    }

    return TRUE;
}

ResticResult restic_parse_snapshot_list(
    const char* json_text,
    ResticSnapshotInfo** out_items,
    size_t* out_count
) {
    if (json_text == NULL || out_items == NULL || out_count == NULL) {
        return RESTIC_ERR_INVALID_ARG;
    }
    *out_items = NULL;
    *out_count = 0;

    cJSON* root = cJSON_Parse(json_text);
    if (root == NULL) {
        return RESTIC_ERR_JSON_PARSE;
    }
    if (cJSON_IsNull(root)) { /* Restic prints "null" for an empty repository */
        cJSON_Delete(root);
        return RESTIC_OK;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return RESTIC_ERR_JSON_PARSE;
    }

    size_t capacity               = 0;
    size_t count                  = 0;
    ResticSnapshotInfo* items     = NULL;
    const cJSON* snap             = NULL;
    cJSON_ArrayForEach(snap, root) {
        if (!cJSON_IsObject(snap)) continue;

        ResticSnapshotInfo info;
        if (!parse_snapshot_object(snap, &info)) continue;

        if (count >= capacity) {
            size_t new_capacity = (capacity == 0) ? 8 : capacity * 2;
            ResticSnapshotInfo* grown =
                (ResticSnapshotInfo*)realloc(items, new_capacity * sizeof(ResticSnapshotInfo));
            if (grown == NULL) {
                free(items);
                cJSON_Delete(root);
                return RESTIC_ERR_OUT_OF_MEMORY;
            }
            items    = grown;
            capacity = new_capacity;
        }
        items[count++] = info;
    }

    cJSON_Delete(root);
    *out_items = items;
    *out_count = count;
    return RESTIC_OK;
}

void restic_free_snapshot_list(ResticSnapshotInfo* items) {
    free(items);
}

const ResticSnapshotInfo* restic_find_snapshot_by_short_id(
    const ResticSnapshotInfo* items,
    size_t count,
    const char* short_id
) {
    if (items == NULL || short_id == NULL || short_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (_stricmp(items[i].short_id, short_id) == 0) {
            return &items[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Per-alias cache                                                                            */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    BOOL used;
    WCHAR alias[MAX_PATH];
    WCHAR alias_acp[MAX_PATH]; /* `alias` after a trip through the ANSI code page (see below) */
    ResticSnapshotInfo* items;
    size_t count;
    ULONGLONG stamp; /* GetTickCount64() of the last store, used for LRU eviction */
} SnapshotCacheSlot;

static SnapshotCacheSlot g_Cache[SNAPSHOT_CACHE_SLOTS];
static SRWLOCK g_CacheLock = SRWLOCK_INIT;

/*
 * The content-column entry points receive ANSI paths, so a repository alias with characters
 * outside the active code page (e.g. Japanese on a Western system) reaches us as "??". This
 * produces the same degraded spelling for a stored alias so it can still be matched.
 */
static void alias_ansi_roundtrip(const WCHAR* alias, WCHAR* out, size_t capacity) {
    char ansi[MAX_PATH * 4] = {0};
    out[0]                  = L'\0';
    int bytes = WideCharToMultiByte(CP_ACP, 0, alias, -1, ansi, (int)sizeof(ansi), NULL, NULL);
    if (bytes <= 0 || MultiByteToWideChar(CP_ACP, 0, ansi, -1, out, (int)capacity) <= 0) {
        wcsncpy_s(out, capacity, alias, _TRUNCATE);
    }
}

/*
 * Finds the slot of `alias` (case-insensitive). Caller must hold the lock.
 * With `allow_ansi_alias` a lookup may also match the ANSI-degraded spelling of an alias, but only
 * when exactly one slot has that spelling: if two aliases degrade to the same text the answer is
 * "not found", because showing the statistics of the wrong repository would be worse than none.
 */
static SnapshotCacheSlot* cache_find_slot(const WCHAR* alias, BOOL allow_ansi_alias) {
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        if (g_Cache[i].used && _wcsicmp(g_Cache[i].alias, alias) == 0) {
            return &g_Cache[i];
        }
    }
    if (!allow_ansi_alias) return NULL;

    SnapshotCacheSlot* match = NULL;
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        if (g_Cache[i].used && _wcsicmp(g_Cache[i].alias_acp, alias) == 0) {
            if (match != NULL) return NULL; /* Ambiguous */
            match = &g_Cache[i];
        }
    }
    return match;
}

/* Finds a free slot, recycling the least recently stored one. Caller must hold the lock. */
static SnapshotCacheSlot* cache_alloc_slot(void) {
    SnapshotCacheSlot* oldest = &g_Cache[0];
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        if (!g_Cache[i].used) {
            return &g_Cache[i];
        }
        if (g_Cache[i].stamp < oldest->stamp) {
            oldest = &g_Cache[i];
        }
    }
    free(oldest->items);
    oldest->items = NULL;
    oldest->count = 0;
    return oldest;
}

BOOL snapshot_stats_store(const WCHAR* alias, const ResticSnapshotInfo* items, size_t count) {
    if (alias == NULL || alias[0] == L'\0' || (items == NULL && count > 0)) {
        return FALSE;
    }

    ResticSnapshotInfo* copy = NULL;
    if (count > 0) {
        copy = (ResticSnapshotInfo*)malloc(count * sizeof(ResticSnapshotInfo));
        if (copy == NULL) return FALSE;
        memcpy(copy, items, count * sizeof(ResticSnapshotInfo));
    }

    WCHAR alias_acp[MAX_PATH];
    alias_ansi_roundtrip(alias, alias_acp, MAX_PATH);

    AcquireSRWLockExclusive(&g_CacheLock);
    SnapshotCacheSlot* slot = cache_find_slot(alias, FALSE);
    if (slot == NULL) {
        slot = cache_alloc_slot();
        slot->used = TRUE;
        wcsncpy_s(slot->alias, MAX_PATH, alias, _TRUNCATE);
    }
    wcsncpy_s(slot->alias_acp, MAX_PATH, alias_acp, _TRUNCATE);
    free(slot->items);
    slot->items = copy;
    slot->count = count;
    slot->stamp = GetTickCount64();
    ReleaseSRWLockExclusive(&g_CacheLock);
    return TRUE;
}

BOOL snapshot_stats_lookup(const WCHAR* alias, const char* short_id, ResticSnapshotInfo* out_info) {
    if (alias == NULL || short_id == NULL || out_info == NULL) return FALSE;

    BOOL found = FALSE;
    AcquireSRWLockShared(&g_CacheLock);
    const SnapshotCacheSlot* slot = cache_find_slot(alias, TRUE);
    if (slot != NULL) {
        const ResticSnapshotInfo* info = restic_find_snapshot_by_short_id(slot->items, slot->count, short_id);
        if (info != NULL) {
            *out_info = *info;
            found     = TRUE;
        }
    }
    ReleaseSRWLockShared(&g_CacheLock);
    return found;
}

BOOL snapshot_stats_lookup_alias(
    const WCHAR* alias,
    ResticSnapshotInfo* out_items,
    size_t capacity,
    size_t* out_count
) {
    if (alias == NULL || out_count == NULL || (out_items == NULL && capacity > 0)) return FALSE;
    *out_count = 0;

    AcquireSRWLockShared(&g_CacheLock);
    const SnapshotCacheSlot* slot = cache_find_slot(alias, TRUE);
    if (slot != NULL) {
        *out_count = slot->count;
        size_t copy_count = (slot->count < capacity) ? slot->count : capacity;
        if (copy_count > 0) {
            memcpy(out_items, slot->items, copy_count * sizeof(ResticSnapshotInfo));
        }
    }
    ReleaseSRWLockShared(&g_CacheLock);
    return slot != NULL;
}

BOOL snapshot_stats_lookup_any(const char* short_id, ResticSnapshotInfo* out_info) {
    if (short_id == NULL || short_id[0] == '\0' || out_info == NULL) return FALSE;

    BOOL found = FALSE;
    ResticSnapshotInfo match;
    memset(&match, 0, sizeof(match));

    AcquireSRWLockShared(&g_CacheLock);
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        const SnapshotCacheSlot* slot = &g_Cache[i];
        if (!slot->used || slot->count == 0) continue;
        const ResticSnapshotInfo* info =
            restic_find_snapshot_by_short_id(slot->items, slot->count, short_id);
        if (info == NULL) continue;
        /* The same snapshot cached under two aliases is harmless (same ID, same data) */
        if (found && strcmp(match.id, info->id) != 0) {
            found = FALSE;
            break;
        }
        match = *info;
        found = TRUE;
    }
    ReleaseSRWLockShared(&g_CacheLock);

    if (found) *out_info = match;
    return found;
}

size_t snapshot_stats_describe(char* out, size_t capacity) {
    if (out == NULL || capacity == 0) return 0;
    out[0] = '\0';

    size_t used   = 0;
    size_t length = 0;
    AcquireSRWLockShared(&g_CacheLock);
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        if (g_Cache[i].used) {
            ++used;
        }
    }
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS && length + 1 < capacity; ++i) {
        if (!g_Cache[i].used) continue;

        /* Aliases are Unicode and the summary is written to a file, so the alias is converted to
           UTF-8: a narrow %ls conversion fails in the "C" locale for any non-ASCII character and
           would truncate the rest of the diagnostic line. */
        char alias_utf8[MAX_PATH * 3 + 1] = {0};
        if (WideCharToMultiByte(CP_UTF8, 0, g_Cache[i].alias, -1, alias_utf8, sizeof(alias_utf8),
                                NULL, NULL) <= 0) {
            strncpy_s(alias_utf8, sizeof(alias_utf8), "?", _TRUNCATE);
        }

        int written = _snprintf_s(
            out + length,
            capacity - length,
            _TRUNCATE,
            "%s(%zu) ",
            alias_utf8,
            g_Cache[i].count
        );
        if (written <= 0) break;
        length += (size_t)written;
    }
    ReleaseSRWLockShared(&g_CacheLock);
    return used;
}

void snapshot_stats_invalidate(const WCHAR* alias) {
    if (alias == NULL) return;

    AcquireSRWLockExclusive(&g_CacheLock);
    SnapshotCacheSlot* slot = cache_find_slot(alias, FALSE);
    if (slot != NULL) {
        free(slot->items);
        memset(slot, 0, sizeof(*slot));
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}

void snapshot_stats_clear(void) {
    AcquireSRWLockExclusive(&g_CacheLock);
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        free(g_Cache[i].items);
        memset(&g_Cache[i], 0, sizeof(g_Cache[i]));
    }
    ReleaseSRWLockExclusive(&g_CacheLock);
}

size_t snapshot_stats_cached_aliases(void) {
    size_t used = 0;
    AcquireSRWLockShared(&g_CacheLock);
    for (size_t i = 0; i < SNAPSHOT_CACHE_SLOTS; ++i) {
        if (g_Cache[i].used) ++used;
    }
    ReleaseSRWLockShared(&g_CacheLock);
    return used;
}
