#include "scda_hooks.h"
#include "scda_gameplay.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

static_assert(sizeof(void*) == 4, "scda_hooks targets 32-bit splintercell3.exe.");

void ScdaLogMsg(const char* fmt, ...) {
    // Forwarded through internal logger set in InstallScdaHooks.
    extern void ScdaLogInternal(const char* fmt, va_list ap);
    va_list ap;
    va_start(ap, fmt);
    ScdaLogInternal(fmt, ap);
    va_end(ap);
}

namespace {

using FStrAssignFn = void(__fastcall*)(void* fstr, void* edx, const wchar_t* s);
using ConfigGetStringFn = int(__fastcall*)(void* self, void* edx, const wchar_t* section, const wchar_t* key,
                                         void* outFStr, const wchar_t* filename);
using ConfigGetStringBufFn = int(__fastcall*)(void* self, void* edx, const wchar_t* section, const wchar_t* key,
                                            wchar_t* value, int size, const wchar_t* filename);
using AppLocalizeFn = const wchar_t*(__cdecl*)(const wchar_t* section, const wchar_t* key, const wchar_t* package,
                                               const wchar_t* lang_ext, int b_optional, int arg6);
using StaticLoadObjectFn = void*(__cdecl*)(void* cls, void* outer, const wchar_t* name, const wchar_t* filename,
                                           DWORD flags, void* sandbox);
using OpenSndFileFn = HANDLE(__stdcall*)(const char* path);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

constexpr uintptr_t kRvaFStrAssign = 0x7A00u;
constexpr uintptr_t kRvaNullHashNode = 0x82C0u;
constexpr uintptr_t kRvaNullHashNext = 0x82C5u;
constexpr uintptr_t kRvaNullHashEmpty = 0x8311u;
constexpr uintptr_t kRvaConfigGetString = 0x22620u;
constexpr uintptr_t kRvaConfigGetStringBuf = 0x23F30u;
constexpr uintptr_t kRvaMeshUpdate = 0x3BC20u;
constexpr uintptr_t kRvaPlayAnimCore = 0x158010u;
constexpr uintptr_t kRvaAnimLookup = 0x15B470u;
constexpr uintptr_t kRvaAnimBlend = 0x15C380u;
constexpr uintptr_t kRvaDyingAnim = 0x2DFA34u;
constexpr uintptr_t kRvaDyingSkip = 0x2DFA8Eu;
constexpr uintptr_t kRvaAppLocalize = 0x3CC4B0u;
constexpr uintptr_t kRvaStaticLoadObject = 0x3E3750u;
constexpr uintptr_t kRvaOpenSndFile = 0x794580u;

uintptr_t g_exe_base = 0;
ScdaLogFn g_log_fn = nullptr;
wchar_t g_active_scda_map[64]{};

ConfigGetStringFn g_orig_config_get_string = nullptr;
ConfigGetStringBufFn g_orig_config_get_string_buf = nullptr;
AppLocalizeFn g_orig_app_localize = nullptr;
StaticLoadObjectFn g_orig_static_load_object = nullptr;
OpenSndFileFn g_orig_open_snd_file = nullptr;
CreateFileAFn g_orig_create_file_a = nullptr;
CreateFileWFn g_orig_create_file_w = nullptr;

void* g_orig_anim_lookup = nullptr;
void* g_orig_play_anim = nullptr;
void* g_orig_anim_blend = nullptr;
void* g_orig_mesh_update = nullptr;
void* g_orig_dying_anim = nullptr;
uintptr_t g_dying_skip = 0;
uintptr_t g_hash_next = 0;
uintptr_t g_hash_empty = 0;

void ScdaLog(const char* fmt, ...) {
    if (!g_log_fn) {
        return;
    }
    char buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log_fn(buf);
}

void WcsToUtf8(const wchar_t* ws, char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (ws) {
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, static_cast<int>(out_sz - 1), nullptr, nullptr);
    }
}

const wchar_t* GetExeDir() {
    static wchar_t s_exe_dir[MAX_PATH]{};
    static LONG s_init = 0;
    if (s_init == 2) {
        return s_exe_dir;
    }
    if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        wchar_t* slash = std::wcsrchr(buf, L'\\');
        if (slash) {
            slash[1] = L'\0';
        }
        wcscpy_s(s_exe_dir, MAX_PATH, buf);
        InterlockedExchange(&s_init, 2);
    } else {
        while (s_init != 2) {
            Sleep(0);
        }
    }
    return s_exe_dir;
}

void TrackActiveScdaMapFromWide(const wchar_t* path) {
    if (!path) {
        return;
    }
    const wchar_t* bs = std::wcsrchr(path, L'\\');
    const wchar_t* fs = std::wcsrchr(path, L'/');
    const wchar_t* base = path;
    if (bs && (!fs || bs > fs)) {
        base = bs + 1;
    } else if (fs) {
        base = fs + 1;
    }
    size_t blen = std::wcslen(base);
    if (blen <= 4 || _wcsicmp(base + blen - 4, L".scl") != 0) {
        return;
    }
    if (_wcsnicmp(base, L"scda_", 5) != 0) {
        if (g_active_scda_map[0] != L'\0' &&
            _wcsicmp(base, L"Entry.scl") != 0 &&
            _wcsnicmp(base, L"Menu", 4) != 0 &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            g_active_scda_map[0] = L'\0';
            ScdaLog("scda_active_map=<none>");
            OnScdaMapLoaded(g_active_scda_map);
        }
        return;
    }
    size_t len = blen - 4;
    if (len > 0 && len < _countof(g_active_scda_map)) {
        if (_wcsnicmp(g_active_scda_map, base, len) == 0 && g_active_scda_map[len] == L'\0') {
            return;
        }
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            return;
        }
        wcsncpy_s(g_active_scda_map, _countof(g_active_scda_map), base, len);
        g_active_scda_map[len] = L'\0';
        char map_u8[64];
        WcsToUtf8(g_active_scda_map, map_u8, sizeof(map_u8));
        ScdaLog("scda_active_map=%s", map_u8);
        OnScdaMapLoaded(g_active_scda_map);
    }
}

void TrackActiveScdaMapFromAnsi(const char* path) {
    if (!path) {
        return;
    }
    const char* bs = std::strrchr(path, '\\');
    const char* fs = std::strrchr(path, '/');
    const char* base = (!bs || (fs && fs > bs)) ? (fs ? fs + 1 : path) : (bs + 1);
    size_t blen = std::strlen(base);
    if (blen <= 4 || _stricmp(base + blen - 4, ".scl") != 0) {
        return;
    }
    wchar_t wpath[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, _countof(wpath));
    TrackActiveScdaMapFromWide(wpath);
}

// ---------------------------------------------------------------------------
// 1. Loose MapRotation.ini & .int localization fallback
// ---------------------------------------------------------------------------

bool IsMapRotationFilename(const wchar_t* filename) {
    return filename && (_wcsicmp(filename, L"MapRotation") == 0 || _wcsicmp(filename, L"MapRotation.ini") == 0);
}

bool LookupLooseMapRotation(const wchar_t* section, const wchar_t* key, wchar_t* out_buf, size_t out_cap) {
    if (!section || !key || !out_buf || out_cap == 0) {
        return false;
    }
    const wchar_t* exe_dir = GetExeDir();
    wchar_t maprot_ini[MAX_PATH]{};
    _snwprintf_s(maprot_ini, MAX_PATH, _TRUNCATE, L"%lsMapRotation.ini", exe_dir);
    if (GetPrivateProfileStringW(section, key, L"", out_buf, static_cast<DWORD>(out_cap), maprot_ini) > 0) {
        return true;
    }
    if (_wcsicmp(section, L"MAPS") == 0 && _wcsnicmp(key, L"MissionCoop[", 12) == 0) {
        const int idx = _wtoi(key + 12);
        if (idx >= 7) {
            wchar_t pattern[MAX_PATH]{};
            _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%ls..\\Data\\Maps\\scda_*.scl", exe_dir);
            WIN32_FIND_DATAW ffd{};
            HANDLE hFind = FindFirstFileW(pattern, &ffd);
            if (hFind != INVALID_HANDLE_VALUE) {
                wchar_t found[32][64]{};
                int count = 0;
                do {
                    if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        continue;
                    }
                    const wchar_t* dot = std::wcsrchr(ffd.cFileName, L'.');
                    const size_t stem_len = dot ? static_cast<size_t>(dot - ffd.cFileName) : std::wcslen(ffd.cFileName);
                    if (stem_len > 0 && stem_len < 64 && count < 32) {
                        wcsncpy_s(found[count], 64, ffd.cFileName, stem_len);
                        found[count][stem_len] = L'\0';
                        ++count;
                    }
                } while (FindNextFileW(hFind, &ffd));
                FindClose(hFind);

                for (int i = 0; i < count; ++i) {
                    for (int j = i + 1; j < count; ++j) {
                        if (_wcsicmp(found[i], found[j]) > 0) {
                            wchar_t tmp[64]{};
                            wcscpy_s(tmp, found[i]);
                            wcscpy_s(found[i], found[j]);
                            wcscpy_s(found[j], tmp);
                        }
                    }
                }

                const int scda_idx = idx - 7;
                if (scda_idx >= 0 && scda_idx < count) {
                    wcsncpy_s(out_buf, out_cap, found[scda_idx], _TRUNCATE);
                    return true;
                }
            }
        }
    }
    return false;
}

bool HasLocExtension(const wchar_t* s, size_t len) {
    static const wchar_t* kExts[] = {L".int", L".fra", L".deu", L".esp", L".ita", L".rus", L".pol", L".org"};
    if (!s || len < 4) {
        return false;
    }
    for (const wchar_t* ext : kExts) {
        if (_wcsicmp(s + len - 4, ext) == 0) {
            return true;
        }
    }
    return false;
}

bool LookupLooseLocalization(const wchar_t* section, const wchar_t* key, const wchar_t* filename, wchar_t* out_buf,
                             size_t out_cap) {
    if (!section || !key || !filename || !out_buf || out_cap == 0) {
        return false;
    }

    const size_t flen = std::wcslen(filename);
    const bool ends_ini = flen >= 4 && _wcsicmp(filename + flen - 4, L".ini") == 0;
    const bool has_loc_hint = !ends_ini || std::wcsstr(filename, L"Localization") != nullptr ||
                              std::wcsstr(filename, L"scda_") != nullptr ||
                              std::wcsstr(filename, L"x01_coop_") != nullptr ||
                              std::wcsstr(filename, L"x02_coop_") != nullptr ||
                              std::wcsstr(filename, L"x03_coop_") != nullptr ||
                              std::wcsstr(filename, L"HUD") != nullptr;
    if (!has_loc_hint) {
        return false;
    }

    const wchar_t* exe_dir = GetExeDir();
    const bool ends_loc = HasLocExtension(filename, flen);
    const wchar_t* bs = std::wcsrchr(filename, L'\\');
    const wchar_t* fs = std::wcsrchr(filename, L'/');
    const wchar_t* base = filename;
    if (bs && (!fs || bs > fs)) {
        base = bs + 1;
    } else if (fs) {
        base = fs + 1;
    }

    wchar_t base_noext[MAX_PATH]{};
    wcsncpy_s(base_noext, MAX_PATH, base, _TRUNCATE);
    const size_t blen = std::wcslen(base_noext);
    if (HasLocExtension(base_noext, blen)) {
        base_noext[blen - 4] = L'\0';
    }

    wchar_t cands[4][MAX_PATH]{};
    int cand_count = 0;
    _snwprintf_s(cands[cand_count++], MAX_PATH, _TRUNCATE, ends_loc ? L"%ls%ls" : L"%ls%ls.int", exe_dir, filename);
    _snwprintf_s(cands[cand_count++], MAX_PATH, _TRUNCATE, L"%lsLocalization\\%ls.int", exe_dir, base_noext);
    if (g_active_scda_map[0] != L'\0') {
        _snwprintf_s(cands[cand_count++], MAX_PATH, _TRUNCATE, L"%lsLocalization\\%ls.int", exe_dir,
                     g_active_scda_map);
        _snwprintf_s(cands[cand_count++], MAX_PATH, _TRUNCATE, L"%lsLocalization\\scda_override.int", exe_dir);
    }

    for (int i = 0; i < cand_count; ++i) {
        if (GetFileAttributesW(cands[i]) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (GetPrivateProfileStringW(section, key, L"", out_buf, static_cast<DWORD>(out_cap), cands[i]) > 0) {
            static LONG loc_logged = 0;
            if (InterlockedIncrement(&loc_logged) <= 40) {
                char sec_u8[64], key_u8[64], val_u8[128];
                WcsToUtf8(section, sec_u8, sizeof(sec_u8));
                WcsToUtf8(key, key_u8, sizeof(key_u8));
                WcsToUtf8(out_buf, val_u8, sizeof(val_u8));
                ScdaLog("scda_loc [%s] %s=\"%s\"", sec_u8, key_u8, val_u8);
            }
            return true;
        }
    }
    return false;
}

int __fastcall Hook_ConfigGetString(void* self, void* edx, const wchar_t* section, const wchar_t* key, void* outFStr,
                                    const wchar_t* filename) {
    const int res = g_orig_config_get_string(self, edx, section, key, outFStr, filename);
    if (res != 0 || !filename || !section || !key) {
        return res;
    }

    auto assign_fstr = reinterpret_cast<FStrAssignFn>(g_exe_base + kRvaFStrAssign);

    if (IsMapRotationFilename(filename)) {
        wchar_t val_buf[256]{};
        if (LookupLooseMapRotation(section, key, val_buf, _countof(val_buf))) {
            assign_fstr(outFStr, nullptr, val_buf);
            static LONG maprot_logged = 0;
            if (InterlockedIncrement(&maprot_logged) <= 32) {
                char sec_u8[64], key_u8[64], val_u8[128];
                WcsToUtf8(section, sec_u8, sizeof(sec_u8));
                WcsToUtf8(key, key_u8, sizeof(key_u8));
                WcsToUtf8(val_buf, val_u8, sizeof(val_u8));
                ScdaLog("scda_maprot [%s] %s=\"%s\"", sec_u8, key_u8, val_u8);
            }
            return 1;
        }
        return 0;
    }

    wchar_t wbuf[2048]{};
    if (LookupLooseLocalization(section, key, filename, wbuf, _countof(wbuf))) {
        assign_fstr(outFStr, nullptr, wbuf);
        return 1;
    }
    return 0;
}

int __fastcall Hook_ConfigGetStringBuf(void* self, void* edx, const wchar_t* section, const wchar_t* key,
                                       wchar_t* value, int size, const wchar_t* filename) {
    if (g_active_scda_map[0] != L'\0' && section && key) {
        ScdaNotifyLocalizedKey(section, key);
        if (value && size > 0 && ScdaOverrideInteractionLabel(section, key, value, static_cast<size_t>(size))) {
            return 1;
        }
    }

    const int res = g_orig_config_get_string_buf(self, edx, section, key, value, size, filename);
    if (res != 0 || !filename || !section || !key || !value || size <= 0) {
        return res;
    }

    if (IsMapRotationFilename(filename)) {
        return LookupLooseMapRotation(section, key, value, static_cast<size_t>(size)) ? 1 : 0;
    }

    if (LookupLooseLocalization(section, key, filename, value, static_cast<size_t>(size))) {
        return 1;
    }
    return 0;
}

const wchar_t* __cdecl Hook_AppLocalize(const wchar_t* section, const wchar_t* key, const wchar_t* package,
                                        const wchar_t* lang_ext, int b_optional, int arg6) {
    const wchar_t* res = g_orig_app_localize(section, key, package, lang_ext, b_optional, arg6);
    if (g_active_scda_map[0] != L'\0' && res && res[0] == L'<' && res[1] == L'?') {
        static LONG miss_logged = 0;
        if (InterlockedIncrement(&miss_logged) <= 40) {
            char res_u8[256];
            WcsToUtf8(res, res_u8, sizeof(res_u8));
            ScdaLog("scda_loc_miss %s", res_u8);
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// 2. Pattern package redirect (EchelonPattern.V* -> <map>_pat.V*)
// ---------------------------------------------------------------------------

void* __cdecl Hook_StaticLoadObject(void* cls, void* outer, const wchar_t* name, const wchar_t* filename, DWORD flags,
                                    void* sandbox) {
    if (name && std::wcsncmp(name, L"EchelonPattern.Vscda_", 21) == 0) {
        const wchar_t* vname = name + 16;  // points to scda_<map>
        if (std::wcslen(vname) < _countof(g_active_scda_map)) {
            wcscpy_s(g_active_scda_map, _countof(g_active_scda_map), vname);
            OnScdaMapLoaded(g_active_scda_map);
        }
    }
    void* res = g_orig_static_load_object(cls, outer, name, filename, flags, sandbox);
    if (!res && name && std::wcsncmp(name, L"EchelonPattern.V", 16) == 0) {
        const wchar_t* vname = name + 15;
        wchar_t alt[300]{};
        _snwprintf_s(alt, _countof(alt), _TRUNCATE, L"%ls_pat.%ls", vname + 1, vname);
        res = g_orig_static_load_object(cls, outer, alt, filename, flags, sandbox);
        char orig_u8[128], alt_u8[128];
        WcsToUtf8(name, orig_u8, sizeof(orig_u8));
        WcsToUtf8(alt, alt_u8, sizeof(alt_u8));
        ScdaLog("scda_slo_redirect from=%s to=%s ret=0x%p", orig_u8, alt_u8, res);
    }
    return res;
}

// ---------------------------------------------------------------------------
// 3. Side-by-side DareAudio sound bank redirect (MAPS/STREAM -> SCDA_*)
// ---------------------------------------------------------------------------

struct SoundRedirect {
    const char* src_a;
    const char* dst_a;
    const wchar_t* src_w;
    const wchar_t* dst_w;
};

constexpr SoundRedirect kSoundRedirects[] = {
    {"MAPS.SM0", "SCDA_MAPS.SM0", L"MAPS.SM0", L"SCDA_MAPS.SM0"},
    {"MAPS.LM0", "SCDA_MAPS.LM0", L"MAPS.LM0", L"SCDA_MAPS.LM0"},
    {"STREAM.SS0", "SCDA_STREAM.SS0", L"STREAM.SS0", L"SCDA_STREAM.SS0"},
    {"STREAM.LS0", "SCDA_STREAM.LS0", L"STREAM.LS0", L"SCDA_STREAM.LS0"},
};

struct NarrowOps {
    using Char = char;
    static const char* LastSep(const char* p) {
        const char* bs = std::strrchr(p, '\\');
        const char* fs = std::strrchr(p, '/');
        return (!bs || (fs && fs > bs)) ? fs : bs;
    }
    static int ICmp(const char* a, const char* b) { return _stricmp(a, b); }
    static size_t Len(const char* p) { return std::strlen(p); }
    static void Copy(char* dst, const char* src) { std::strcpy(dst, src); }
    static bool Exists(const char* path) {
        char full[MAX_PATH]{};
        return GetFullPathNameA(path, MAX_PATH, full, nullptr) != 0 &&
               GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES;
    }
    static const char* Match(const char* base) {
        for (const SoundRedirect& r : kSoundRedirects) {
            if (ICmp(base, r.src_a) == 0) {
                return r.dst_a;
            }
        }
        return nullptr;
    }
};

struct WideOps {
    using Char = wchar_t;
    static const wchar_t* LastSep(const wchar_t* p) {
        const wchar_t* bs = std::wcsrchr(p, L'\\');
        const wchar_t* fs = std::wcsrchr(p, L'/');
        return (!bs || (fs && fs > bs)) ? fs : bs;
    }
    static int ICmp(const wchar_t* a, const wchar_t* b) { return _wcsicmp(a, b); }
    static size_t Len(const wchar_t* p) { return std::wcslen(p); }
    static void Copy(wchar_t* dst, const wchar_t* src) { std::wcscpy(dst, src); }
    static bool Exists(const wchar_t* path) {
        wchar_t full[MAX_PATH]{};
        return GetFullPathNameW(path, MAX_PATH, full, nullptr) != 0 &&
               GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
    }
    static const wchar_t* Match(const wchar_t* base) {
        for (const SoundRedirect& r : kSoundRedirects) {
            if (ICmp(base, r.src_w) == 0) {
                return r.dst_w;
            }
        }
        return nullptr;
    }
};

template <typename Ops>
const typename Ops::Char* RewriteSoundPath(const typename Ops::Char* path, typename Ops::Char out[MAX_PATH]) {
    if (!path || g_active_scda_map[0] == L'\0') {
        return path;
    }
    const typename Ops::Char* sep = Ops::LastSep(path);
    const typename Ops::Char* base = sep ? sep + 1 : path;
    const typename Ops::Char* repl = Ops::Match(base);
    if (!repl) {
        return path;
    }
    const size_t prefix = static_cast<size_t>(base - path);
    const size_t repl_len = Ops::Len(repl);
    if (prefix + repl_len + 1 >= MAX_PATH) {
        return path;
    }
    std::memcpy(out, path, prefix * sizeof(typename Ops::Char));
    Ops::Copy(out + prefix, repl);
    return Ops::Exists(out) ? out : path;
}

void LogSoundRedirect(const char* kind, const char* from, const char* to) {
    static LONG snd_logged = 0;
    if (InterlockedIncrement(&snd_logged) <= 40) {
        ScdaLog("scda_snd_redirect kind=%s from=\"%s\" to=\"%s\"", kind, from, to);
    }
}

thread_local bool g_in_create_file = false;

HANDLE __stdcall Hook_OpenSndFile(const char* path) {
    char buf[MAX_PATH]{};
    const char* use = RewriteSoundPath<NarrowOps>(path, buf);
    if (use != path) {
        LogSoundRedirect("OpenSndFile", path, use);
    }
    return g_orig_open_snd_file(use);
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (g_in_create_file || !g_orig_create_file_a) {
        return g_orig_create_file_a
                   ? g_orig_create_file_a(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                          dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
                   : INVALID_HANDLE_VALUE;
    }
    TrackActiveScdaMapFromAnsi(lpFileName);
    g_in_create_file = true;
    char buf[MAX_PATH]{};
    const char* use = RewriteSoundPath<NarrowOps>(lpFileName, buf);
    g_in_create_file = false;
    if (use != lpFileName) {
        LogSoundRedirect("CreateFileA", lpFileName, use);
    }
    return g_orig_create_file_a(use, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                                dwFlagsAndAttributes, hTemplateFile);
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    if (g_in_create_file || !g_orig_create_file_w) {
        return g_orig_create_file_w
                   ? g_orig_create_file_w(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                          dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
                   : INVALID_HANDLE_VALUE;
    }
    TrackActiveScdaMapFromWide(lpFileName);
    g_in_create_file = true;
    wchar_t buf[MAX_PATH]{};
    const wchar_t* use = RewriteSoundPath<WideOps>(lpFileName, buf);
    g_in_create_file = false;
    if (use != lpFileName) {
        char from_u8[MAX_PATH], to_u8[MAX_PATH];
        WcsToUtf8(lpFileName, from_u8, sizeof(from_u8));
        WcsToUtf8(use, to_u8, sizeof(to_u8));
        LogSoundRedirect("CreateFileW", from_u8, to_u8);
    }
    return g_orig_create_file_w(use, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
                                dwFlagsAndAttributes, hTemplateFile);
}

// ---------------------------------------------------------------------------
// 4. Null-Anim & Null-Hash Safety Guards
// ---------------------------------------------------------------------------

void __declspec(naked) Hook_AnimLookup() {
    __asm {
        test ecx, ecx
        jnz call_orig
        mov eax, 0FFFFFFFFh
        ret 8
    call_orig:
        jmp dword ptr [g_orig_anim_lookup]
    }
}

void __declspec(naked) Hook_PlayAnimNull() {
    __asm {
        test ecx, ecx
        jnz call_orig
        mov eax, dword ptr [esp+0Ch]
        test eax, eax
        jz do_ret
        mov dword ptr [eax], 0FFFFFFFFh
    do_ret:
        xor eax, eax
        ret 24h
    call_orig:
        jmp dword ptr [g_orig_play_anim]
    }
}

void __declspec(naked) Hook_BlendNull() {
    __asm {
        test ecx, ecx
        jnz call_orig
        mov eax, dword ptr [esp+0Ch]
        test eax, eax
        jz do_ret
        mov dword ptr [eax], 0FFFFFFFFh
    do_ret:
        xor eax, eax
        ret 20h
    call_orig:
        jmp dword ptr [g_orig_anim_blend]
    }
}

void __declspec(naked) Hook_MeshUpdate() {
    __asm {
        test ecx, ecx
        jz skip
        jmp dword ptr [g_orig_mesh_update]
    skip:
        xor eax, eax
        ret 8
    }
}

void __declspec(naked) Hook_DyingNull() {
    __asm {
        test ebp, ebp
        jz skip
        jmp dword ptr [g_orig_dying_anim]
    skip:
        xor edi, edi
        xor eax, eax
        jmp dword ptr [g_dying_skip]
    }
}

void __declspec(naked) Hook_NullHashNode() {
    __asm {
        test eax, eax
        jz empty
        cmp eax, ecx
        je empty
        cmp esi, dword ptr [eax+8]
        jmp dword ptr [g_hash_next]
    empty:
        jmp dword ptr [g_hash_empty]
    }
}

uint8_t* ResolveRvaOrScan(uintptr_t exe_base, uintptr_t rva, const uint8_t* exp, size_t exp_len) {
    auto* primary = reinterpret_cast<uint8_t*>(exe_base + rva);
    if (std::memcmp(primary, exp, exp_len) == 0) {
        return primary;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* unique_hit = nullptr;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || sec->Misc.VirtualSize < exp_len) {
            continue;
        }
        auto* start = reinterpret_cast<uint8_t*>(exe_base + sec->VirtualAddress);
        const size_t limit = sec->Misc.VirtualSize - exp_len;
        for (size_t off = 0; off <= limit; ++off) {
            if (std::memcmp(start + off, exp, exp_len) == 0) {
                if (unique_hit != nullptr) {
                    return nullptr;
                }
                unique_hit = start + off;
            }
        }
    }
    return unique_hit;
}

bool InstallCheckedRvaHook(const char* name, uintptr_t rva, const uint8_t* exp, size_t exp_len, void* detour,
                           void** orig) {
    uint8_t* target = ResolveRvaOrScan(g_exe_base, rva, exp, exp_len);
    if (!target) {
        auto* raw = reinterpret_cast<const uint8_t*>(g_exe_base + rva);
        ScdaLog("scda_hook_%s_prologue_mismatch rva=0x%06lX got=%02X_%02X_%02X_%02X", name,
                static_cast<unsigned long>(rva), raw[0], raw[1], raw[2], raw[3]);
        return false;
    }
    const MH_STATUS c = MH_CreateHook(target, detour, orig);
    const MH_STATUS e = (c == MH_OK) ? MH_EnableHook(target) : c;
    if (e != MH_OK) {
        ScdaLog("scda_hook_%s_err (%s)", name, MH_StatusToString(e));
        return false;
    }
    return true;
}

bool InstallKernelHook(const char* name, void* detour, void** orig) {
    void* target = nullptr;
    const MH_STATUS c = MH_CreateHookApiEx(L"kernel32.dll", name, detour, orig, &target);
    const MH_STATUS e = (c == MH_OK && target != nullptr) ? MH_EnableHook(target) : c;
    if (e != MH_OK) {
        ScdaLog("scda_hook_%s_err (%s)", name, MH_StatusToString(e));
        return false;
    }
    return true;
}

}  // namespace

void ScdaLogInternal(const char* fmt, va_list ap) {
    if (!g_log_fn) {
        return;
    }
    char buf[512]{};
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    g_log_fn(buf);
}

const wchar_t* GetActiveScdaMap() {
    return g_active_scda_map;
}

void* ScdaStaticLoadObject(void* cls, void* outer, const wchar_t* name, const wchar_t* filename, uint32_t flags,
                           void* sandbox) {
    if (!g_orig_static_load_object) {
        return nullptr;
    }
    return g_orig_static_load_object(cls, outer, name, filename, flags, sandbox);
}

void InstallScdaHooks(uintptr_t exe_base, ScdaLogFn log_fn) {
    if (exe_base == 0) {
        return;
    }
    g_exe_base = exe_base;
    g_log_fn = log_fn;
    g_dying_skip = exe_base + kRvaDyingSkip;
    g_hash_next = exe_base + kRvaNullHashNext;
    g_hash_empty = exe_base + kRvaNullHashEmpty;

    static const uint8_t exp_getstr[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const uint8_t exp_getstr_buf[10] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xB0};
    static const uint8_t exp_apploc[10] = {0x81, 0xEC, 0x04, 0x02, 0x00, 0x00, 0xA1, 0xD0, 0x36, 0x1D};
    static const uint8_t exp_slo[6] = {0x8B, 0x0D, 0xE8, 0x1F, 0x31, 0x11};
    static const uint8_t exp_open[6] = {0x8B, 0x44, 0x24, 0x04, 0x6A, 0x00};
    static const uint8_t exp_lookup[8] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x83, 0xFB, 0xFF};
    static const uint8_t exp_play[6] = {0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
    static const uint8_t exp_blend[6] = {0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
    static const uint8_t exp_mesh[7] = {0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0xF1};
    static const uint8_t exp_dying[7] = {0x8B, 0x45, 0x6C, 0x33, 0xFF, 0x85, 0xC0};
    static const uint8_t exp_hash[5] = {0x74, 0x4F, 0x3B, 0x70, 0x08};

    int ok_count = 0;
    if (InstallCheckedRvaHook("ConfigGetString", kRvaConfigGetString, exp_getstr, sizeof(exp_getstr),
                              reinterpret_cast<void*>(&Hook_ConfigGetString),
                              reinterpret_cast<void**>(&g_orig_config_get_string))) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("ConfigGetStringBuf", kRvaConfigGetStringBuf, exp_getstr_buf, sizeof(exp_getstr_buf),
                              reinterpret_cast<void*>(&Hook_ConfigGetStringBuf),
                              reinterpret_cast<void**>(&g_orig_config_get_string_buf))) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("AppLocalize", kRvaAppLocalize, exp_apploc, sizeof(exp_apploc),
                              reinterpret_cast<void*>(&Hook_AppLocalize),
                              reinterpret_cast<void**>(&g_orig_app_localize))) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("StaticLoadObject", kRvaStaticLoadObject, exp_slo, sizeof(exp_slo),
                              reinterpret_cast<void*>(&Hook_StaticLoadObject),
                              reinterpret_cast<void**>(&g_orig_static_load_object))) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("OpenSndFile", kRvaOpenSndFile, exp_open, sizeof(exp_open),
                              reinterpret_cast<void*>(&Hook_OpenSndFile),
                              reinterpret_cast<void**>(&g_orig_open_snd_file))) {
        ++ok_count;
    }
    if (InstallKernelHook("CreateFileA", reinterpret_cast<void*>(&Hook_CreateFileA),
                          reinterpret_cast<void**>(&g_orig_create_file_a))) {
        ++ok_count;
    }
    if (InstallKernelHook("CreateFileW", reinterpret_cast<void*>(&Hook_CreateFileW),
                          reinterpret_cast<void**>(&g_orig_create_file_w))) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("AnimLookup", kRvaAnimLookup, exp_lookup, sizeof(exp_lookup),
                              reinterpret_cast<void*>(&Hook_AnimLookup), &g_orig_anim_lookup)) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("PlayAnimCore", kRvaPlayAnimCore, exp_play, sizeof(exp_play),
                              reinterpret_cast<void*>(&Hook_PlayAnimNull), &g_orig_play_anim)) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("AnimBlend", kRvaAnimBlend, exp_blend, sizeof(exp_blend),
                              reinterpret_cast<void*>(&Hook_BlendNull), &g_orig_anim_blend)) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("MeshUpdate", kRvaMeshUpdate, exp_mesh, sizeof(exp_mesh),
                              reinterpret_cast<void*>(&Hook_MeshUpdate), &g_orig_mesh_update)) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("DyingAnim", kRvaDyingAnim, exp_dying, sizeof(exp_dying),
                              reinterpret_cast<void*>(&Hook_DyingNull), &g_orig_dying_anim)) {
        ++ok_count;
    }
    if (InstallCheckedRvaHook("NullHashNode", kRvaNullHashNode, exp_hash, sizeof(exp_hash),
                              reinterpret_cast<void*>(&Hook_NullHashNode), nullptr)) {
        ++ok_count;
    }

    const int gp_ok = InstallScdaGameplayHooks(exe_base);
    ScdaLog("scda_hooks_ready count=%d/13 gameplay=%d", ok_count, gp_ok);
}
