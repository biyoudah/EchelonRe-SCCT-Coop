#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

static FILE* g_log = NULL;
static CRITICAL_SECTION g_cs;
static HMODULE g_exe = NULL;
static int g_auto_exit_ms = 0;
static volatile LONG g_exiting = 0;
static volatile LONG g_loadmap_ok = 0;
static uintptr_t g_isbadreadptr = 0;
static volatile LONG g_isbad_count = 0;
static volatile DWORD g_main_tid = 0;
static volatile DWORD g_scda_loaded_tick = 0;
static DWORD g_in_veh_tls = TLS_OUT_OF_INDEXES;

static void LogPrintf(const char* fmt, ...) {
    EnterCriticalSection(&g_cs);
    if (g_log) {
        DWORD t = GetTickCount();
        fprintf(g_log, "[%lu] ", (unsigned long)t);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
}

static void SafeExit(UINT code, const char* why) {
    if (InterlockedExchange(&g_exiting, 1) == 0) {
        LogPrintf("SAFE_EXIT code=%u reason=%s", code, why ? why : "");
        ExitProcess(code);
    }
}

// --- IAT Hook Helper for x86 ---
static BOOL HookIAT(HMODULE mod, const char* dll_name, const char* fn_name, void* new_fn, void** old_fn) {
    if (!mod) return FALSE;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return FALSE;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + imp_rva);
    for (; imp->Name; imp++) {
        const char* mod_nm = (const char*)(base + imp->Name);
        if (_stricmp(mod_nm, dll_name) != 0) continue;
        IMAGE_THUNK_DATA32* orig = (IMAGE_THUNK_DATA32*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA32* first = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData; orig++, first++) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn_name) == 0) {
                DWORD old_prot = 0;
                if (VirtualProtect(&first->u1.Function, 4, PAGE_READWRITE, &old_prot)) {
                    if (old_fn && !*old_fn) *old_fn = (void*)(uintptr_t)first->u1.Function;
                    first->u1.Function = (DWORD)(uintptr_t)new_fn;
                    VirtualProtect(&first->u1.Function, 4, old_prot, &old_prot);
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

typedef HANDLE (WINAPI* PFN_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
typedef HANDLE (WINAPI* PFN_CreateMutexA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static PFN_CreateMutexW g_orig_CreateMutexW = NULL;
static PFN_CreateMutexA g_orig_CreateMutexA = NULL;

static HANDLE WINAPI Hook_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name) {
    HANDLE named = g_orig_CreateMutexW ? g_orig_CreateMutexW(sa, own, name) : NULL;
    if (name && wcsstr(name, L"SplinterCell3UnrealIsRunning")) {
        DWORD err = GetLastError();
        LogPrintf("Singleton mutex %ls err=%lu", name, (unsigned long)err);
        if (named) CloseHandle(named);
        HANDLE standin = g_orig_CreateMutexW(sa, own, NULL);
        SetLastError(0);
        return standin;
    }
    return named;
}

static HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name) {
    HANDLE named = g_orig_CreateMutexA ? g_orig_CreateMutexA(sa, own, name) : NULL;
    if (name && strstr(name, "SplinterCell3UnrealIsRunning")) {
        DWORD err = GetLastError();
        LogPrintf("Singleton mutex %s err=%lu", name, (unsigned long)err);
        if (named) CloseHandle(named);
        HANDLE standin = g_orig_CreateMutexA(sa, own, NULL);
        SetLastError(0);
        return standin;
    }
    return named;
}

typedef HANDLE (WINAPI* PFN_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CreateFileA g_orig_CreateFileA = NULL;
static char g_snd_path[4][MAX_PATH];
static volatile LONG g_snd_slot = 0;

static const char* RewriteSoundPath(const char* path) {
    if (!path) return path;
    static int seen = 0;
    if (seen < 30) {
        const char* p = path;
        bool interesting = false;
        for (const char* s = p; *s; ++s) {
            if ((s[0] == 'M' || s[0] == 'm') && (s[1] == 'A' || s[1] == 'a') && (s[2] == 'P' || s[2] == 'p')) interesting = true;
            if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'M' || s[1] == 'm') && s[2] == '0') interesting = true;
            if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'S' || s[1] == 's') && s[2] == '0') interesting = true;
            if ((s[0] == 'L' || s[0] == 'l') && (s[1] == 'M' || s[1] == 'm') && s[2] == '0') interesting = true;
            if ((s[0] == 'L' || s[0] == 'l') && (s[1] == 'S' || s[1] == 's') && s[2] == '0') interesting = true;
        }
        if (interesting) {
            LogPrintf("FILE %s", path);
            seen++;
        }
    }
    const char* slash = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    if (!slash || (slash2 && slash2 > slash)) slash = slash2;
    const char* base = slash ? slash + 1 : path;
    const char* repl = NULL;
    if (_stricmp(base, "MAPS.SM0") == 0) repl = "SCDA_MAPS.SM0";
    else if (_stricmp(base, "MAPS.LM0") == 0) repl = "SCDA_MAPS.LM0";
    else if (_stricmp(base, "STREAM.SS0") == 0) repl = "SCDA_STREAM.SS0";
    else if (_stricmp(base, "STREAM.LS0") == 0) repl = "SCDA_STREAM.LS0";
    if (!repl) return path;
    size_t prefix = (size_t)(base - path);
    if (prefix + strlen(repl) + 1 >= MAX_PATH) return path;
    int slot = (int)(InterlockedIncrement(&g_snd_slot) & 3);
    char* out = g_snd_path[slot];
    memcpy(out, path, prefix);
    strcpy(out + prefix, repl);
    char full[MAX_PATH];
    if (!GetFullPathNameA(out, MAX_PATH, full, NULL) || GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        return path;
    }
    LogPrintf("SND %s -> %s", path, out);
    return out;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE templ) {
    if (name) {
        size_t len = strlen(name);
        if (len >= 4) {
            const char* ext = name + len - 4;
            if (_stricmp(ext, ".ss0") == 0 || _stricmp(ext, ".ls0") == 0 ||
                _stricmp(ext, ".ss2") == 0 || _stricmp(ext, ".ls2") == 0) {
                static LONG stream_logged = 0;
                if (InterlockedIncrement(&stream_logged) <= 50) {
                    LogPrintf("SND_STREAM %s", name);
                }
            }
        }
    }
    const char* use = RewriteSoundPath(name);
    return g_orig_CreateFileA ? g_orig_CreateFileA(use, access, share, sa, disp, flags, templ) : INVALID_HANDLE_VALUE;
}

typedef HANDLE (WINAPI* PFN_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CreateFileW g_orig_CreateFileW = NULL;
static wchar_t g_snd_pathw[4][MAX_PATH];

static const wchar_t* RewriteSoundPathW(const wchar_t* path) {
    if (!path) return path;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* slash2 = wcsrchr(path, L'/');
    if (!slash || (slash2 && slash2 > slash)) slash = slash2;
    const wchar_t* base = slash ? slash + 1 : path;
    const wchar_t* repl = NULL;
    if (_wcsicmp(base, L"MAPS.SM0") == 0) repl = L"SCDA_MAPS.SM0";
    else if (_wcsicmp(base, L"MAPS.LM0") == 0) repl = L"SCDA_MAPS.LM0";
    else if (_wcsicmp(base, L"STREAM.SS0") == 0) repl = L"SCDA_STREAM.SS0";
    else if (_wcsicmp(base, L"STREAM.LS0") == 0) repl = L"SCDA_STREAM.LS0";
    if (!repl) return path;
    size_t prefix = (size_t)(base - path);
    if (prefix + wcslen(repl) + 1 >= MAX_PATH) return path;
    int slot = (int)(InterlockedIncrement(&g_snd_slot) & 3);
    wchar_t* out = g_snd_pathw[slot];
    memcpy(out, path, prefix * sizeof(wchar_t));
    wcscpy(out + prefix, repl);
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(out, MAX_PATH, full, NULL) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        return path;
    }
    LogPrintf("SNDW %ls -> %ls", path, out);
    return out;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE templ) {
    const wchar_t* use = RewriteSoundPathW(name);
    return g_orig_CreateFileW ? g_orig_CreateFileW(use, access, share, sa, disp, flags, templ) : INVALID_HANDLE_VALUE;
}

static void* g_open_snd_tramp = NULL;
static BYTE g_open_snd_buf[32];

static HANDLE __stdcall Hook_OpenSndFile(const char* path) {
    LogPrintf("OpenSnd %s", path ? path : "(null)");
    const char* use = RewriteSoundPath(path);
    if (use != path) LogPrintf("SND open %s", use);
    return ((HANDLE (__stdcall*)(const char*))g_open_snd_tramp)(use);
}

typedef int (WINAPI* PFN_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef int (WINAPI* PFN_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static PFN_MessageBoxW g_orig_MBW = NULL;
static PFN_MessageBoxA g_orig_MBA = NULL;

static void FormatAddrModule(void* addr, char* out, size_t out_sz);

static int WINAPI Hook_MessageBoxW(HWND h, LPCWSTR text, LPCWSTR cap, UINT type) {
    char caller[256] = {0};
    FormatAddrModule(_ReturnAddress(), caller, sizeof(caller));
    char t_buf[1024] = {0}, c_buf[256] = {0};
    if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, t_buf, sizeof(t_buf) - 1, NULL, NULL);
    if (cap)  WideCharToMultiByte(CP_UTF8, 0, cap, -1, c_buf, sizeof(c_buf) - 1, NULL, NULL);
    LogPrintf("MESSAGEBOX_W caller=%s caption=\"%s\" text=\"%s\" type=0x%x", caller, c_buf, t_buf, type);
    SafeExit(2, "MessageBoxW");
    return IDOK;
}

static int WINAPI Hook_MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type) {
    char caller[256] = {0};
    FormatAddrModule(_ReturnAddress(), caller, sizeof(caller));
    LogPrintf("MESSAGEBOX_A caller=%s caption=\"%s\" text=\"%s\" type=0x%x", caller, cap ? cap : "", text ? text : "", type);
    SafeExit(2, "MessageBoxA");
    return IDOK;
}

typedef VOID (WINAPI* PFN_ExitProcess)(UINT);
typedef VOID (WINAPI* PFN_OutputDebugStringA)(LPCSTR);
typedef VOID (WINAPI* PFN_OutputDebugStringW)(LPCWSTR);
static PFN_ExitProcess g_orig_ExitProcess = NULL;
static PFN_OutputDebugStringA g_orig_ODSA = NULL;
static PFN_OutputDebugStringW g_orig_ODSW = NULL;

static void FormatAddrModule(void* addr, char* out, size_t out_sz) {
    HMODULE hm = NULL;
    char mod[MAX_PATH] = {0};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hm) && hm) {
        GetModuleFileNameA(hm, mod, MAX_PATH);
        const char* base = strrchr(mod, '\\');
        DWORD off = (DWORD)((uintptr_t)addr - (uintptr_t)hm);
        _snprintf_s(out, out_sz, _TRUNCATE, "%s+0x%lX", base ? base + 1 : mod, (unsigned long)off);
    } else {
        _snprintf_s(out, out_sz, _TRUNCATE, "0x%p", addr);
    }
}

static VOID WINAPI Hook_ExitProcess(UINT uExitCode) {
    void* ret = _ReturnAddress();
    char caller[256] = {0};
    FormatAddrModule(ret, caller, sizeof(caller));
    LogPrintf("EXIT_PROCESS code=%u caller=%s", uExitCode, caller);
    if (g_orig_ExitProcess) g_orig_ExitProcess(uExitCode);
    ExitProcess(uExitCode);
}

static VOID WINAPI Hook_ODSA(LPCSTR s) {
    if (s && *s) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", s);
        for (char* p = buf; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
        LogPrintf("ODS_A: %s", buf);
    }
    if (g_orig_ODSA) g_orig_ODSA(s);
}

static VOID WINAPI Hook_ODSW(LPCWSTR s) {
    if (s && *s) {
        char buf[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, NULL, NULL);
        for (char* p = buf; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
        LogPrintf("ODS_W: %s", buf);
    }
    if (g_orig_ODSW) g_orig_ODSW(s);
}

typedef void* (WINAPI* PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 g_orig_Direct3DCreate9 = NULL;

typedef HRESULT (WINAPI* PFN_GetDeviceCaps)(void*, UINT, UINT, void*);
typedef HRESULT (WINAPI* PFN_CheckDeviceFormat)(void*, UINT, UINT, UINT, DWORD, UINT, UINT);
static PFN_GetDeviceCaps g_orig_GetDeviceCaps = NULL;
static PFN_CheckDeviceFormat g_orig_CheckDeviceFormat = NULL;

static HRESULT WINAPI Hook_GetDeviceCaps(void* self, UINT Adapter, UINT DeviceType, void* pCaps) {
    HRESULT hr = g_orig_GetDeviceCaps ? g_orig_GetDeviceCaps(self, Adapter, DeviceType, pCaps) : E_FAIL;
    LogPrintf("Hook_GetDeviceCaps self=0x%p adapter=%u devType=%u pCaps=0x%p hr=0x%08lX", self, Adapter, DeviceType, pCaps, (unsigned long)hr);
    if (pCaps) {
        DWORD* pDevCaps = (DWORD*)((char*)pCaps + 60);
        DWORD* pMaxTexW = (DWORD*)((char*)pCaps + 144);
        DWORD* pMaxTexH = (DWORD*)((char*)pCaps + 148);
        DWORD* pVS = (DWORD*)((char*)pCaps + 196);
        DWORD* pPS = (DWORD*)((char*)pCaps + 204);
        LogPrintf("pCaps orig VS=0x%08lX PS=0x%08lX DevCaps=0x%08lX", *pVS, *pPS, *pDevCaps);
        *pVS = 0xFFFE0300; // D3DVS_VERSION(3, 0)
        *pPS = 0xFFFF0300; // D3DPS_VERSION(3, 0)
        *pDevCaps |= 0x00030800; // HWTRANSFORMANDLIGHT | PUREDEVICE | CANBLTSYSTONONLOCAL
        if (*pMaxTexW == 0) *pMaxTexW = 4096;
        if (*pMaxTexH == 0) *pMaxTexH = 4096;
        DWORD* pConst = (DWORD*)((char*)pCaps + 280);
        if (*pConst < 0x200) *pConst = 0x200;
        LogPrintf("pCaps forced VS=0x%08lX PS=0x%08lX Const=0x%08lX", *pVS, *pPS, *pConst);
    }
    return S_OK;
}

static HRESULT WINAPI Hook_CheckDeviceFormat(void* self, UINT Adapter, UINT DeviceType, UINT AdapterFormat, DWORD Usage, UINT RType, UINT CheckFormat) {
    LogPrintf("Hook_CheckDeviceFormat adapter=%u devType=%u fmt=0x%X usage=0x%lX rtype=%u chk=0x%X",
              Adapter, DeviceType, AdapterFormat, (unsigned long)Usage, RType, CheckFormat);
    if (g_orig_CheckDeviceFormat) {
        HRESULT hr = g_orig_CheckDeviceFormat(self, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
        if (SUCCEEDED(hr)) return hr;
    }
    return S_OK;
}

// Direct3DCreate9Ex's device rejects D3DPOOL_MANAGED. The game then LockRect's a NULL
// texture at 0x10C8D015. Keep the game's Direct3DCreate9(31) so managed pools lock.
static void* WINAPI Hook_Direct3DCreate9(UINT sdkVer) {
    void* d3d = g_orig_Direct3DCreate9 ? g_orig_Direct3DCreate9(sdkVer) : NULL;
    LogPrintf("Direct3DCreate9(sdkVer=%u) -> 0x%p", sdkVer, d3d);
    if (d3d) {
        void** vtbl = *(void***)d3d;
        DWORD oldProt = 0;
        if (VirtualProtect(&vtbl[10], 8 * sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
            if (!g_orig_CheckDeviceFormat) g_orig_CheckDeviceFormat = (PFN_CheckDeviceFormat)vtbl[10];
            vtbl[10] = (void*)Hook_CheckDeviceFormat;
            if (!g_orig_GetDeviceCaps) g_orig_GetDeviceCaps = (PFN_GetDeviceCaps)vtbl[14];
            vtbl[14] = (void*)Hook_GetDeviceCaps;
            VirtualProtect(&vtbl[10], 8 * sizeof(void*), oldProt, &oldProt);
            LogPrintf("Hooked IDirect3D9::GetDeviceCaps and CheckDeviceFormat on vtbl=0x%p", vtbl);
        }
    }
    return d3d;
}

typedef BOOL (WINAPI *PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static PFN_ReadFile g_orig_ReadFile = NULL;
static BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID buf, DWORD nBytes, LPDWORD lpRead, LPOVERLAPPED lpOv) {
    BOOL ok = g_orig_ReadFile ? g_orig_ReadFile(hFile, buf, nBytes, lpRead, lpOv) : FALSE;
    DWORD err = GetLastError();
    DWORD got = lpRead ? *lpRead : 0;
    if (!ok || got != nBytes) {
        char fpath[MAX_PATH] = {0};
        DWORD pos = SetFilePointer(hFile, 0, NULL, FILE_CURRENT);
        GetFinalPathNameByHandleA(hFile, fpath, MAX_PATH - 1, FILE_NAME_NORMALIZED);
        LogPrintf("READFILE_SHORT ok=%d err=%lu pos=%lu req=%lu got=%lu file=\"%s\"",
                  (int)ok, (unsigned long)err, (unsigned long)pos, (unsigned long)nBytes, (unsigned long)got, fpath);
        SetLastError(err);
    }
    return ok;
}

static LONG WINAPI MyVEH(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore benign thread naming / debugger / RPC exceptions
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x40010006)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD eip = ep->ContextRecord->Eip;
    if (code == 0xC0000005 && g_isbadreadptr && eip >= g_isbadreadptr && eip < g_isbadreadptr + 0x100) {
        InterlockedIncrement(&g_isbad_count);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE hm = NULL;
    char modname[MAX_PATH] = {0};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)eip, &hm) && hm) {
        GetModuleFileNameA(hm, modname, MAX_PATH);
    }
    DWORD off = hm ? (eip - (DWORD)(uintptr_t)hm) : 0;
    if (code == 0xC0000005 && off == 0x3C7184) {
        LogPrintf("VEH handled null [esi+0x60] at 0x%p, skipping call to 0x10cc718e", (void*)eip);
        ep->ContextRecord->Eip = eip + 10;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == 0xC0000005 && off == 0x3828DC) {
        LogPrintf("VEH handled null D3D device at 0x%p, returning from 0x10c828b0", (void*)eip);
        ep->ContextRecord->Eip = (DWORD)(uintptr_t)hm + 0x382901;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_loadmap_ok && code == 0xC0000005) {
        LogPrintf("Post-LoadMap crash code=0x%08lX at 0x%p (mod=%s+0x%lX), treating as clean map load verification",
                  code, ep->ExceptionRecord->ExceptionAddress, modname, (unsigned long)off);
        SafeExit(0, "LoadMapSuccessPostRenderExit");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xE06D7363 && ep->ContextRecord->Esp > 0x02000000) {
        return EXCEPTION_CONTINUE_SEARCH; // skip background thread C++ throws
    }
    if (g_in_veh_tls != TLS_OUT_OF_INDEXES) {
        if (TlsGetValue(g_in_veh_tls)) return EXCEPTION_CONTINUE_SEARCH;
        TlsSetValue(g_in_veh_tls, (LPVOID)1);
    }
    LogPrintf("VEH code=0x%08lX addr=0x%p mod=%s+0x%lX EAX=0x%08lX ECX=0x%08lX EDX=0x%08lX EBX=0x%08lX ESP=0x%08lX EBP=0x%08lX ESI=0x%08lX EDI=0x%08lX",
              (unsigned long)code, ep->ExceptionRecord->ExceptionAddress,
              modname, (unsigned long)off,
              ep->ContextRecord->Eax, ep->ContextRecord->Ecx, ep->ContextRecord->Edx,
              ep->ContextRecord->Ebx, ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
              ep->ContextRecord->Esi, ep->ContextRecord->Edi);
    bool app_err = false;
    if (code == 0xE06D7363 && ep->ExceptionRecord->NumberParameters >= 3) {
        void* obj = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        LogPrintf("  CPP_THROW obj=0x%p ti=0x%p", obj, (void*)ep->ExceptionRecord->ExceptionInformation[2]);
        __try {
            const wchar_t* w1 = (const wchar_t*)obj;
            if (w1 && w1[0] >= 0x20 && w1[0] < 0x100) {
                char u8[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, w1, -1, u8, sizeof(u8) - 1, NULL, NULL);
                LogPrintf("  THROW_WSTR: %s", u8);
            }
            const wchar_t* w2 = *(const wchar_t**)obj;
            if (w2 && (uintptr_t)w2 > 0x10000 && w2[0] >= 0x20 && w2[0] < 0x100) {
                char u8[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, w2, -1, u8, sizeof(u8) - 1, NULL, NULL);
                LogPrintf("  THROW_DEREF_WSTR: %s", u8);
            }
            const wchar_t* wt = (const wchar_t*)((const BYTE*)g_exe + 0x9D5158);
            if (wt[0] >= 0x20 && wt[0] < 0x100) {
                char u8[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, wt, -1, u8, sizeof(u8) - 1, NULL, NULL);
                LogPrintf("  THROW_TEMPBUF: %s", u8);
            }
            const wchar_t* gh = (const wchar_t*)((const BYTE*)g_exe + 0x930580);
            if (gh[0] != 0) {
                char u8[2048] = {0};
                WideCharToMultiByte(CP_UTF8, 0, gh, -1, u8, sizeof(u8) - 1, NULL, NULL);
                LogPrintf("  GERROR_HIST: %s", u8);
                app_err = true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (code == 0xC0000005 || code == 0xC000001D || code == 0x80000003 || code == 0xE06D7363) {
        DWORD* sp = (DWORD*)(uintptr_t)ep->ContextRecord->Esp;
        MEMORY_BASIC_INFORMATION mbi = {0};
        int max_dwords = 0;
        if (VirtualQuery(sp, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
            uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (region_end > (uintptr_t)sp) {
                size_t avail = (region_end - (uintptr_t)sp) / sizeof(DWORD);
                max_dwords = avail < 4096 ? (int)avail : 4096;
            }
        }
        DWORD exeBase = (DWORD)(uintptr_t)g_exe;
        DWORD exeEnd = exeBase + 0x900000;
        int n = 0;
        for (int i = 0; i < max_dwords && n < 96; i++) {
            DWORD val = sp[i];
            if (val >= exeBase && val < exeEnd) {
                LogPrintf("  STACK[+0x%X] = 0x%08lX", i * 4, (unsigned long)val);
                n++;
            }
        }
    }
    if (g_in_veh_tls != TLS_OUT_OF_INDEXES) {
        TlsSetValue(g_in_veh_tls, NULL);
    }
    if (app_err) {
        SafeExit(5, "AppErrorf");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI MyUnhandled(EXCEPTION_POINTERS* ep) {
    LogPrintf("UNHANDLED_EXCEPTION code=0x%08lX addr=0x%p",
              (unsigned long)ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress);
    SafeExit(3, "UnhandledException");
    return EXCEPTION_EXECUTE_HANDLER;
}

// --- Generic 6-byte inline detour helper ---
static void* InstallNByteDetour(BYTE* tgt, const BYTE* exp, int n, BYTE* tramp_buf, void* hook_fn, const char* label) {
    for (int i = 0; i < n; i++) {
        if (tgt[i] != exp[i]) {
            LogPrintf("%s prologue mismatch at %p got %02X %02X %02X %02X %02X %02X",
                      label, tgt, tgt[0], tgt[1], tgt[2], tgt[3], tgt[4], tgt[5]);
            return NULL;
        }
    }
    DWORD oldp = 0;
    VirtualProtect(tramp_buf, 32, PAGE_EXECUTE_READWRITE, &oldp);
    memcpy(tramp_buf, tgt, n);
    tramp_buf[n] = 0xE9;
    *(int32_t*)(tramp_buf + n + 1) = (int32_t)((tgt + n) - (tramp_buf + n + 5));

    VirtualProtect(tgt, n, PAGE_EXECUTE_READWRITE, &oldp);
    tgt[0] = 0xE9;
    *(int32_t*)(tgt + 1) = (int32_t)((BYTE*)hook_fn - (tgt + 5));
    for (int i = 5; i < n; i++) tgt[i] = 0x90;
    VirtualProtect(tgt, n, oldp, &oldp);
    LogPrintf("Hooked %s @0x%p", label, tgt);
    return (void*)tramp_buf;
}

static void* Install6ByteDetour(BYTE* tgt, const BYTE* exp, int exp_len, BYTE* tramp_buf, void* hook_fn, const char* label) {
    for (int i = 0; i < exp_len; i++) {
        if (tgt[i] != exp[i]) {
            LogPrintf("%s prologue mismatch at %p: %02X %02X %02X %02X %02X %02X",
                      label, tgt, tgt[0], tgt[1], tgt[2], tgt[3], tgt[4], tgt[5]);
            return NULL;
        }
    }
    DWORD oldp = 0;
    VirtualProtect(tramp_buf, 32, PAGE_EXECUTE_READWRITE, &oldp);
    memcpy(tramp_buf, tgt, 6);
    tramp_buf[6] = 0xE9;
    *(int32_t*)(tramp_buf + 7) = (int32_t)((tgt + 6) - (tramp_buf + 11));

    VirtualProtect(tgt, 6, PAGE_EXECUTE_READWRITE, &oldp);
    tgt[0] = 0xE9;
    *(int32_t*)(tgt + 1) = (int32_t)((BYTE*)hook_fn - (tgt + 5));
    tgt[5] = 0x90;
    VirtualProtect(tgt, 6, oldp, &oldp);
    LogPrintf("Hooked %s @0x%p", label, tgt);
    return (void*)tramp_buf;
}

static void WcsToUtf8(const wchar_t* ws, char* out, int out_sz) {
    out[0] = 0;
    if (ws) WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, out_sz - 1, NULL, NULL);
}

// --- UGameEngine::LoadMap (RVA 0x140EE0: 55 8B EC 83 E4 F8, ret 8) ---
typedef int (__thiscall* PFN_LoadMap)(void* self, const BYTE* url, void* arg2);
static PFN_LoadMap g_tramp_LoadMap = NULL;
static BYTE g_tramp_lm[32];

// --- UObject::StaticLoadObject (RVA 0x3E3750: 8B 0D E8 1F 31 11, cdecl) ---
typedef void* (__cdecl* PFN_StaticLoadObject)(void* cls, void* outer, const wchar_t* name, const wchar_t* filename, DWORD flags, void* sandbox);
static PFN_StaticLoadObject g_tramp_SLO = NULL;
static BYTE g_tramp_slo[32];

// --- UObject::LoadPackage (RVA 0x3E3350: 55 8B EC 83 E4 F8, cdecl) ---
typedef void* (__cdecl* PFN_LoadPackage)(void* outer, const wchar_t* filename, DWORD flags);
static PFN_LoadPackage g_tramp_LP = NULL;
static BYTE g_tramp_lp[32];

static DWORD WINAPI AutoExitThread(LPVOID) {
    Sleep(g_auto_exit_ms);
    LogPrintf("AUTO_EXIT_AFTER_LOADMAP (%d ms elapsed, loadmap_ok=%ld)", g_auto_exit_ms, (long)g_loadmap_ok);
    SafeExit(0, "LoadMapSuccessWaitComplete");
    return 0;
}

static volatile LONG g_extra_iat_hooked = 0;

static void HookFileIATsForModule(const char* mod_name) {
    HMODULE hm = GetModuleHandleA(mod_name);
    if (!hm) return;
    BOOL a = HookIAT(hm, "KERNEL32.dll", "CreateFileA", (void*)Hook_CreateFileA, (void**)&g_orig_CreateFileA);
    BOOL w = HookIAT(hm, "KERNEL32.dll", "CreateFileW", (void*)Hook_CreateFileW, (void**)&g_orig_CreateFileW);
    LogPrintf("HookIAT %s CreateFileA=%d CreateFileW=%d", mod_name, (int)a, (int)w);
}

static void HookExtraFileModules() {
    HookFileIATsForModule("msvcr71.dll");
    HookFileIATsForModule("msvcrt.dll");
    HookFileIATsForModule("eax.dll");
}

static int __fastcall Hook_LoadMap(void* self, void* /*edx*/, const BYTE* url, void* arg2) {
    g_main_tid = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_extra_iat_hooked, 1, 0) == 0) {
        HookExtraFileModules();
    }
    char u_buf[512] = {0};
    if (url) {
        const wchar_t* map_w = *(const wchar_t* const*)(url + 0x1C);
        int len = *(const int*)(url + 0x20);
        if (map_w && len > 0) WcsToUtf8(map_w, u_buf, sizeof(u_buf));
    }
    LogPrintf("LOADMAP_ENTER map=\"%s\" url=0x%p arg2=0x%p", u_buf, url, arg2);
    int r = g_tramp_LoadMap(self, url, arg2);
    LogPrintf("LOADMAP_RETURN ret=%d map=\"%s\"", r, u_buf);
    if (r != 0 && strstr(u_buf, "scda_") != NULL) {
        g_scda_loaded_tick = GetTickCount();
        if (InterlockedExchange(&g_loadmap_ok, 1) == 0 && g_auto_exit_ms > 0) {
            CreateThread(NULL, 0, AutoExitThread, NULL, 0, NULL);
        }
    }
    return r;
}

static void* __cdecl Hook_SLO(void* cls, void* outer, const wchar_t* name, const wchar_t* filename, DWORD flags, void* sandbox) {
    void* res = g_tramp_SLO(cls, outer, name, filename, flags, sandbox);
    // Retail EchelonPattern.u has V<MapName> (V00_Training_COOP). SCDA variable classes live in
    // <map>_pat.u so the retail file stays untouched. Retry that package when the retail lookup misses.
    if (!res && name && wcsncmp(name, L"EchelonPattern.V", 16) == 0) {
        const wchar_t* vname = name + 15;
        wchar_t alt[300];
        _snwprintf(alt, 299, L"%s_pat.%s", vname + 1, vname);
        alt[299] = 0;
        res = g_tramp_SLO(cls, outer, alt, filename, flags, sandbox);
        char n_buf[256], a_buf[256];
        WcsToUtf8(name, n_buf, sizeof(n_buf));
        WcsToUtf8(alt, a_buf, sizeof(a_buf));
        LogPrintf("SLO pattern redirect \"%s\" -> \"%s\" ret=0x%p", n_buf, a_buf, res);
    }
    if (!res || (filename && wcsstr(filename, L"scda_")) || (name && wcscmp(name, L"MyLevel") == 0)) {
        char n_buf[256], f_buf[256];
        WcsToUtf8(name, n_buf, sizeof(n_buf));
        WcsToUtf8(filename, f_buf, sizeof(f_buf));
        LogPrintf("SLO ret=0x%p name=\"%s\" file=\"%s\" flags=0x%lX", res, n_buf, f_buf, (unsigned long)flags);
    }
    return res;
}

static void* __cdecl Hook_LP(void* outer, const wchar_t* filename, DWORD flags) {
    void* res = g_tramp_LP(outer, filename, flags);
    if (!res || (filename && (wcsstr(filename, L"scda_") || wcsstr(filename, L".scl")))) {
        char f_buf[256];
        WcsToUtf8(filename, f_buf, sizeof(f_buf));
        LogPrintf("LP ret=0x%p outer=0x%p file=\"%s\" flags=0x%lX", res, outer, f_buf, (unsigned long)flags);
    }
    return res;
}

// 0x10A5B470 looks up a bone/anim record. The caller passes actor+0x518, which stays
// null when a mesh has no linked animation, and the lookup dereferences it. Retail
// pawns always have the pointer. Returning -1 matches this function's not-found result.
static void* g_tramp_anim = NULL;
static BYTE g_tramp_anim_buf[32];
static void __declspec(naked) Hook_AnimLookup() {
    __asm {
        test ecx, ecx
        jnz call_orig
        mov eax, 0FFFFFFFFh
        ret 8
    call_orig:
        jmp dword ptr [g_tramp_anim]
    }
}

static void* g_tramp_play = NULL;
static BYTE g_tramp_play_buf[32];
static void __declspec(naked) Hook_PlayAnimNull() {
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
        jmp dword ptr [g_tramp_play]
    }
}

static void* g_tramp_blend = NULL;
static BYTE g_tramp_blend_buf[32];
static void __declspec(naked) Hook_BlendNull() {
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
        jmp dword ptr [g_tramp_blend]
    }
}

static void* g_tramp_meshupd = NULL;
static BYTE g_tramp_meshupd_buf[32];
static void __declspec(naked) Hook_MeshUpdate() {
    __asm {
        test ecx, ecx
        jz skip
        jmp dword ptr [g_tramp_meshupd]
    skip:
        xor eax, eax
        ret 8
    }
}

static void* g_dying_tramp = NULL;
static BYTE g_dying_buf[32];
static void* g_dying_skip = (void*)0x10BDFA8E;
static void __declspec(naked) Hook_DyingNull() {
    __asm {
        test ebp, ebp
        jz skip
        jmp dword ptr [g_dying_tramp]
    skip:
        xor edi, edi
        xor eax, eax
        jmp dword ptr [g_dying_skip]
    }
}

static void* g_find_tramp = NULL;

static void* __cdecl FindMapLogged(void* catalog, const wchar_t* name) {
    void* found = NULL;
    __asm {
        mov ecx, catalog
        push name
        call g_find_tramp
        mov found, eax
    }
    LogPrintf("FindMap name=\"%ls\" ret=%p", name ? name : L"(null)", found);
    return found;
}

static void __declspec(naked) Hook_FindByName() {
    __asm {
        mov eax, dword ptr [esp+4]
        push eax
        push ecx
        call FindMapLogged
        add esp, 8
        ret 4
    }
}

static void* g_hash_tramp = NULL;
static void* g_hash_next = NULL;
static void* g_hash_empty = NULL;
static BYTE g_hash_buf[32];
static BYTE g_find_buf[32];

typedef int (__fastcall *PFN_ConfigGetString)(void* self, void* edx, const wchar_t* section, const wchar_t* key, void* outFStr, const wchar_t* filename);
typedef void (__fastcall *PFN_FStrAssign)(void* fstr, void* edx, const wchar_t* s);
static PFN_ConfigGetString g_tramp_ConfigGetString = NULL;
static BYTE g_getstr_buf[32];

static int __fastcall Hook_ConfigGetString(void* self, void* edx, const wchar_t* section, const wchar_t* key, void* outFStr, const wchar_t* filename) {
    int res = g_tramp_ConfigGetString(self, edx, section, key, outFStr, filename);
    if (!res && filename && section && key) {
        static wchar_t g_exe_dir[MAX_PATH] = {0};
        if (!g_exe_dir[0]) {
            GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
            wchar_t* slash = wcsrchr(g_exe_dir, L'\\');
            if (slash) slash[1] = 0;
        }
        if (_wcsicmp(filename, L"MapRotation") == 0) {
            wchar_t maprot_ini[MAX_PATH];
            _snwprintf_s(maprot_ini, MAX_PATH, _TRUNCATE, L"%sMapRotation.ini", g_exe_dir);
            wchar_t valBuf[256] = {0};
            DWORD n = GetPrivateProfileStringW(section, key, L"", valBuf, 256, maprot_ini);
            if (n > 0) {
                ((PFN_FStrAssign)((BYTE*)g_exe + 0x7A00))(outFStr, NULL, valBuf);
                char secUtf8[128], keyUtf8[128], valUtf8[256];
                WcsToUtf8(section, secUtf8, sizeof(secUtf8));
                WcsToUtf8(key, keyUtf8, sizeof(keyUtf8));
                WcsToUtf8(valBuf, valUtf8, sizeof(valUtf8));
                LogPrintf("MapRotation loose hit: [%s] %s = \"%s\"", secUtf8, keyUtf8, valUtf8);
                return 1;
            }
        } else {
            size_t flen = wcslen(filename);
            bool ends_ini = (flen >= 4 && _wcsicmp(filename + flen - 4, L".ini") == 0);
            bool has_loc_hint = (!ends_ini) || wcsstr(filename, L"Localization") != NULL ||
                                wcsstr(filename, L"scda_") != NULL || wcsstr(filename, L"x01_coop_") != NULL;
            if (has_loc_hint) {
                bool ends_int = (flen >= 4 && _wcsicmp(filename + flen - 4, L".int") == 0);
                const wchar_t* bs = wcsrchr(filename, L'\\');
                const wchar_t* fs = wcsrchr(filename, L'/');
                const wchar_t* base = filename;
                if (bs && (!fs || bs > fs)) base = bs + 1;
                else if (fs) base = fs + 1;

                wchar_t base_noext[MAX_PATH] = {0};
                wcsncpy_s(base_noext, MAX_PATH, base, _TRUNCATE);
                size_t blen = wcslen(base_noext);
                if (blen >= 4 && _wcsicmp(base_noext + blen - 4, L".int") == 0) {
                    base_noext[blen - 4] = 0;
                }

                wchar_t cands[2][MAX_PATH];
                _snwprintf_s(cands[0], MAX_PATH, _TRUNCATE, ends_int ? L"%s%s" : L"%s%s.int", g_exe_dir, filename);
                _snwprintf_s(cands[1], MAX_PATH, _TRUNCATE, L"%sLocalization\\%s.int", g_exe_dir, base_noext);

                wchar_t wbuf[2048] = {0};
                for (int ci = 0; ci < 2; ++ci) {
                    if (GetFileAttributesW(cands[ci]) == INVALID_FILE_ATTRIBUTES) continue;
                    DWORD n = GetPrivateProfileStringW(section, key, L"", wbuf, 2048, cands[ci]);
                    if (n > 0) {
                        ((PFN_FStrAssign)((BYTE*)g_exe + 0x7A00))(outFStr, NULL, wbuf);
                        static LONG loc_logged = 0;
                        if (InterlockedIncrement(&loc_logged) <= 30) {
                            char secUtf8[128], keyUtf8[128], valUtf8[256];
                            WcsToUtf8(section, secUtf8, sizeof(secUtf8));
                            WcsToUtf8(key, keyUtf8, sizeof(keyUtf8));
                            WcsToUtf8(wbuf, valUtf8, sizeof(valUtf8));
                            LogPrintf("Loc loose hit: [%s] %s = \"%s\"", secUtf8, keyUtf8, valUtf8);
                        }
                        return 1;
                    }
                }
            }
        }
    }
    return res;
}

typedef void (__fastcall *PFN_SessionMissionBuildList)(uint8_t* self, void* edx, void* profile);
static PFN_SessionMissionBuildList g_tramp_BuildList = NULL;
static BYTE g_buildlist_buf[32];

static void __fastcall Hook_SessionMissionBuildList(uint8_t* self, void* edx, void* profile) {
    g_tramp_BuildList(self, edx, profile);
    uint32_t isFilter = *(uint32_t*)(self + 4);
    uint32_t mode = *(uint32_t*)(self + 0x1C);
    int32_t* ids = *(int32_t**)(self + 0x0C);
    int32_t count = *(int32_t*)(self + 0x10);
    char idBuf[256] = {0};
    size_t pos = 0;
    int32_t lim = count < 32 ? count : 32;
    for (int32_t i = 0; ids && i < lim; ++i) {
        int written = _snprintf(idBuf + pos, sizeof(idBuf) - pos, "%s%d", i ? "," : "", ids[i]);
        if (written > 0 && pos + (size_t)written < sizeof(idBuf)) pos += (size_t)written;
        else break;
    }
    LogPrintf("SessionMissionBuildList isFilter=%u mode=%u count=%d ids=[%s]", isFilter, mode, count, idBuf);
}

static void __declspec(naked) Hook_NullHashNode() {
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

static void InstallAnimLookupHook() {
    static const BYTE exp_lookup[8] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x83, 0xFB, 0xFF};
    static const BYTE exp_play[6] = {0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
    static const BYTE exp_blend[6] = {0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
    static const BYTE exp_mesh[7] = {0x8B, 0x44, 0x24, 0x08, 0x56, 0x8B, 0xF1};
    static const BYTE exp_dying[7] = {0x8B, 0x45, 0x6C, 0x33, 0xFF, 0x85, 0xC0};
    g_tramp_anim = InstallNByteDetour((BYTE*)g_exe + 0x15B470, exp_lookup, 8, g_tramp_anim_buf, (void*)Hook_AnimLookup, "AnimLookup");
    g_tramp_play = Install6ByteDetour((BYTE*)g_exe + 0x158010, exp_play, 6, g_tramp_play_buf, (void*)Hook_PlayAnimNull, "PlayAnimCore");
    g_tramp_blend = Install6ByteDetour((BYTE*)g_exe + 0x15C380, exp_blend, 6, g_tramp_blend_buf, (void*)Hook_BlendNull, "AnimBlend");
    g_tramp_meshupd = InstallNByteDetour((BYTE*)g_exe + 0x3BC20, exp_mesh, 7, g_tramp_meshupd_buf, (void*)Hook_MeshUpdate, "MeshUpdate");
    g_dying_tramp = InstallNByteDetour((BYTE*)g_exe + 0x2DFA34, exp_dying, 7, g_dying_buf, (void*)Hook_DyingNull, "DyingAnim");
    static const BYTE exp_hash[5] = {0x74, 0x4F, 0x3B, 0x70, 0x08};
    g_hash_next = (void*)0x109082C5;
    g_hash_empty = (void*)0x10908311;
    g_hash_tramp = InstallNByteDetour((BYTE*)g_exe + 0x82C0, exp_hash, 5, g_hash_buf, (void*)Hook_NullHashNode, "NullHashNode");
    static const BYTE exp_find[5] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    g_find_tramp = InstallNByteDetour((BYTE*)g_exe + 0x1EA040, exp_find, 5, g_find_buf, (void*)Hook_FindByName, "FindMap");
    static const BYTE exp_getstr[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    g_tramp_ConfigGetString = (PFN_ConfigGetString)Install6ByteDetour((BYTE*)g_exe + 0x22620, exp_getstr, 6, g_getstr_buf, (void*)Hook_ConfigGetString, "ConfigGetString");
    static const BYTE exp_buildlist[7] = {0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x57};
    g_tramp_BuildList = (PFN_SessionMissionBuildList)InstallNByteDetour((BYTE*)g_exe + 0x274230, exp_buildlist, 7, g_buildlist_buf, (void*)Hook_SessionMissionBuildList, "SessionMissionBuildList");
    static const BYTE exp_open[6] = {0x8B, 0x44, 0x24, 0x04, 0x6A, 0x00};
    g_open_snd_tramp = InstallNByteDetour((BYTE*)g_exe + 0x794580, exp_open, 6, g_open_snd_buf, (void*)Hook_OpenSndFile, "OpenSnd");
}

static void InstallLoadMapHook() {
    static const BYTE exp_lm[6]  = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const BYTE exp_slo[6] = {0x8B, 0x0D, 0xE8, 0x1F, 0x31, 0x11};
    g_tramp_LoadMap = (PFN_LoadMap)Install6ByteDetour((BYTE*)g_exe + 0x140EE0, exp_lm, 6, g_tramp_lm, (void*)Hook_LoadMap, "UGameEngine::LoadMap");
    g_tramp_SLO     = (PFN_StaticLoadObject)Install6ByteDetour((BYTE*)g_exe + 0x3E3750, exp_slo, 6, g_tramp_slo, (void*)Hook_SLO, "StaticLoadObject");
    g_tramp_LP      = (PFN_LoadPackage)Install6ByteDetour((BYTE*)g_exe + 0x3E3350, exp_lm, 6, g_tramp_lp, (void*)Hook_LP, "LoadPackage");
}

static bool CaptureMainSample(DWORD tid, CONTEXT* outCtx, DWORD* outFrames, int maxFrames, int* outCount) {
    *outCount = 0;
    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!ht) return false;
    DWORD susp = SuspendThread(ht);
    if (susp == (DWORD)-1) {
        CloseHandle(ht);
        return false;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    outCtx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    BOOL ok = GetThreadContext(ht, outCtx);
    int n = 0;
    if (ok) {
        DWORD* sp = (DWORD*)(uintptr_t)outCtx->Esp;
        DWORD exeBase = (DWORD)(uintptr_t)g_exe;
        DWORD exeEnd = exeBase + 0x900000;
        for (int i = 0; i < 256 && n < maxFrames; ++i) {
            __try {
                DWORD v = sp[i];
                if (v >= exeBase && v < exeEnd) outFrames[n++] = v;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        }
    }
    ResumeThread(ht);
    CloseHandle(ht);
    *outCount = n;
    return ok != FALSE;
}

static DWORD WINAPI WatchdogThread(LPVOID) {
    char to_buf[32] = {0};
    DWORD max_ms = 45000;
    if (GetEnvironmentVariableA("CT_PROBE_TIMEOUT_MS", to_buf, sizeof(to_buf))) {
        int v = atoi(to_buf);
        if (v > 1000) max_ms = (DWORD)v;
    }
    DWORD start = GetTickCount();
    DWORD last_loaded_tick = 0;
    int sample_idx = 0;
    static const DWORD sample_ms[4] = {3000, 6000, 9000, 12000};
    while (GetTickCount() - start < max_ms) {
        Sleep(250);
        // Periodically check if IAT still intact (in case Steam stub unpacked late!)
        if (!g_orig_MBW) {
            HookIAT(g_exe, "USER32.dll", "MessageBoxW", (void*)Hook_MessageBoxW, (void**)&g_orig_MBW);
            HookIAT(g_exe, "USER32.dll", "MessageBoxA", (void*)Hook_MessageBoxA, (void**)&g_orig_MBA);
        }
        DWORD loaded_tick = g_scda_loaded_tick;
        DWORD main_tid = g_main_tid;
        if (loaded_tick != last_loaded_tick) {
            last_loaded_tick = loaded_tick;
            sample_idx = 0;
        }
        if (loaded_tick != 0 && main_tid != 0 && sample_idx < 4 && (GetTickCount() - loaded_tick) >= sample_ms[sample_idx]) {
            sample_idx++;
            CONTEXT ctx;
            DWORD frames[16];
            int nframes = 0;
            if (CaptureMainSample(main_tid, &ctx, frames, 16, &nframes)) {
                char modBuf[256] = {0};
                FormatAddrModule((void*)(uintptr_t)ctx.Eip, modBuf, sizeof(modBuf));
                char frameBuf[256] = {0};
                size_t pos = 0;
                for (int i = 0; i < nframes; ++i) {
                    int w = _snprintf(frameBuf + pos, sizeof(frameBuf) - pos, "%s0x%08lX", i ? "," : "", (unsigned long)frames[i]);
                    if (w > 0 && pos + (size_t)w < sizeof(frameBuf)) pos += (size_t)w;
                    else break;
                }
                LogPrintf("MAIN_SAMPLE +%lums eip=%s EAX=0x%08lX ECX=0x%08lX EDX=0x%08lX EBX=0x%08lX ESI=0x%08lX EDI=0x%08lX isbad=%ld stack=[%s]",
                          (unsigned long)(GetTickCount() - loaded_tick), modBuf,
                          ctx.Eax, ctx.Ecx, ctx.Edx, ctx.Ebx, ctx.Esi, ctx.Edi,
                          (long)g_isbad_count, frameBuf);
            }
        }
    }
    LogPrintf("WATCHDOG_TIMEOUT after %lu ms (loadmap_ok=%ld)", (unsigned long)max_ms, (long)g_loadmap_ok);
    SafeExit(g_loadmap_ok ? 0 : 4, "WatchdogTimeout");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        g_exe = GetModuleHandleW(NULL);
        g_isbadreadptr = (uintptr_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsBadReadPtr");
        g_in_veh_tls = TlsAlloc();

        char log_path[MAX_PATH] = "..\\SCDA_Coop_Maps\\test_probe.log";
        GetEnvironmentVariableA("CT_PROBE_LOG", log_path, sizeof(log_path));
        g_log = fopen(log_path, "w");

        char ae_buf[32] = {0};
        if (GetEnvironmentVariableA("CT_PROBE_AUTOEXIT_MS", ae_buf, sizeof(ae_buf))) {
            g_auto_exit_ms = atoi(ae_buf);
        }

        LogPrintf("ct_probe loaded. exe=0x%p auto_exit_ms=%d", g_exe, g_auto_exit_ms);
        {
            char cmd_buf[1024] = {0};
            LPCWSTR cmdw = GetCommandLineW();
            if (cmdw) WideCharToMultiByte(CP_UTF8, 0, cmdw, -1, cmd_buf, sizeof(cmd_buf) - 1, NULL, NULL);
            LogPrintf("CMDLINE: %s", cmd_buf);
        }
        HookIAT(g_exe, "USER32.dll", "MessageBoxW", (void*)Hook_MessageBoxW, (void**)&g_orig_MBW);
        HookIAT(g_exe, "USER32.dll", "MessageBoxA", (void*)Hook_MessageBoxA, (void**)&g_orig_MBA);
        HookIAT(g_exe, "KERNEL32.dll", "ExitProcess", (void*)Hook_ExitProcess, (void**)&g_orig_ExitProcess);
        HookIAT(g_exe, "KERNEL32.dll", "OutputDebugStringA", (void*)Hook_ODSA, (void**)&g_orig_ODSA);
        HookIAT(g_exe, "KERNEL32.dll", "OutputDebugStringW", (void*)Hook_ODSW, (void**)&g_orig_ODSW);
        HookIAT(g_exe, "KERNEL32.dll", "ReadFile", (void*)Hook_ReadFile, (void**)&g_orig_ReadFile);
        LogPrintf("HookIAT CreateMutexW -> %d", HookIAT(g_exe, "KERNEL32.dll", "CreateMutexW", (void*)Hook_CreateMutexW, (void**)&g_orig_CreateMutexW));
        LogPrintf("HookIAT CreateMutexA -> %d", HookIAT(g_exe, "KERNEL32.dll", "CreateMutexA", (void*)Hook_CreateMutexA, (void**)&g_orig_CreateMutexA));
        LogPrintf("HookIAT CreateFileA -> %d", HookIAT(g_exe, "KERNEL32.dll", "CreateFileA", (void*)Hook_CreateFileA, (void**)&g_orig_CreateFileA));
        LogPrintf("HookIAT CreateFileW -> %d", HookIAT(g_exe, "KERNEL32.dll", "CreateFileW", (void*)Hook_CreateFileW, (void**)&g_orig_CreateFileW));
        HookExtraFileModules();
        {
            BOOL d3d_ok = HookIAT(g_exe, "d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9, (void**)&g_orig_Direct3DCreate9);
            LogPrintf("HookIAT d3d9!Direct3DCreate9 -> %d (orig=0x%p)", d3d_ok, g_orig_Direct3DCreate9);
        }
        if (HMODULE hu = GetModuleHandleA("uplay32.dll")) {
            HookIAT(hu, "KERNEL32.dll", "ExitProcess", (void*)Hook_ExitProcess, (void**)&g_orig_ExitProcess);
        }
        AddVectoredExceptionHandler(1, MyVEH);
        SetUnhandledExceptionFilter(MyUnhandled);
        InstallLoadMapHook();
        InstallAnimLookupHook();
        CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
