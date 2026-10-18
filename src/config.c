#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "config.h"
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <wchar.h>
#include <string.h>

/* Module-level directory state initialized at plugin load time */
static WCHAR g_plugin_dir[CONFIG_MAX_PATH]   = {0};
static WCHAR g_fallback_dir[CONFIG_MAX_PATH] = {0};

/* Default configuration file template created when no INI file exists */
static const char kDefaultIniTemplate[] =
    "[Settings]\r\n"
    "; Optional path to restic.exe (if empty, checks plugin folder then system PATH)\r\n"
    "ResticPath=restic.exe\r\n"
    "\r\n"
    "[Repositories]\r\n"
    "; <Display_Alias>=<Restic_Repository_Target>\r\n"
    "; LocalBackup=D:\\Backups\\ResticRepo\r\n"
    "; LinuxServer=rest:https://backup.lan:8000/\r\n"
    "; CloudS3=s3:s3.us-east-1.amazonaws.com/company-backup/restic\r\n"
    "; NAS_SMB=\\\\nas\\share\\restic_repo\r\n";

static BOOL is_space_char(WCHAR c) {
    return (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
}

/* Trims leading and trailing whitespace in-place */
static void trim_whitespace(WCHAR* str) {
    if (!str || str[0] == L'\0') return;

    WCHAR* start = str;
    while (*start && is_space_char(*start)) {
        start++;
    }

    size_t len = wcslen(start);
    while (len > 0 && is_space_char(start[len - 1])) {
        len--;
    }

    if (start != str && len > 0) {
        wmemmove(str, start, len);
    }
    str[len] = L'\0';
}

/* Removes surrounding double or single quotes in-place */
static void strip_quotes(WCHAR* str) {
    if (!str) return;
    size_t len = wcslen(str);
    if (len >= 2) {
        WCHAR first = str[0];
        WCHAR last  = str[len - 1];
        if ((first == L'"' && last == L'"') || (first == L'\'' && last == L'\'')) {
            wmemmove(str, str + 1, len - 2);
            str[len - 2] = L'\0';
        }
    }
}

/* Removes trailing path separators while preserving drive roots (e.g. "C:\") */
static void strip_trailing_separators(WCHAR* path) {
    if (!path) return;
    size_t len = wcslen(path);
    while (len > 1 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        if (len == 3 && path[1] == L':') {
            break; /* Keep "X:\" */
        }
        path[--len] = L'\0';
    }
}

/* Extracts the parent directory of a file path */
static void extract_parent_dir(const WCHAR* file_path, WCHAR* out_dir, size_t out_capacity) {
    if (!out_dir || out_capacity == 0) return;
    out_dir[0] = L'\0';
    if (!file_path || file_path[0] == L'\0') return;

    wcsncpy_s(out_dir, out_capacity, file_path, _TRUNCATE);

    WCHAR* last_sep = NULL;
    for (WCHAR* p = out_dir; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            last_sep = p;
        }
    }

    if (!last_sep) {
        out_dir[0] = L'\0';
        return;
    }

    if (last_sep == out_dir + 2 && out_dir[1] == L':') {
        last_sep[1] = L'\0'; /* Keep root "C:\" */
    } else {
        *last_sep = L'\0';
    }
}

/* Joins a directory and leaf name with a single backslash separator */
static BOOL join_path(const WCHAR* dir, const WCHAR* leaf, WCHAR* out_path, size_t out_capacity) {
    if (!out_path || out_capacity == 0 || !leaf) return FALSE;
    out_path[0] = L'\0';

    if (!dir || dir[0] == L'\0') {
        return wcsncpy_s(out_path, out_capacity, leaf, _TRUNCATE) == 0;
    }

    size_t dir_len = wcslen(dir);
    BOOL has_sep = (dir[dir_len - 1] == L'\\' || dir[dir_len - 1] == L'/');

    if (wcsncpy_s(out_path, out_capacity, dir, _TRUNCATE) != 0) return FALSE;
    if (!has_sep) {
        if (wcscat_s(out_path, out_capacity, L"\\") != 0) return FALSE;
    }
    return wcscat_s(out_path, out_capacity, leaf) == 0;
}

/* Checks whether path points to an existing regular file */
static BOOL file_exists(const WCHAR* path) {
    if (!path || path[0] == L'\0') return FALSE;
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

/* Checks whether a string contains directory separators or a drive colon */
static BOOL has_path_component(const WCHAR* path) {
    if (!path) return FALSE;
    for (const WCHAR* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') return TRUE;
    }
    return FALSE;
}

/* Checks whether a path is relative (not rooted with "\" or "X:") */
static BOOL is_relative_path(const WCHAR* path) {
    if (!path || path[0] == L'\0') return TRUE;
    if (path[0] == L'\\' || path[0] == L'/') return FALSE;
    if (wcslen(path) >= 2 && path[1] == L':') return FALSE;
    return TRUE;
}

/* Validates that an alias is a legal single-level Windows directory name */
static BOOL is_valid_alias(const WCHAR* alias) {
    if (!alias || alias[0] == L'\0') return FALSE;
    if (wcscmp(alias, L".") == 0 || wcscmp(alias, L"..") == 0) return FALSE;

    const WCHAR* invalid_chars = L"\\/:*?\"<>|";
    for (const WCHAR* p = alias; *p != L'\0'; ++p) {
        if (*p < 32 || wcschr(invalid_chars, *p) != NULL) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Creates a default restic_plugin.ini template if it does not already exist */
static BOOL create_default_ini_file(const WCHAR* ini_path) {
    if (!ini_path || ini_path[0] == L'\0') return FALSE;
    if (file_exists(ini_path)) return TRUE;

    HANDLE hFile = CreateFileW(
        ini_path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD bytes_to_write = (DWORD)(sizeof(kDefaultIniTemplate) - 1);
    DWORD bytes_written  = 0;
    BOOL ok = WriteFile(hFile, kDefaultIniTemplate, bytes_to_write, &bytes_written, NULL);
    CloseHandle(hFile);

    return ok && (bytes_written == bytes_to_write);
}

void config_set_plugin_dir(const WCHAR* dir_path) {
    if (!dir_path) {
        g_plugin_dir[0] = L'\0';
        return;
    }
    wcsncpy_s(g_plugin_dir, CONFIG_MAX_PATH, dir_path, _TRUNCATE);
    trim_whitespace(g_plugin_dir);
    strip_quotes(g_plugin_dir);
    strip_trailing_separators(g_plugin_dir);
}

void config_set_plugin_dir_from_module(HMODULE hModule) {
    WCHAR module_path[CONFIG_MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(hModule, module_path, CONFIG_MAX_PATH);
    if (len == 0 || len >= CONFIG_MAX_PATH) {
        return;
    }
    WCHAR parent_dir[CONFIG_MAX_PATH] = {0};
    extract_parent_dir(module_path, parent_dir, CONFIG_MAX_PATH);
    config_set_plugin_dir(parent_dir);
}

const WCHAR* config_get_plugin_dir(void) {
    return g_plugin_dir;
}

void config_set_fallback_dir(const WCHAR* dir_path) {
    if (!dir_path) {
        g_fallback_dir[0] = L'\0';
        return;
    }
    wcsncpy_s(g_fallback_dir, CONFIG_MAX_PATH, dir_path, _TRUNCATE);
    trim_whitespace(g_fallback_dir);
    strip_quotes(g_fallback_dir);
    strip_trailing_separators(g_fallback_dir);
}

void config_set_fallback_from_default_params(const char* default_ini_name) {
    if (!default_ini_name || default_ini_name[0] == '\0') {
        g_fallback_dir[0] = L'\0';
        return;
    }

    WCHAR wide_ini_path[CONFIG_MAX_PATH] = {0};
    int converted = MultiByteToWideChar(
        CP_ACP,
        0,
        default_ini_name,
        -1,
        wide_ini_path,
        CONFIG_MAX_PATH
    );
    if (converted <= 0) {
        g_fallback_dir[0] = L'\0';
        return;
    }

    WCHAR parent_dir[CONFIG_MAX_PATH] = {0};
    extract_parent_dir(wide_ini_path, parent_dir, CONFIG_MAX_PATH);
    config_set_fallback_dir(parent_dir);
}

const WCHAR* config_get_fallback_dir(void) {
    return g_fallback_dir;
}

BOOL config_resolve_ini_path(WCHAR* out_path, size_t out_capacity, BOOL create_if_missing) {
    if (!out_path || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';

    WCHAR primary_candidate[CONFIG_MAX_PATH]  = {0};
    WCHAR fallback_candidate[CONFIG_MAX_PATH] = {0};

    /* 1. Primary search: <plugin_dir>\restic_plugin.ini */
    if (g_plugin_dir[0] != L'\0') {
        join_path(g_plugin_dir, CONFIG_INI_FILENAME, primary_candidate, CONFIG_MAX_PATH);
        if (file_exists(primary_candidate)) {
            wcsncpy_s(out_path, out_capacity, primary_candidate, _TRUNCATE);
            return TRUE;
        }
    }

    /* 2. Fallback search: <fallback_dir>\restic_plugin.ini */
    if (g_fallback_dir[0] != L'\0') {
        join_path(g_fallback_dir, CONFIG_INI_FILENAME, fallback_candidate, CONFIG_MAX_PATH);
        if (file_exists(fallback_candidate)) {
            wcsncpy_s(out_path, out_capacity, fallback_candidate, _TRUNCATE);
            return TRUE;
        }
        if (create_if_missing && create_default_ini_file(fallback_candidate)) {
            wcsncpy_s(out_path, out_capacity, fallback_candidate, _TRUNCATE);
            return TRUE;
        }
        wcsncpy_s(out_path, out_capacity, fallback_candidate, _TRUNCATE);
        return FALSE;
    }

    /* 3. No fallback directory set: use primary candidate and optionally create it */
    if (primary_candidate[0] != L'\0') {
        if (create_if_missing && create_default_ini_file(primary_candidate)) {
            wcsncpy_s(out_path, out_capacity, primary_candidate, _TRUNCATE);
            return TRUE;
        }
        wcsncpy_s(out_path, out_capacity, primary_candidate, _TRUNCATE);
    }
    return FALSE;
}

BOOL config_resolve_restic_path(const WCHAR* ini_path, WCHAR* out_path, size_t out_capacity) {
    if (!out_path || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';

    WCHAR configured_val[CONFIG_MAX_PATH] = {0};

    /* Read [Settings] ResticPath if an existing INI file is provided */
    if (ini_path && file_exists(ini_path)) {
        WCHAR full_ini[CONFIG_MAX_PATH] = {0};
        if (GetFullPathNameW(ini_path, CONFIG_MAX_PATH, full_ini, NULL) == 0) {
            wcsncpy_s(full_ini, CONFIG_MAX_PATH, ini_path, _TRUNCATE);
        }

        GetPrivateProfileStringW(
            L"Settings",
            L"ResticPath",
            L"",
            configured_val,
            CONFIG_MAX_PATH,
            full_ini
        );
        trim_whitespace(configured_val);
        strip_quotes(configured_val);
        trim_whitespace(configured_val);
    }

    /* 1. Explicit [Settings] ResticPath priority */
    if (configured_val[0] != L'\0' && _wcsicmp(configured_val, CONFIG_DEFAULT_RESTIC_EXE) != 0) {
        if (has_path_component(configured_val)) {
            WCHAR candidate[CONFIG_MAX_PATH] = {0};
            if (is_relative_path(configured_val) && g_plugin_dir[0] != L'\0') {
                join_path(g_plugin_dir, configured_val, candidate, CONFIG_MAX_PATH);
            } else {
                wcsncpy_s(candidate, CONFIG_MAX_PATH, configured_val, _TRUNCATE);
            }
            if (file_exists(candidate)) {
                wcsncpy_s(out_path, out_capacity, candidate, _TRUNCATE);
                return TRUE;
            }
        } else {
            /* Bare custom executable name (e.g. "restic_custom.exe") */
            if (g_plugin_dir[0] != L'\0') {
                WCHAR local_custom[CONFIG_MAX_PATH] = {0};
                join_path(g_plugin_dir, configured_val, local_custom, CONFIG_MAX_PATH);
                if (file_exists(local_custom)) {
                    wcsncpy_s(out_path, out_capacity, local_custom, _TRUNCATE);
                    return TRUE;
                }
            }
            WCHAR path_custom[CONFIG_MAX_PATH] = {0};
            DWORD found = SearchPathW(NULL, configured_val, L".exe", CONFIG_MAX_PATH, path_custom, NULL);
            if (found > 0 && found < CONFIG_MAX_PATH) {
                wcsncpy_s(out_path, out_capacity, path_custom, _TRUNCATE);
                return TRUE;
            }
        }
    }

    /* 2. Check <plugin_dir>\restic.exe */
    if (g_plugin_dir[0] != L'\0') {
        WCHAR plugin_restic[CONFIG_MAX_PATH] = {0};
        join_path(g_plugin_dir, CONFIG_DEFAULT_RESTIC_EXE, plugin_restic, CONFIG_MAX_PATH);
        if (file_exists(plugin_restic)) {
            wcsncpy_s(out_path, out_capacity, plugin_restic, _TRUNCATE);
            return TRUE;
        }
    }

    /* 3. Fallback to system PATH environment variable */
    WCHAR system_restic[CONFIG_MAX_PATH] = {0};
    DWORD found = SearchPathW(NULL, CONFIG_DEFAULT_RESTIC_EXE, NULL, CONFIG_MAX_PATH, system_restic, NULL);
    if (found > 0 && found < CONFIG_MAX_PATH) {
        wcsncpy_s(out_path, out_capacity, system_restic, _TRUNCATE);
        return TRUE;
    }

    wcsncpy_s(out_path, out_capacity, CONFIG_DEFAULT_RESTIC_EXE, _TRUNCATE);
    return FALSE;
}

/* Appends or updates a repository entry in out_config */
static BOOL upsert_repo_entry(
    ResticConfig* config,
    size_t* capacity,
    const WCHAR* alias,
    const WCHAR* target
) {
    /* Update in-place if alias already exists (case-insensitive) */
    for (size_t i = 0; i < config->repo_count; ++i) {
        if (_wcsicmp(config->repos[i].alias, alias) == 0) {
            wcsncpy_s(config->repos[i].target, CONFIG_MAX_PATH, target, _TRUNCATE);
            return TRUE;
        }
    }

    if (config->repo_count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        ResticRepoEntry* new_repos = (ResticRepoEntry*)realloc(
            config->repos,
            new_cap * sizeof(ResticRepoEntry)
        );
        if (!new_repos) return FALSE;
        config->repos = new_repos;
        *capacity = new_cap;
    }

    ResticRepoEntry* entry = &config->repos[config->repo_count];
    memset(entry, 0, sizeof(ResticRepoEntry));
    wcsncpy_s(entry->alias, MAX_PATH, alias, _TRUNCATE);
    wcsncpy_s(entry->target, CONFIG_MAX_PATH, target, _TRUNCATE);
    config->repo_count++;
    return TRUE;
}

BOOL config_load_from_file(const WCHAR* ini_path, ResticConfig* out_config) {
    if (!out_config) return FALSE;
    memset(out_config, 0, sizeof(ResticConfig));

    if (!ini_path || ini_path[0] == L'\0') {
        config_resolve_restic_path(NULL, out_config->restic_path, CONFIG_MAX_PATH);
        return FALSE;
    }

    /* Normalize INI path so Win32 Profile APIs do not default to %WINDIR% */
    if (GetFullPathNameW(ini_path, CONFIG_MAX_PATH, out_config->ini_path, NULL) == 0) {
        wcsncpy_s(out_config->ini_path, CONFIG_MAX_PATH, ini_path, _TRUNCATE);
    }

    config_resolve_restic_path(out_config->ini_path, out_config->restic_path, CONFIG_MAX_PATH);

    if (!file_exists(out_config->ini_path)) {
        return FALSE;
    }

    /* Read [Repositories] section with dynamic buffer expansion */
    DWORD buf_cap = 16384;
    WCHAR* section_buf = NULL;
    DWORD chars_read = 0;

    while (buf_cap <= 262144) {
        WCHAR* new_buf = (WCHAR*)realloc(section_buf, buf_cap * sizeof(WCHAR));
        if (!new_buf) {
            free(section_buf);
            return FALSE;
        }
        section_buf = new_buf;

        chars_read = GetPrivateProfileSectionW(
            L"Repositories",
            section_buf,
            buf_cap,
            out_config->ini_path
        );
        if (chars_read < buf_cap - 2) {
            break;
        }
        buf_cap *= 2;
    }

    size_t repo_capacity = 0;
    const WCHAR* cursor = section_buf;

    while (chars_read > 0 && *cursor != L'\0') {
        size_t line_len = wcslen(cursor);
        WCHAR line[CONFIG_MAX_PATH * 2];
        wcsncpy_s(line, _countof(line), cursor, _TRUNCATE);
        cursor += line_len + 1;

        trim_whitespace(line);
        if (line[0] == L'\0' || line[0] == L';' || line[0] == L'#') {
            continue;
        }

        WCHAR* eq = wcschr(line, L'=');
        if (!eq) {
            continue;
        }

        *eq = L'\0';
        WCHAR* alias  = line;
        WCHAR* target = eq + 1;

        trim_whitespace(alias);
        strip_quotes(alias);
        trim_whitespace(alias);

        trim_whitespace(target);
        strip_quotes(target);
        trim_whitespace(target);

        if (!is_valid_alias(alias) || target[0] == L'\0') {
            continue;
        }

        if (!upsert_repo_entry(out_config, &repo_capacity, alias, target)) {
            free(section_buf);
            config_free(out_config);
            return FALSE;
        }
    }

    free(section_buf);
    return TRUE;
}

BOOL config_load(ResticConfig* out_config) {
    if (!out_config) return FALSE;
    memset(out_config, 0, sizeof(ResticConfig));

    WCHAR resolved_ini[CONFIG_MAX_PATH] = {0};
    config_resolve_ini_path(resolved_ini, CONFIG_MAX_PATH, TRUE);
    return config_load_from_file(resolved_ini, out_config);
}

const ResticRepoEntry* config_find_repo(const ResticConfig* config, const WCHAR* alias) {
    if (!config || !config->repos || !alias || alias[0] == L'\0') {
        return NULL;
    }
    for (size_t i = 0; i < config->repo_count; ++i) {
        if (_wcsicmp(config->repos[i].alias, alias) == 0) {
            return &config->repos[i];
        }
    }
    return NULL;
}

void config_free(ResticConfig* config) {
    if (!config) return;
    free(config->repos);
    memset(config, 0, sizeof(ResticConfig));
}
