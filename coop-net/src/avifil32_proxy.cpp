#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HMODULE g_self_mod = nullptr;
static HMODULE g_real_avifil32 = nullptr;
static HMODULE g_coop_net = nullptr;

static FARPROC g_pfn_AVIFileCreateStreamW = nullptr;
static FARPROC g_pfn_AVIFileExit = nullptr;
static FARPROC g_pfn_AVIFileInit = nullptr;
static FARPROC g_pfn_AVIFileOpenW = nullptr;
static FARPROC g_pfn_AVIFileRelease = nullptr;
static FARPROC g_pfn_AVIMakeCompressedStream = nullptr;
static FARPROC g_pfn_AVISaveOptions = nullptr;
static FARPROC g_pfn_AVIStreamRelease = nullptr;
static FARPROC g_pfn_AVIStreamSetFormat = nullptr;
static FARPROC g_pfn_AVIStreamWrite = nullptr;

static void LoadRealAvifil32(HMODULE self_mod) {
    if (g_real_avifil32) return;
    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len + 16 < MAX_PATH) {
        lstrcatW(sys_dir, L"\\AVIFIL32.dll");
        HMODULE mod = LoadLibraryW(sys_dir);
        if (mod && mod != self_mod) {
            g_real_avifil32 = mod;
        }
    }
    if (!g_real_avifil32) return;
    g_pfn_AVIFileCreateStreamW = GetProcAddress(g_real_avifil32, "AVIFileCreateStreamW");
    g_pfn_AVIFileExit = GetProcAddress(g_real_avifil32, "AVIFileExit");
    g_pfn_AVIFileInit = GetProcAddress(g_real_avifil32, "AVIFileInit");
    g_pfn_AVIFileOpenW = GetProcAddress(g_real_avifil32, "AVIFileOpenW");
    g_pfn_AVIFileRelease = GetProcAddress(g_real_avifil32, "AVIFileRelease");
    g_pfn_AVIMakeCompressedStream = GetProcAddress(g_real_avifil32, "AVIMakeCompressedStream");
    g_pfn_AVISaveOptions = GetProcAddress(g_real_avifil32, "AVISaveOptions");
    g_pfn_AVIStreamRelease = GetProcAddress(g_real_avifil32, "AVIStreamRelease");
    g_pfn_AVIStreamSetFormat = GetProcAddress(g_real_avifil32, "AVIStreamSetFormat");
    g_pfn_AVIStreamWrite = GetProcAddress(g_real_avifil32, "AVIStreamWrite");
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
    if (dir[0] == L'\0') {
        return 0;
    }

    // 1. Load Ultimate ASI Loader (msvfw32.dll / msacm32.dll) if present next to the EXE
    //    because our System\AVIFIL32.dll proxy intercepts the import that would normally load msvfw32.dll.
    const wchar_t* asi_loaders[] = {L"msvfw32.dll", L"msacm32.dll"};
    for (const wchar_t* loader_name : asi_loaders) {
        if (GetModuleHandleW(loader_name) == nullptr) {
            wchar_t loader_path[MAX_PATH] = {};
            if (lstrlenW(dir) + lstrlenW(loader_name) + 2 < MAX_PATH) {
                lstrcpyW(loader_path, dir);
                lstrcatW(loader_path, loader_name);
                if (FileExistsW(loader_path)) {
                    LoadLibraryW(loader_path);
                }
            }
        }
    }

    // 2. Load and activate ScctCoopNet.dll.
    HMODULE coop_mod = GetModuleHandleW(L"ScctCoopNet.dll");
    if (!coop_mod) {
        wchar_t coop_path[MAX_PATH] = {};
        if (lstrlenW(dir) + 20 < MAX_PATH) {
            lstrcpyW(coop_path, dir);
            lstrcatW(coop_path, L"ScctCoopNet.dll");
            if (FileExistsW(coop_path)) {
                coop_mod = LoadLibraryW(coop_path);
            }
        }
        if (!coop_mod) {
            coop_mod = LoadLibraryW(L"ScctCoopNet.dll");
        }
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

    // 3. Fallback: if WidescreenFix.asi is enabled on disk but Ultimate ASI Loader
    //    did not load it (e.g. msvfw32.dll absent or skipped due to delayed entry), load it directly.
    Sleep(200);
    if (GetModuleHandleW(L"SplinterCellChaosTheory.WidescreenFix.asi") == nullptr) {
        wchar_t asi_path[MAX_PATH] = {};
        const wchar_t* rel_asi = L"scripts\\SplinterCellChaosTheory.WidescreenFix.asi";
        if (lstrlenW(dir) + lstrlenW(rel_asi) + 2 < MAX_PATH) {
            lstrcpyW(asi_path, dir);
            lstrcatW(asi_path, rel_asi);
            if (FileExistsW(asi_path)) {
                LoadLibraryW(asi_path);
            }
        }
    }

    return 0;
}

extern "C" {
__declspec(naked) void AVIFileCreateStreamW() { __asm { jmp g_pfn_AVIFileCreateStreamW } }
__declspec(naked) void AVIFileExit() { __asm { jmp g_pfn_AVIFileExit } }
__declspec(naked) void AVIFileInit() { __asm { jmp g_pfn_AVIFileInit } }
__declspec(naked) void AVIFileOpenW() { __asm { jmp g_pfn_AVIFileOpenW } }
__declspec(naked) void AVIFileRelease() { __asm { jmp g_pfn_AVIFileRelease } }
__declspec(naked) void AVIMakeCompressedStream() { __asm { jmp g_pfn_AVIMakeCompressedStream } }
__declspec(naked) void AVISaveOptions() { __asm { jmp g_pfn_AVISaveOptions } }
__declspec(naked) void AVIStreamRelease() { __asm { jmp g_pfn_AVIStreamRelease } }
__declspec(naked) void AVIStreamSetFormat() { __asm { jmp g_pfn_AVIStreamSetFormat } }
__declspec(naked) void AVIStreamWrite() { __asm { jmp g_pfn_AVIStreamWrite } }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_self_mod = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadRealAvifil32(hModule);
        HANDLE th = CreateThread(nullptr, 0, ProxyInitThread, hModule, 0, nullptr);
        if (th) {
            CloseHandle(th);
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_real_avifil32) {
            FreeLibrary(g_real_avifil32);
            g_real_avifil32 = nullptr;
        }
    }
    return TRUE;
}
