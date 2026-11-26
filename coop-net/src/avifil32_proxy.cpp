#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

static void LoadRealAvifil32() {
    if (g_real_avifil32) return;
    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0 && len + 16 < MAX_PATH) {
        lstrcatW(sys_dir, L"\\AVIFIL32.dll");
        g_real_avifil32 = LoadLibraryW(sys_dir);
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

static void LoadCoopNet(HMODULE self_mod) {
    if (g_coop_net || GetModuleHandleW(L"ScctCoopNet.dll")) return;
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(self_mod, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        for (DWORD i = n; i > 0; --i) {
            if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
                path[i] = L'\0';
                break;
            }
        }
        if (lstrlenW(path) + 20 < MAX_PATH) {
            lstrcatW(path, L"ScctCoopNet.dll");
            g_coop_net = LoadLibraryW(path);
        }
    }
    if (!g_coop_net) {
        g_coop_net = LoadLibraryW(L"ScctCoopNet.dll");
    }
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
        DisableThreadLibraryCalls(hModule);
        LoadRealAvifil32();
        LoadCoopNet(hModule);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_real_avifil32) {
            FreeLibrary(g_real_avifil32);
            g_real_avifil32 = nullptr;
        }
    }
    return TRUE;
}
