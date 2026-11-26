#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static HMODULE g_real_ddraw = nullptr;
static HMODULE g_coop_net = nullptr;
static FARPROC g_pfn_DirectDrawCreateEx = nullptr;

static void LoadRealDdraw(HMODULE self_mod) {
    if (g_real_ddraw) return;
    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len + 12 < MAX_PATH) {
        lstrcatW(sys_dir, L"\\ddraw.dll");
        HMODULE mod = LoadLibraryW(sys_dir);
        if (mod && mod != self_mod) {
            g_real_ddraw = mod;
        }
    }
    if (!g_real_ddraw) return;
    g_pfn_DirectDrawCreateEx = GetProcAddress(g_real_ddraw, "DirectDrawCreateEx");
}

static void GetModuleDir(HMODULE mod, wchar_t* out, DWORD cap) {
    out[0] = L'\0';
    DWORD n = GetModuleFileNameW(mod, out, cap);
    if (n > 0 && n < cap) {
        for (DWORD i = n; i > 0; --i) {
            if (out[i - 1] == L'\\' || out[i - 1] == L'/') {
                out[i] = L'\0';
                return;
            }
        }
    }
    out[0] = L'\0';
}

static bool FileExistsW(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static DWORD WINAPI ProxyInitThread(LPVOID param) {
    HMODULE self_mod = static_cast<HMODULE>(param);
    wchar_t dir[MAX_PATH] = {};
    GetModuleDir(self_mod, dir, MAX_PATH);

    HMODULE coop_mod = GetModuleHandleW(L"ScctCoopNet.dll");
    if (!coop_mod && dir[0] != L'\0') {
        wchar_t coop_path[MAX_PATH] = {};
        if (lstrlenW(dir) + 20 < MAX_PATH) {
            lstrcpyW(coop_path, dir);
            lstrcatW(coop_path, L"ScctCoopNet.dll");
            if (FileExistsW(coop_path)) {
                coop_mod = LoadLibraryW(coop_path);
            }
        }
    }
    if (!coop_mod) {
        coop_mod = LoadLibraryW(L"ScctCoopNet.dll");
    }
    g_coop_net = coop_mod;
    if (coop_mod) {
        typedef int(__cdecl* PFN_ScctCoopNet_Install)();
        auto install_fn = reinterpret_cast<PFN_ScctCoopNet_Install>(
            GetProcAddress(coop_mod, "ScctCoopNet_Install"));
        if (install_fn) {
            install_fn();
        }
    }
    return 0;
}

extern "C" {
__declspec(naked) void DirectDrawCreateEx() { __asm { jmp g_pfn_DirectDrawCreateEx } }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadRealDdraw(hModule);
        HANDLE th = CreateThread(nullptr, 0, ProxyInitThread, hModule, 0, nullptr);
        if (th) {
            CloseHandle(th);
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_real_ddraw) {
            FreeLibrary(g_real_ddraw);
            g_real_ddraw = nullptr;
        }
    }
    return TRUE;
}
