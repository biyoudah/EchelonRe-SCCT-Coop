// ScctCoopNet.dll
// Loaded only by scct-coop-launch.
// enabled=1 journals Winsock and does not rewrite LAN addresses.
// transport=1 copies UDP 9100/9103 payloads unchanged to the relay named in scct-coop-net.ini.
// transport=0 sends nothing and rewrites nothing. enabled=0 with transport=0 installs no hook.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <bcrypt.h>
#include <intrin.h>
#include <mmsystem.h>
#include <winhttp.h>
#include "voice_audio.h"

#include "scda_hooks.h"
#include "transport.h"

#include <MinHook.h>

#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {

constexpr unsigned long long kExpectedSizeRetail = 9867264ull;
constexpr unsigned long long kExpectedSizeUbi = 9877160ull;
constexpr uintptr_t kExpectedBase = 0x10900000u;
constexpr int kHexBytes = 32;
constexpr int kDiscoveryHex = 384;
constexpr unsigned short kDiscoveryPort = 9100;
constexpr unsigned short kSessionPort = 9103;
constexpr LONG kMaxLines = 4096;
constexpr uint8_t kExpectedMd5Retail[16] = {
    0x9a, 0x65, 0xe3, 0x26, 0xdf, 0x1b, 0x27, 0x3c,
    0xe2, 0x4e, 0xae, 0x90, 0x4b, 0xea, 0xbf, 0xb2,
};
constexpr uint8_t kExpectedMd5Ubi[16] = {
    0x33, 0xc0, 0x95, 0x1a, 0x4a, 0x80, 0xf2, 0xa9,
    0x7d, 0xaf, 0x3e, 0x30, 0x2c, 0x53, 0x42, 0x32,
};

bool IsReadableRange(const void* ptr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<uintptr_t>(ptr);
    const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + len <= region_end;
}

bool IsCompatibleV105Image(uintptr_t base) {
    if (base != kExpectedBase) {
        return false;
    }
    static const uint8_t kTickBytes[8] = {0x83, 0xEC, 0x1C, 0x53, 0x55, 0x56, 0x8B, 0xF1};
    static const uint8_t kAvailBytes[10] = {0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xD9};
    static const uint8_t kInitBytes[10] = {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x8B, 0xF1, 0x57};

    const auto* tick_ptr = reinterpret_cast<const uint8_t*>(base + 0x143920u);
    const auto* avail_ptr = reinterpret_cast<const uint8_t*>(base + 0x301BF0u);
    const auto* init_ptr = reinterpret_cast<const uint8_t*>(base + 0x302580u);
    const auto* vtable_ptr = reinterpret_cast<const uintptr_t*>(base + 0x7D6200u);

    if (!IsReadableRange(tick_ptr, sizeof(kTickBytes)) ||
        !IsReadableRange(avail_ptr, sizeof(kAvailBytes)) ||
        !IsReadableRange(init_ptr, sizeof(kInitBytes)) ||
        !IsReadableRange(vtable_ptr, sizeof(uintptr_t))) {
        return false;
    }
    return std::memcmp(tick_ptr, kTickBytes, sizeof(kTickBytes)) == 0 &&
           std::memcmp(avail_ptr, kAvailBytes, sizeof(kAvailBytes)) == 0 &&
           std::memcmp(init_ptr, kInitBytes, sizeof(kInitBytes)) == 0 &&
           *vtable_ptr == base + 0x143920u;
}

using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WsaSendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WsaOverlappedResultFn = BOOL(WSAAPI*)(SOCKET, LPWSAOVERLAPPED, LPDWORD, BOOL, LPDWORD);
using WsaRecvFromFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using CreateMutexWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
using CreateMutexAFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
using Direct3DCreate9Fn = void*(WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, void**);

HMODULE g_module = nullptr;
CRITICAL_SECTION g_log_lock{};
bool g_log_ready = false;
HANDLE g_log = INVALID_HANDLE_VALUE;
LONG g_lines = 0;
bool g_capped = false;
volatile LONG g_seq = 0;
bool g_hooks = false;
bool g_journal = false;
uint32_t g_local_ip_be = 0;
bool g_signature_ok = false;
wchar_t g_custom_name[20]{};
int g_default_ffire = 0;
int g_enable_voice = 1;
int g_voice_test_level = 0;

struct PreserveError {
    int wsa;
    DWORD win;
    PreserveError() : wsa(WSAGetLastError()), win(GetLastError()) {}
    ~PreserveError() {
        WSASetLastError(wsa);
        SetLastError(win);
    }
};

BindFn g_bind = nullptr;
SendToFn g_sendto = nullptr;
RecvFromFn g_recvfrom = nullptr;
WsaSendToFn g_WSASendTo = nullptr;
WsaRecvFromFn g_WSARecvFrom = nullptr;
WsaOverlappedResultFn g_WSAGetOverlappedResult = nullptr;
CreateMutexWFn g_CreateMutexW = nullptr;
CreateMutexAFn g_CreateMutexA = nullptr;
Direct3DCreate9Fn g_Direct3DCreate9 = nullptr;
Direct3DCreate9ExFn g_Direct3DCreate9Ex = nullptr;

constexpr uint32_t kD3DPoolDefault = 0;
constexpr uint32_t kD3DPoolManaged = 1;
constexpr DWORD kD3DUsageDynamic = 0x00000200u;

using CreateDeviceFn = HRESULT(__stdcall*)(void*, UINT, UINT, HWND, DWORD, void*, void**);
using CreateTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, UINT, uint32_t, void**, HANDLE*);
using CreateVolumeTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, UINT, DWORD, UINT, uint32_t, void**, HANDLE*);
using CreateCubeTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, UINT, uint32_t, void**, HANDLE*);
using CreateVertexBufferFn = HRESULT(__stdcall*)(void*, UINT, DWORD, DWORD, uint32_t, void**, HANDLE*);
using CreateIndexBufferFn = HRESULT(__stdcall*)(void*, UINT, DWORD, UINT, uint32_t, void**, HANDLE*);

CreateDeviceFn g_CreateDevice = nullptr;
CreateTextureFn g_CreateTexture = nullptr;
CreateVolumeTextureFn g_CreateVolumeTexture = nullptr;
CreateCubeTextureFn g_CreateCubeTexture = nullptr;
CreateVertexBufferFn g_CreateVertexBuffer = nullptr;
CreateIndexBufferFn g_CreateIndexBuffer = nullptr;

HRESULT __stdcall Hook_CreateTexture(void* dev, UINT width, UINT height, UINT levels, DWORD usage, UINT fmt,
                                     uint32_t pool, void** ppTex, HANDLE* pShared) {
    if (pool == kD3DPoolManaged) {
        pool = kD3DPoolDefault;
        usage |= kD3DUsageDynamic;
    }
    return g_CreateTexture(dev, width, height, levels, usage, fmt, pool, ppTex, pShared);
}

HRESULT __stdcall Hook_CreateVolumeTexture(void* dev, UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                           UINT fmt, uint32_t pool, void** ppVol, HANDLE* pShared) {
    if (pool == kD3DPoolManaged) {
        pool = kD3DPoolDefault;
        usage |= kD3DUsageDynamic;
    }
    return g_CreateVolumeTexture(dev, width, height, depth, levels, usage, fmt, pool, ppVol, pShared);
}

HRESULT __stdcall Hook_CreateCubeTexture(void* dev, UINT edge, UINT levels, DWORD usage, UINT fmt, uint32_t pool,
                                         void** ppCube, HANDLE* pShared) {
    if (pool == kD3DPoolManaged) {
        pool = kD3DPoolDefault;
        usage |= kD3DUsageDynamic;
    }
    return g_CreateCubeTexture(dev, edge, levels, usage, fmt, pool, ppCube, pShared);
}

HRESULT __stdcall Hook_CreateVertexBuffer(void* dev, UINT length, DWORD usage, DWORD fvf, uint32_t pool, void** ppVB,
                                          HANDLE* pShared) {
    if (pool == kD3DPoolManaged) {
        pool = kD3DPoolDefault;
        usage |= kD3DUsageDynamic;
    }
    return g_CreateVertexBuffer(dev, length, usage, fvf, pool, ppVB, pShared);
}

HRESULT __stdcall Hook_CreateIndexBuffer(void* dev, UINT length, DWORD usage, UINT fmt, uint32_t pool, void** ppIB,
                                         HANDLE* pShared) {
    if (pool == kD3DPoolManaged) {
        pool = kD3DPoolDefault;
        usage |= kD3DUsageDynamic;
    }
    return g_CreateIndexBuffer(dev, length, usage, fmt, pool, ppIB, pShared);
}

wchar_t g_shot_dir[MAX_PATH]{};
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFn g_Present = nullptr;

void SaveBackBufferBmp(IDirect3DDevice9* dev, const wchar_t* path) {
    if (!dev || !path || path[0] == L'\0') {
        return;
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        return;
    }
    D3DSURFACE_DESC desc{};
    if (FAILED(bb->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0 ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) {
        bb->Release();
        return;
    }
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        !sys) {
        bb->Release();
        return;
    }
    if (SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY)) && locked.pBits != nullptr) {
            const uint32_t w = desc.Width;
            const uint32_t h = desc.Height;
            const uint32_t stride = (w * 3 + 3u) & ~3u;
            const uint32_t img_bytes = stride * h;
            BITMAPFILEHEADER bfh{};
            bfh.bfType = 0x4D42;
            bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + img_bytes;
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            BITMAPINFOHEADER bih{};
            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = static_cast<LONG>(w);
            bih.biHeight = static_cast<LONG>(h);
            bih.biPlanes = 1;
            bih.biBitCount = 24;
            bih.biCompression = BI_RGB;
            bih.biSizeImage = img_bytes;

            std::wstring tmp_path = std::wstring(path) + L".tmp";
            HANDLE f = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(f, &bfh, sizeof(bfh), &written, nullptr);
                WriteFile(f, &bih, sizeof(bih), &written, nullptr);
                std::vector<uint8_t> row(stride, 0);
                for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
                    const uint8_t* src = static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        row[x * 3 + 0] = src[x * 4 + 0];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + 2];
                    }
                    WriteFile(f, row.data(), stride, &written, nullptr);
                }
                CloseHandle(f);
                MoveFileExW(tmp_path.c_str(), path, MOVEFILE_REPLACE_EXISTING);
            }
            sys->UnlockRect();
        }
    }
    sys->Release();
    bb->Release();
}

void Logf(const char* fmt, ...);

static bool s_time_period_active = false;

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND hWnd,
                                       const RGNDATA* dirty) {
    if (g_shot_dir[0] != L'\0') {
        static uint64_t last_check = 0;
        const uint64_t now = GetTickCount64();
        if (now - last_check >= 150) {
            last_check = now;
            wchar_t req_path[MAX_PATH]{};
            _snwprintf_s(req_path, MAX_PATH, _TRUNCATE, L"%ls\\shot_%lu.req", g_shot_dir, GetCurrentProcessId());
            if (GetFileAttributesW(req_path) != INVALID_FILE_ATTRIBUTES) {
                char target_utf8[MAX_PATH * 2]{};
                DWORD got = 0;
                HANDLE rf = CreateFileW(req_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (rf != INVALID_HANDLE_VALUE) {
                    ReadFile(rf, target_utf8, sizeof(target_utf8) - 1, &got, nullptr);
                    CloseHandle(rf);
                }
                DeleteFileW(req_path);
                while (got > 0 && (target_utf8[got - 1] == '\r' || target_utf8[got - 1] == '\n' ||
                                   target_utf8[got - 1] == ' ')) {
                    --got;
                }
                target_utf8[got] = '\0';
                if (got > 0) {
                    wchar_t target_w[MAX_PATH]{};
                    if (MultiByteToWideChar(CP_UTF8, 0, target_utf8, -1, target_w, MAX_PATH) > 0) {
                        SaveBackBufferBmp(dev, target_w);
                    }
                }
            }
        }
    }
    // Configurable frame-rate cap (SCCT_MAX_FPS, default 60; 0 disables) to prevent
    // high-FPS physics/audio desync in Splinter Cell Chaos Theory.
    {
        static int s_max_fps = -1;
        static LARGE_INTEGER s_freq{};
        static LARGE_INTEGER s_next_frame{};
        if (s_max_fps < 0) {
            char fps_buf[32]{};
            const DWORD fn = GetEnvironmentVariableA("SCCT_MAX_FPS", fps_buf, sizeof(fps_buf));
            s_max_fps = (fn > 0 && fn < sizeof(fps_buf)) ? std::atoi(fps_buf) : 60;
            if (s_max_fps < 0) {
                s_max_fps = 0;
            }
            if (s_max_fps > 0) {
                timeBeginPeriod(1);
                s_time_period_active = true;
            }
            QueryPerformanceFrequency(&s_freq);
            QueryPerformanceCounter(&s_next_frame);
            const int ws_present = GetModuleHandleW(L"SplinterCellChaosTheory.WidescreenFix.asi") != nullptr ? 1 : 0;
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=fps_cap_init max_fps=%d widescreen_present=%d",
                 GetCurrentProcessId(), s_max_fps, ws_present);
        }
        if (s_max_fps > 0 && s_freq.QuadPart > 0) {
            const LONGLONG frame_ticks = s_freq.QuadPart / s_max_fps;
            LARGE_INTEGER now_qpc{};
            QueryPerformanceCounter(&now_qpc);
            if (now_qpc.QuadPart < s_next_frame.QuadPart) {
                const LONGLONG rem_ms = ((s_next_frame.QuadPart - now_qpc.QuadPart) * 1000LL) / s_freq.QuadPart;
                if (rem_ms > 2) {
                    Sleep(static_cast<DWORD>(rem_ms - 1));
                }
                do {
                    QueryPerformanceCounter(&now_qpc);
                } while (now_qpc.QuadPart < s_next_frame.QuadPart);
            }
            s_next_frame.QuadPart = now_qpc.QuadPart + frame_ticks;
        }
    }
    return g_Present(dev, src, dst, hWnd, dirty);
}

HRESULT __stdcall Hook_CreateDevice(void* d3d, UINT adapter, UINT devType, HWND focusHwnd, DWORD behaviorFlags,
                                    void* pp, void** ppReturnedDevice) {
    const HRESULT hr = g_CreateDevice(d3d, adapter, devType, focusHwnd, behaviorFlags, pp, ppReturnedDevice);
    if (SUCCEEDED(hr) && ppReturnedDevice != nullptr && *ppReturnedDevice != nullptr) {
        static LONG dev_hooks_done = 0;
        if (InterlockedCompareExchange(&dev_hooks_done, 1, 0) == 0) {
            void** vtbl = *reinterpret_cast<void***>(*ppReturnedDevice);
            if (vtbl != nullptr) {
                struct Slot {
                    int idx;
                    void* detour;
                    void** orig;
                } slots[] = {
                    {23, reinterpret_cast<void*>(&Hook_CreateTexture), reinterpret_cast<void**>(&g_CreateTexture)},
                    {24, reinterpret_cast<void*>(&Hook_CreateVolumeTexture),
                     reinterpret_cast<void**>(&g_CreateVolumeTexture)},
                    {25, reinterpret_cast<void*>(&Hook_CreateCubeTexture),
                     reinterpret_cast<void**>(&g_CreateCubeTexture)},
                    {26, reinterpret_cast<void*>(&Hook_CreateVertexBuffer),
                     reinterpret_cast<void**>(&g_CreateVertexBuffer)},
                    {27, reinterpret_cast<void*>(&Hook_CreateIndexBuffer),
                     reinterpret_cast<void**>(&g_CreateIndexBuffer)},
                };
                for (const Slot& s : slots) {
                    if (vtbl[s.idx] != nullptr && MH_CreateHook(vtbl[s.idx], s.detour, s.orig) == MH_OK) {
                        MH_EnableHook(vtbl[s.idx]);
                    }
                }
                if (vtbl[17] != nullptr &&
                    MH_CreateHook(vtbl[17], reinterpret_cast<void*>(&Hook_Present),
                                  reinterpret_cast<void**>(&g_Present)) == MH_OK) {
                    MH_EnableHook(vtbl[17]);
                }
            }
        }
    }
    return hr;
}

void* WINAPI Hook_Direct3DCreate9(UINT sdkVersion) {
    if (g_Direct3DCreate9Ex != nullptr) {
        void* d3dex = nullptr;
        if (SUCCEEDED(g_Direct3DCreate9Ex(sdkVersion, &d3dex)) && d3dex != nullptr) {
            static LONG create_dev_hooked = 0;
            if (InterlockedCompareExchange(&create_dev_hooked, 1, 0) == 0) {
                void** vtbl = *reinterpret_cast<void***>(d3dex);
                if (vtbl != nullptr && vtbl[16] != nullptr &&
                    MH_CreateHook(vtbl[16], reinterpret_cast<void*>(&Hook_CreateDevice),
                                  reinterpret_cast<void**>(&g_CreateDevice)) == MH_OK) {
                    MH_EnableHook(vtbl[16]);
                }
            }
            return d3dex;
        }
    }
    void* d3d = g_Direct3DCreate9 ? g_Direct3DCreate9(sdkVersion) : nullptr;
    if (d3d != nullptr) {
        static LONG create_dev_hooked = 0;
        if (InterlockedCompareExchange(&create_dev_hooked, 1, 0) == 0) {
            void** vtbl = *reinterpret_cast<void***>(d3d);
            if (vtbl != nullptr && vtbl[16] != nullptr &&
                MH_CreateHook(vtbl[16], reinterpret_cast<void*>(&Hook_CreateDevice),
                              reinterpret_cast<void**>(&g_CreateDevice)) == MH_OK) {
                MH_EnableHook(vtbl[16]);
            }
        }
    }
    return d3d;
}

bool AllowMultiInstance() {
    static LONG s_cached = -1;
    LONG v = InterlockedCompareExchange(&s_cached, -1, -1);
    if (v < 0) {
        wchar_t buf[16]{};
        const DWORD n = GetEnvironmentVariableW(L"SCCT_ALLOW_MULTI_INSTANCE", buf, _countof(buf));
        v = (n > 0 && std::wcscmp(buf, L"1") == 0) ? 1 : 0;
        InterlockedExchange(&s_cached, v);
    }
    return v == 1;
}

HANDLE WINAPI Hook_CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name) {
    if (AllowMultiInstance() && name != nullptr && _wcsnicmp(name, L"SplinterCell3", 13) == 0) {
        wchar_t unique[256];
        _snwprintf_s(unique, sizeof(unique) / sizeof(unique[0]), _TRUNCATE, L"%ls_%lu", name, GetCurrentProcessId());
        return g_CreateMutexW(sa, initialOwner, unique);
    }
    return g_CreateMutexW(sa, initialOwner, name);
}

HANDLE WINAPI Hook_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name) {
    if (AllowMultiInstance() && name != nullptr && _strnicmp(name, "SplinterCell3", 13) == 0) {
        char unique[256];
        std::snprintf(unique, sizeof(unique), "%s_%lu", name, GetCurrentProcessId());
        return g_CreateMutexA(sa, initialOwner, unique);
    }
    return g_CreateMutexA(sa, initialOwner, name);
}

std::wstring ModuleFile(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

void HexBytes(const uint8_t* data, int count, char* out, size_t out_len) {
    static const char* digits = "0123456789abcdef";
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count; ++i) {
        if (used + 4 >= out_len) {
            break;
        }
        if (i != 0) {
            out[used++] = ' ';
        }
        out[used++] = digits[data[i] >> 4];
        out[used++] = digits[data[i] & 0x0F];
        out[used] = '\0';
    }
}

void Md5Text(const uint8_t md5[16], char* out) {
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = digits[md5[i] >> 4];
        out[i * 2 + 1] = digits[md5[i] & 0x0F];
    }
    out[32] = '\0';
}

void NowText(char* out, size_t out_len) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(out, out_len, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, st.wMilliseconds);
}

void WriteLine(const char* line) {
    if (!g_log_ready) {
        OutputDebugStringA(line);
        return;
    }
    EnterCriticalSection(&g_log_lock);
    if (g_capped) {
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    if (g_lines >= kMaxLines) {
        g_capped = true;
        char capped[256];
        char heure[40];
        NowText(heure, sizeof(heure));
        std::snprintf(capped, sizeof(capped),
                      "heure=%s pid=%lu dir=plafond local=- pair=- len=0 hex= seq=0 tronque=1 "
                      "note=journal coupe a %ld lignes\r\n",
                      heure, GetCurrentProcessId(), kMaxLines);
        if (g_log != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(g_log, capped, static_cast<DWORD>(std::strlen(capped)), &wrote, nullptr);
            FlushFileBuffers(g_log);
        }
        LeaveCriticalSection(&g_log_lock);
        return;
    }
    ++g_lines;
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_log, line, static_cast<DWORD>(std::strlen(line)), &wrote, nullptr);
        const char nl[] = "\r\n";
        WriteFile(g_log, nl, 2, &wrote, nullptr);
        FlushFileBuffers(g_log);
    } else {
        OutputDebugStringA(line);
    }
    LeaveCriticalSection(&g_log_lock);
}

void Logf(const char* fmt, ...) {
    char line[2200];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    va_end(args);
    WriteLine(line);
}

bool OpenLog(const std::wstring& path) {
    if (!g_log_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_ready = true;
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_log != INVALID_HANDLE_VALUE;
}

int CopyBytes(const void* src, int len, uint8_t* dst, int cap) {
    if (!src || len <= 0 || cap <= 0) {
        return 0;
    }
    const int n = len < cap ? len : cap;
    __try {
        std::memcpy(dst, src, static_cast<size_t>(n));
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void FormatSockaddr(const sockaddr* addr, int len, char* out, size_t out_len) {
    if (!addr || len < static_cast<int>(sizeof(unsigned short))) {
        std::snprintf(out, out_len, "-");
        return;
    }
    unsigned short family = 0;
    if (CopyBytes(addr, static_cast<int>(sizeof(family)), reinterpret_cast<uint8_t*>(&family), sizeof(family)) < 0) {
        std::snprintf(out, out_len, "illisible");
        return;
    }
    if (family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in sa{};
        if (CopyBytes(addr, static_cast<int>(sizeof(sa)), reinterpret_cast<uint8_t*>(&sa), sizeof(sa)) < 0) {
            std::snprintf(out, out_len, "illisible");
            return;
        }
        const unsigned char* ip = reinterpret_cast<const unsigned char*>(&sa.sin_addr);
        std::snprintf(out, out_len, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(sa.sin_port));
        return;
    }
    std::snprintf(out, out_len, "famille_%u", family);
}

void LocalEndpoint(SOCKET sock, char* out, size_t out_len) {
    sockaddr_in sa{};
    int len = sizeof(sa);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&sa), &len) != 0 || sa.sin_family != AF_INET) {
        std::snprintf(out, out_len, "-");
        return;
    }
    const unsigned char* ip = reinterpret_cast<const unsigned char*>(&sa.sin_addr);
    std::snprintf(out, out_len, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(sa.sin_port));
}

void LogCall(const char* dir, SOCKET sock, const sockaddr* peer, int peer_len, int len, const uint8_t* preview,
             int preview_n, bool truncated, bool unreadable, int result, const char* note) {
    if (!g_journal) {
        return;
    }
    char heure[40];
    char local[64];
    char pair[64];
    char hex[kDiscoveryHex * 3 + 4];
    NowText(heure, sizeof(heure));
    LocalEndpoint(sock, local, sizeof(local));
    FormatSockaddr(peer, peer_len, pair, sizeof(pair));
    if (unreadable) {
        std::snprintf(hex, sizeof(hex), "illisible");
    } else if (preview_n <= 0) {
        hex[0] = '\0';
    } else {
        HexBytes(preview, preview_n, hex, sizeof(hex));
    }
    const LONG seq = InterlockedIncrement(&g_seq);
    Logf("heure=%s pid=%lu dir=%s local=%s pair=%s len=%d hex=%s seq=%ld tronque=%d resultat=%d note=%s", heure,
         GetCurrentProcessId(), dir, local, pair, len, hex, seq, truncated ? 1 : 0, result, note ? note : "-");
}

int PreviewOne(const void* data, int len, uint8_t* out, int cap, bool* truncated, bool* unreadable) {
    *truncated = len > cap;
    const int n = CopyBytes(data, len, out, cap);
    *unreadable = n < 0;
    return n < 0 ? 0 : n;
}

unsigned short InetPort(const sockaddr* addr, int len) {
    sockaddr_in sa{};
    if (!addr || len < static_cast<int>(sizeof(sa))) {
        return 0;
    }
    if (CopyBytes(addr, static_cast<int>(sizeof(sa)), reinterpret_cast<uint8_t*>(&sa), sizeof(sa)) < 0) {
        return 0;
    }
    if (sa.sin_family != AF_INET) {
        return 0;
    }
    return ntohs(sa.sin_port);
}

unsigned short LocalPort(SOCKET sock) {
    sockaddr_in sa{};
    int len = sizeof(sa);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&sa), &len) != 0 || sa.sin_family != AF_INET) {
        return 0;
    }
    return ntohs(sa.sin_port);
}

// LAN discovery uses UDP port 9100. The session port 9103 stays on the short preview.
const char* DiscoveryNote(SOCKET sock, const sockaddr* peer, int peer_len) {
    if (InetPort(peer, peer_len) == kDiscoveryPort || LocalPort(sock) == kDiscoveryPort) {
        return "decouverte_9100";
    }
    return "-";
}

int PreviewCap(const char* note) {
    return note != nullptr && note[0] != '-' ? kDiscoveryHex : kHexBytes;
}

int PreviewBuffers(LPWSABUF buffers, DWORD count, uint8_t* out, int cap, bool* truncated, bool* unreadable) {
    int filled = 0;
    int total = 0;
    *truncated = false;
    *unreadable = false;
    if (!buffers || count == 0) {
        return 0;
    }
    for (DWORD i = 0; i < count; ++i) {
        int chunk = 0;
        if (CopyBytes(&buffers[i].len, sizeof(chunk), reinterpret_cast<uint8_t*>(&chunk), sizeof(chunk)) < 0) {
            *unreadable = true;
            return filled;
        }
        if (chunk < 0) {
            break;
        }
        total += chunk;
        if (filled >= cap) {
            continue;
        }
        char* ptr = nullptr;
        if (CopyBytes(&buffers[i].buf, sizeof(ptr), reinterpret_cast<uint8_t*>(&ptr), sizeof(ptr)) < 0) {
            *unreadable = true;
            return filled;
        }
        const int room = cap - filled;
        const int n = CopyBytes(ptr, chunk, out + filled, room);
        if (n < 0) {
            *unreadable = true;
            return filled;
        }
        filled += n;
    }
    *truncated = total > cap;
    return filled;
}

unsigned short GameLanPort(SOCKET sock, const sockaddr* peer, int peer_len);
int GatherAll(LPWSABUF buffers, DWORD count, uint8_t* out, int cap);
void CopyGamePayload(SOCKET sock, unsigned short game_port, const void* data, int len);

int WSAAPI Hook_bind(SOCKET sock, const sockaddr* addr, int namelen) {
    sockaddr_in override_addr{};
    const sockaddr* use_addr = addr;
    int use_len = namelen;
    if (g_local_ip_be != 0 && addr != nullptr && namelen >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in sa{};
        if (CopyBytes(addr, sizeof(sa), reinterpret_cast<uint8_t*>(&sa), sizeof(sa)) == sizeof(sa) &&
            sa.sin_family == AF_INET) {
            const unsigned short p = ntohs(sa.sin_port);
            if (p == kDiscoveryPort || p == kSessionPort) {
                override_addr = sa;
                override_addr.sin_addr.s_addr = g_local_ip_be;
                use_addr = reinterpret_cast<const sockaddr*>(&override_addr);
                use_len = sizeof(override_addr);
            }
        }
    }
    const int result = g_bind(sock, use_addr, use_len);
    PreserveError saved;
    if (result == 0) {
        if (g_local_ip_be != 0) {
            BOOL off = FALSE;
            DWORD ignored = 0;
            WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
        }
        TransportNoteSocket(sock);
    }
    uint8_t preview[kHexBytes];
    bool truncated = false;
    bool unreadable = false;
    const int n = PreviewOne(use_addr, use_len, preview, kHexBytes, &truncated, &unreadable);
    LogCall("bind", sock, use_addr, use_len, use_len, preview, n, truncated, unreadable, result,
            DiscoveryNote(sock, use_addr, use_len));
    return result;
}

int WSAAPI Hook_sendto(SOCKET sock, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    uint8_t preview[kDiscoveryHex];
    bool truncated = false;
    bool unreadable = false;
    const int n = PreviewOne(buf, len, preview, kDiscoveryHex, &truncated, &unreadable);
    const unsigned short game_port = GameLanPort(sock, to, tolen);
    if (TransportActive() && !TransportSending() && game_port != 0) {
        TransportNoteOutboundTarget(game_port, to, tolen);
    }
    sockaddr_in discard{};
    const sockaddr* use_to = to;
    int use_tolen = tolen;
    const bool redirect_discard =
        g_local_ip_be != 0 && TransportActive() && !TransportSending() && game_port != 0;
    if (redirect_discard) {
        discard.sin_family = AF_INET;
        discard.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        discard.sin_port = htons(9);
        use_to = reinterpret_cast<const sockaddr*>(&discard);
        use_tolen = sizeof(discard);
    }
    const int result = g_sendto(sock, buf, len, flags, use_to, use_tolen);
    PreserveError saved;
    if (redirect_discard) {
        BOOL off = FALSE;
        DWORD ignored = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    }
    CopyGamePayload(sock, game_port, buf, len);
    const char* note = DiscoveryNote(sock, to, tolen);
    const int cap = PreviewCap(note);
    const int shown = n > cap ? cap : n;
    LogCall("sendto", sock, to, tolen, len, preview, shown, truncated || n > cap, unreadable, result, note);
    return result;
}

int WSAAPI Hook_recvfrom(SOCKET sock, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    const int result = g_recvfrom(sock, buf, len, flags, from, fromlen);
    PreserveError saved;
    if (kTransportCut != 1 && result > 0 && buf != nullptr && from != nullptr && fromlen != nullptr) {
        TransportNoteSocket(sock);
        sockaddr_in peer{};
        if (TransportTakePeer(sock, from, *fromlen, &peer)) {
            int stored = *fromlen;
            ScctCoopNet_Deposit(buf, result, reinterpret_cast<const sockaddr*>(&peer), static_cast<int>(sizeof(peer)),
                                buf, len, from, &stored);
            *fromlen = stored;
        }
    }
    uint8_t preview[kDiscoveryHex];
    bool truncated = false;
    bool unreadable = false;
    const int got = result > 0 ? result : 0;
    int peer_len = 0;
    if (fromlen) {
        CopyBytes(fromlen, sizeof(peer_len), reinterpret_cast<uint8_t*>(&peer_len), sizeof(peer_len));
    }
    const char* note = DiscoveryNote(sock, from, peer_len);
    const int cap = PreviewCap(note);
    const int n = PreviewOne(buf, got, preview, cap, &truncated, &unreadable);
    LogCall("recvfrom", sock, from, peer_len, got, preview, n, truncated, unreadable, result, note);
    return result;
}

int WSAAPI Hook_WSASendTo(SOCKET sock, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to,
                          int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE complete) {
    uint8_t preview[kDiscoveryHex];
    bool truncated = false;
    bool unreadable = false;
    const int n = PreviewBuffers(buffers, count, preview, kDiscoveryHex, &truncated, &unreadable);
    thread_local uint8_t gathered[8192];
    int gathered_n = 0;
    const unsigned short game_port = GameLanPort(sock, to, tolen);
    if (TransportActive() && !TransportSending() && game_port != 0) {
        TransportNoteSocket(sock);
        TransportNoteOutboundTarget(game_port, to, tolen);
        gathered_n = GatherAll(buffers, count, gathered, 8192);
    }
    int total = 0;
    if (buffers) {
        for (DWORD i = 0; i < count; ++i) {
            int chunk = 0;
            if (CopyBytes(&buffers[i].len, sizeof(chunk), reinterpret_cast<uint8_t*>(&chunk), sizeof(chunk)) ==
                    sizeof(chunk) &&
                chunk > 0) {
                total += chunk;
            }
        }
    }
    sockaddr_in discard{};
    const sockaddr* use_to = to;
    int use_tolen = tolen;
    const bool redirect_discard =
        g_local_ip_be != 0 && TransportActive() && !TransportSending() && game_port != 0;
    if (redirect_discard) {
        discard.sin_family = AF_INET;
        discard.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        discard.sin_port = htons(9);
        use_to = reinterpret_cast<const sockaddr*>(&discard);
        use_tolen = sizeof(discard);
    }
    const int result = g_WSASendTo(sock, buffers, count, sent, flags, use_to, use_tolen, overlapped, complete);
    PreserveError saved;
    if (redirect_discard) {
        BOOL off = FALSE;
        DWORD ignored = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    }
    if (gathered_n > 0 && gathered_n <= 8192) {
        TransportEnqueuePayload(game_port, gathered, gathered_n);
    }
    const char* note = DiscoveryNote(sock, to, tolen);
    const int cap = PreviewCap(note);
    const int shown = n > cap ? cap : n;
    LogCall("WSASendTo", sock, to, tolen, total, preview, shown, truncated || total > cap, unreadable, result, note);
    return result;
}

int WSAAPI Hook_WSARecvFrom(SOCKET sock, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from,
                            LPINT fromlen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE complete) {
    LPWSAOVERLAPPED_COMPLETION_ROUTINE routine = complete;
    if (kTransportCut != 1 && TransportActive() && !TransportSending()) {
        routine = TransportPrepareRecv(sock, from, fromlen, overlapped, complete);
    }
    const int result = g_WSARecvFrom(sock, buffers, count, received, flags, from, fromlen, overlapped, routine);
    PreserveError saved;
    const bool pending = result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING;
    if (kTransportCut != 1 && !pending && result == 0 && from != nullptr && fromlen != nullptr) {
        TransportNoteSocket(sock);
        TransportRewriteFrom(sock, from, fromlen);
    }
    if (kTransportCut != 1) {
        TransportRecvDone(sock, overlapped, result, pending);
    }
    uint8_t preview[kDiscoveryHex];
    bool truncated = false;
    bool unreadable = false;
    int n = 0;
    int got = 0;
    int peer_len = 0;
    if (fromlen && !pending) {
        CopyBytes(fromlen, sizeof(peer_len), reinterpret_cast<uint8_t*>(&peer_len), sizeof(peer_len));
    }
    const char* note = DiscoveryNote(sock, from, peer_len);
    const int cap = PreviewCap(note);
    if (!pending && result == 0 && received) {
        DWORD bytes = 0;
        if (CopyBytes(received, sizeof(bytes), reinterpret_cast<uint8_t*>(&bytes), sizeof(bytes)) > 0) {
            got = static_cast<int>(bytes);
        }
        n = PreviewBuffers(buffers, count, preview, cap, &truncated, &unreadable);
        if (got <= cap) {
            truncated = false;
        }
        if (n > got && got >= 0) {
            n = got;
        }
    }
    LogCall(pending ? "WSARecvFrom_attente" : "WSARecvFrom", sock, from, peer_len, got, preview, n, truncated,
            unreadable, result, note);
    return result;
}

BOOL WSAAPI Hook_WSAGetOverlappedResult(SOCKET sock, LPWSAOVERLAPPED overlapped, LPDWORD bytes, BOOL wait,
                                        LPDWORD flags) {
    const BOOL ok = g_WSAGetOverlappedResult(sock, overlapped, bytes, wait, flags);
    PreserveError saved;
    if (kTransportCut != 1 && ok && TransportActive() && !TransportSending()) {
        TransportCompleteOverlapped(sock, overlapped);
    }
    return ok;
}

bool FileMd5(const std::wstring& path, uint8_t out[16], unsigned long long* size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length)) {
        CloseHandle(file);
        return false;
    }
    *size = static_cast<unsigned long long>(length.QuadPart);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_len = 0;
    DWORD ignored = 0;
    bool ok = false;
    std::vector<UCHAR> object;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(file);
        return false;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len),
                          &ignored, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(file);
        return false;
    }
    object.resize(object_len);
    if (BCryptCreateHash(alg, &hash, object.data(), object_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(file);
        return false;
    }
    std::vector<UCHAR> buffer(1 << 16);
    ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (BCryptHashData(hash, buffer.data(), got, 0) != 0) {
            ok = false;
            break;
        }
    }
    if (ok && BCryptFinishHash(hash, out, 16, 0) != 0) {
        ok = false;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}

std::wstring FileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool EqualsIgnoreCase(const std::wstring& left, const wchar_t* right) {
    return _wcsicmp(left.c_str(), right) == 0;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// enabled and transport stay 0 unless the file says 1. relay stays empty if absent.
struct NetConfig {
    int enabled;
    int transport;
    char relay[256];
};

NetConfig ReadConfig(const std::wstring& path) {
    NetConfig cfg{};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return cfg;
    }
    char buffer[8192];
    DWORD got = 0;
    const BOOL read = ReadFile(file, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(file);
    if (!read) {
        return cfg;
    }
    buffer[got] = '\0';
    size_t i = 0;
    if (got >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF) {
        i = 3;
    }
    while (i < got) {
        size_t end = i;
        while (end < got && buffer[end] != '\n' && buffer[end] != '\r') {
            ++end;
        }
        std::string line(buffer + i, buffer + end);
        i = end + 1;
        const auto hash = line.find('#');
        const auto semi = line.find(';');
        size_t cut = line.size();
        if (hash != std::string::npos) {
            cut = hash;
        }
        if (semi != std::string::npos && semi < cut) {
            cut = semi;
        }
        line.resize(cut);
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto key_begin = line.find_first_not_of(" \t");
        if (key_begin == std::string::npos || key_begin > eq) {
            continue;
        }
        auto key_end = line.find_last_not_of(" \t", eq - 1);
        std::string key = line.substr(key_begin, key_end - key_begin + 1);
        auto val_begin = line.find_first_not_of(" \t", eq + 1);
        std::string value;
        if (val_begin != std::string::npos) {
            auto val_end = line.find_last_not_of(" \t");
            value = line.substr(val_begin, val_end - val_begin + 1);
        }
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (key == "enabled") {
            cfg.enabled = value == "1" ? 1 : 0;
        } else if (key == "transport") {
            cfg.transport = value == "1" ? 1 : 0;
        } else if (key == "relay" && value.size() < sizeof(cfg.relay)) {
            std::memcpy(cfg.relay, value.data(), value.size());
            cfg.relay[value.size()] = '\0';
        }
    }
    return cfg;
}

bool ParseRelay(const char* text, char* host, size_t host_cap, unsigned short* port) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    const char* colon = std::strrchr(text, ':');
    if (colon == nullptr || colon == text || colon[1] == '\0') {
        return false;
    }
    const size_t host_len = static_cast<size_t>(colon - text);
    if (host_len == 0 || host_len >= host_cap) {
        return false;
    }
    std::memcpy(host, text, host_len);
    host[host_len] = '\0';
    char* end = nullptr;
    const long value = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    *port = static_cast<unsigned short>(value);
    return true;
}

void TransportSink(const char* line) {
    WriteLine(line);
}

unsigned short GameLanPort(SOCKET sock, const sockaddr* peer, int peer_len) {
    const unsigned short local = LocalPort(sock);
    if (local == kDiscoveryPort || local == kSessionPort) {
        return local;
    }
    const unsigned short remote = InetPort(peer, peer_len);
    if (remote == kDiscoveryPort || remote == kSessionPort) {
        return remote;
    }
    return 0;
}

int GatherAll(LPWSABUF buffers, DWORD count, uint8_t* out, int cap) {
    if (buffers == nullptr || count == 0) {
        return 0;
    }
    int filled = 0;
    for (DWORD i = 0; i < count; ++i) {
        int chunk = 0;
        if (CopyBytes(&buffers[i].len, sizeof(chunk), reinterpret_cast<uint8_t*>(&chunk), sizeof(chunk)) < 0) {
            return -1;
        }
        if (chunk < 0) {
            return -1;
        }
        if (chunk > cap || filled > cap - chunk) {
            return -(cap + 1);
        }
        if (chunk == 0) {
            continue;
        }
        char* ptr = nullptr;
        if (CopyBytes(&buffers[i].buf, sizeof(ptr), reinterpret_cast<uint8_t*>(&ptr), sizeof(ptr)) < 0) {
            return -1;
        }
        if (CopyBytes(ptr, chunk, out + filled, chunk) != chunk) {
            return -1;
        }
        filled += chunk;
    }
    return filled;
}

void CopyGamePayload(SOCKET sock, unsigned short game_port, const void* data, int len) {
    if (!TransportActive() || TransportSending() || game_port == 0) {
        return;
    }
    TransportNoteSocket(sock);
    if (len > 8192) {
        TransportCopyPayload(game_port, data, len);
        return;
    }
    if (data == nullptr || len <= 0) {
        return;
    }
    TransportCopyPayload(game_port, data, len);
}

using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetForegroundWindowFn = BOOL(WINAPI*)(HWND);
using SetActiveWindowFn = HWND(WINAPI*)(HWND);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);

ShowWindowFn g_ShowWindow = nullptr;
SetForegroundWindowFn g_SetForegroundWindow = nullptr;
SetActiveWindowFn g_SetActiveWindow = nullptr;
SetWindowPosFn g_SetWindowPos = nullptr;
volatile LONG g_focus_counts[4] = {};

void LogFocus(int slot, const char* api, const char* detail) {
    const LONG count = InterlockedIncrement(&g_focus_counts[slot]);
    if (count > 5 && (count % 100) != 0) {
        return;
    }
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=focus local=- pair=- len=%ld hex= seq=0 tronque=0 note=%s %s bloque hors ecran",
         heure, GetCurrentProcessId(), count, api, detail);
}

bool WouldShowOnDesktop(HWND hwnd, int x, int y, UINT flags) {
    if (flags & SWP_NOMOVE) {
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) {
            return false;
        }
        x = rect.left;
        y = rect.top;
    }
    return x > -8000 || y > -8000;
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    PreserveError saved;
    const bool reveal = after == HWND_TOPMOST || (flags & SWP_SHOWWINDOW) != 0 || WouldShowOnDesktop(hwnd, x, y, flags);
    if (!reveal) {
        return g_SetWindowPos(hwnd, after, x, y, cx, cy, flags | SWP_NOACTIVATE);
    }
    char detail[80];
    std::snprintf(detail, sizeof(detail), "x=%d y=%d", x, y);
    LogFocus(0, "SetWindowPos", detail);
    flags = (flags | SWP_NOACTIVATE) & ~static_cast<UINT>(SWP_NOMOVE | SWP_SHOWWINDOW);
    return g_SetWindowPos(hwnd, HWND_NOTOPMOST, -32000, -32000, cx, cy, flags);
}

BOOL WINAPI Hook_ShowWindow(HWND hwnd, int cmd) {
    PreserveError saved;
    const bool reveal = cmd == SW_SHOWNORMAL || cmd == SW_SHOWMAXIMIZED || cmd == SW_SHOWNOACTIVATE || cmd == SW_SHOW ||
                        cmd == SW_SHOWNA || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT;
    if (!reveal) {
        return g_ShowWindow(hwnd, cmd);
    }
    char detail[40];
    std::snprintf(detail, sizeof(detail), "cmd=%d", cmd);
    LogFocus(1, "ShowWindow", detail);
    const BOOL shown = g_ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (g_SetWindowPos) {
        g_SetWindowPos(hwnd, HWND_NOTOPMOST, -32000, -32000, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    }
    return shown;
}

BOOL WINAPI Hook_SetForegroundWindow(HWND hwnd) {
    PreserveError saved;
    LogFocus(2, "SetForegroundWindow", "sans appel");
    if (g_SetWindowPos) {
        g_SetWindowPos(hwnd, HWND_NOTOPMOST, -32000, -32000, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    }
    return TRUE;
}

HWND WINAPI Hook_SetActiveWindow(HWND hwnd) {
    PreserveError saved;
    LogFocus(3, "SetActiveWindow", "sans appel");
    if (hwnd && g_SetWindowPos) {
        g_SetWindowPos(hwnd, HWND_NOTOPMOST, -32000, -32000, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    }
    return GetActiveWindow();
}

bool InstallFocusGuards() {
    struct Slot {
        const char* name;
        void* detour;
        void** original;
    };
    Slot slots[] = {
        {"ShowWindow", reinterpret_cast<void*>(&Hook_ShowWindow), reinterpret_cast<void**>(&g_ShowWindow)},
        {"SetForegroundWindow", reinterpret_cast<void*>(&Hook_SetForegroundWindow),
         reinterpret_cast<void**>(&g_SetForegroundWindow)},
        {"SetActiveWindow", reinterpret_cast<void*>(&Hook_SetActiveWindow), reinterpret_cast<void**>(&g_SetActiveWindow)},
        {"SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos), reinterpret_cast<void**>(&g_SetWindowPos)},
    };
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    bool ok = true;
    for (Slot& slot : slots) {
        const MH_STATUS status = MH_CreateHookApi(L"user32.dll", slot.name, slot.detour, slot.original);
        if (status != MH_OK) {
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=garde %s refusee (%s)",
                 GetCurrentProcessId(), slot.name, MH_StatusToString(status));
            ok = false;
            continue;
        }
        void* target = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, slot.name)) : nullptr;
        const MH_STATUS enabled = target ? MH_EnableHook(target) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (enabled != MH_OK) {
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=garde %s non active (%s)",
                 GetCurrentProcessId(), slot.name, MH_StatusToString(enabled));
            ok = false;
        }
    }
    Logf("heure=0 pid=%lu dir=etat local=- pair=- len=%ld hex= seq=0 tronque=0 note=gardes focus %s",
         GetCurrentProcessId(), 0L, ok ? "actives" : "partielles");
    return ok;
}

// ---------------------------------------------------------------------------
// Plutonium-style gameplay features:
// 1. Custom Username replacing DEFAULT (0x1095D8D0, 0x10AD6950)
// 2. Friendly Fire Toggle in LAN Create/Modify (0x10B51290, 0x10B51350)
// 3. Kick Second Player in LAN Lobby (0x10B4D1C0, 0x10B4CA10)
// 4. Voice Chat affecting Stealth Sound Meter (0x10A3C7F0, 0x10AE73A0)
// ---------------------------------------------------------------------------

struct GMallocVtbl {
    void* (__thiscall* Malloc)(void* self, int size, const void* tag);
    void* (__thiscall* Realloc)(void* self, void* ptr, int size, const void* tag);
    void(__thiscall* Free)(void* self, void* ptr);
};
struct GMallocObj {
    GMallocVtbl* vtbl;
};

struct UeFString {
    wchar_t* data;
    int32_t count;
    int32_t max;
};

struct SessionPlayerArray {
    uint8_t* data;
    int32_t count;
    int32_t max;
};

using SetLocalSessionPlayerNameFn = int(__cdecl*)(const wchar_t*, int);
using GetCurrentProfileFn = void*(__thiscall*)(void*, void*);
using SessionOptionsResetFn = void(__thiscall*)(uint8_t*);
using SessionOptionsInitFn = char(__thiscall*)(uint8_t*, int, int, int);
using LobbyLanTickFn = void(__thiscall*)(uint8_t*, float);
using LobbyLanOnCommandFn = char(__thiscall*)(uint8_t*, uint8_t*);
using EngineInitVoiceChatFn = int(__thiscall*)(uint8_t*);
using VoiceTickFn = void(__thiscall*)(uint8_t*);
using OnLocalVoiceNoiseFn = void(__thiscall*)(void*, uint32_t, uint32_t);
using FStringFromWStrFn = void*(__thiscall*)(void*, const wchar_t*);
using FStringDtorFn = void(__thiscall*)(void*);
using AddOptionFn = void(__thiscall*)(void*, void*);
using FindFNameAFn = uint32_t*(__cdecl*)(uint32_t*, const char*);
using FindChildWidgetFn = void*(__thiscall*)(void*, uint32_t);
using SetWidgetVisibleFn = void(__thiscall*)(void*, int);
using IsInLanSessionFn = int(__cdecl*)();
using IsLanHostFn = int(__cdecl*)();
using GetSessionPlayersFn = void(__cdecl*)(SessionPlayerArray*);
using EmptyPlayerArrayFn = void(__thiscall*)(SessionPlayerArray*, int, int, int);
using LanKickByIdFn = int(__cdecl*)(uint8_t);
using GetLocalControllerFn = uint8_t*(__thiscall*)(void*);
using MakeNoiseFn = void(__thiscall*)(void*, void*, float, int, float*, void*, float*, int);
using MatchNameInitDefaultFn = void(__thiscall*)(uint8_t*, void*);
using OnKickReceivedFn = void(__cdecl*)(void*);
using QuazalLeaveSessionFn = int(__cdecl*)();
using LobbyLanLeaveConfirmedFn = void(__thiscall*)(uint8_t*, int);

// Online menu / GameService / Quazal Online hooks
using QuazalSessionDescCtorFn = void*(__thiscall*)(void*);
using QuazalCreateSessionFn = int(__cdecl*)(void*);
using QuazalJoinSessionFn = int(__cdecl*)(void*);
using QuazalStartBrowseFn = int(__cdecl*)();
using QuazalRegisterListenerFn = void(__cdecl*)(void*);
using QuazalUnregisterListenerFn = void(__cdecl*)(void*);
using QuazalGetBrowseResultsFn = int(__cdecl*)(SessionPlayerArray*);
using QuazalGetSessionDescFn = int(__cdecl*)(void*);
using QuazalInitFn = int(__cdecl*)(void*, int);
using IsOnlineSessionFn = int(__cdecl*)();
using QuazalPollFn = int(__cdecl*)();
using LoginOnShowFn = char(__thiscall*)(uint8_t*, void*);
using LobbyOnlineTickFn = char(__thiscall*)(uint8_t*, void*, float);
using RoomOnlineTickFn = char(__thiscall*)(uint8_t*, void*, float);
using RoomOnlineOnMsgBoxFn = void(__thiscall*)(uint8_t*, int);
using RoomOnlineLaunchGameFn = char(__thiscall*)(uint8_t*);
using OnlineLobbyCbFn = void(__cdecl*)(void*, int, const char*, int);
using OnlineMotdCbFn = void(__cdecl*)(void*, const char*, const char*);
using OnlineMemberJoinCbFn = void(__cdecl*)(void*, const char*, int);
using OnlineRoomFoundCbFn = void(__cdecl*)(void*, int, int, const char*, int, const void*, const char*, const char*);

SetLocalSessionPlayerNameFn g_orig_SetLocalSessionPlayerName = nullptr;
GetCurrentProfileFn g_orig_GetCurrentProfile = nullptr;
SessionOptionsResetFn g_orig_SessionOptionsReset = nullptr;
SessionOptionsInitFn g_orig_SessionOptionsInit = nullptr;
LobbyLanTickFn g_orig_LobbyLanTick = nullptr;
LobbyLanOnCommandFn g_orig_LobbyLanOnCommand = nullptr;
EngineInitVoiceChatFn g_orig_EngineInitVoiceChat = nullptr;
VoiceTickFn g_orig_VoiceTick = nullptr;
MatchNameInitDefaultFn g_orig_MatchNameInitDefault = nullptr;
OnKickReceivedFn g_orig_OnKickReceived = nullptr;

QuazalSessionDescCtorFn g_orig_QuazalSessionDescCtor = nullptr;
QuazalCreateSessionFn g_orig_QuazalCreateSession = nullptr;
QuazalJoinSessionFn g_orig_QuazalJoinSession = nullptr;
QuazalStartBrowseFn g_orig_QuazalStartBrowse = nullptr;
QuazalInitFn g_orig_QuazalInit = nullptr;
IsOnlineSessionFn g_orig_IsOnlineSession = nullptr;
LoginOnShowFn g_orig_LoginOnShow = nullptr;
LobbyOnlineTickFn g_orig_LobbyOnlineTick = nullptr;
RoomOnlineTickFn g_orig_RoomOnlineTick = nullptr;
RoomOnlineLaunchGameFn g_orig_RoomOnlineLaunchGame = nullptr;

struct UeStdWString {
    uint32_t pad0;
    union {
        wchar_t buf[8];
        wchar_t* ptr;
    } u;
    uint32_t len;
    uint32_t cap;
};

alignas(16) uint8_t g_dummy_gameservice[0x4000]{};
alignas(16) uint8_t g_active_online_desc[0x8CC]{};
bool g_has_active_online_desc = false;
bool g_online_logged_in = false;
bool g_online_in_lobby_group = false;
bool g_online_room_created = false;
bool g_online_join_requested = false;
int g_online_active_room_id = 0;
uint8_t* g_lobby_online_ctx = nullptr;
OnlineRoomFoundCbFn g_online_room_found_cb = nullptr;
uint64_t g_last_online_browse_ms = 0;
volatile LONG g_online_browse_registered = 0;
volatile LONG g_kicked_from_online_session = 0;
wchar_t g_online_username_w[64] = L"Agent";
char g_online_username_a[64] = "Agent";
const wchar_t* g_online_password_w = L"password";
const wchar_t* g_online_key_w = L"SCCT-COOP-ONLINE";

uint32_t g_fname_B_Kick = 0;
volatile LONG g_kicked_from_lan_session = 0;
LONG g_logged_name = 0;
LONG g_logged_ffire = 0;
LONG g_logged_voice_init = 0;
uint64_t g_voice_hold_until = 0;
uint32_t g_voice_hold_level = 0;
uint64_t g_last_voice_emit_ms = 0;
uint64_t g_last_voice_log_ms = 0;

// CSessionOptionFriendlyFire vtable built from native FriendlyFire + PrivateSlot (YES/NO toggle) methods.
void* g_ffire_vtable[14] = {
    reinterpret_cast<void*>(0x10B72650),
    reinterpret_cast<void*>(0x10B72290),
    reinterpret_cast<void*>(0x10B72220),
    reinterpret_cast<void*>(0x10B73150),
    reinterpret_cast<void*>(0x10B725C0),
    reinterpret_cast<void*>(0x10B725D0),
    reinterpret_cast<void*>(0x10B738E0),
    reinterpret_cast<void*>(0x10B72800),
    reinterpret_cast<void*>(0x10B72810),
    reinterpret_cast<void*>(0x10B727E0),
    reinterpret_cast<void*>(0x10B727F0),
    reinterpret_cast<void*>(0x10B72830),
    reinterpret_cast<void*>(0x10B73A60),
    reinterpret_cast<void*>(0x10B74100),
};

// Modernized 48 kHz mono miniaudio + RNNoise + Opus (24 kbps, 20 ms, DTX) mic capture & peak monitor.
volatile LONG g_mic_peak = 0;
LONG g_mic_started = 0;
volatile LONG g_mic_device_id = -1;

void StartMicCapture() {
    if (InterlockedCompareExchange(&g_mic_started, 1, 0) != 0) {
        return;
    }
    const int dev_id = static_cast<int>(InterlockedCompareExchange(&g_mic_device_id, 0, 0));
    if (!VoiceAudioStartCapture(dev_id, &g_mic_peak)) {
        InterlockedExchange(&g_mic_started, 0);
    }
}

void StopMicCapture() {
    VoiceAudioStopCapture();
    InterlockedExchange(&g_mic_started, 0);
    InterlockedExchange(&g_mic_peak, 0);
}

bool RoomPtrOk(const void* p) {
    return reinterpret_cast<uintptr_t>(p) >= 0x10000u && reinterpret_cast<uintptr_t>(p) < 0x7FFF0000u;
}

uint32_t EnsureBKickFName() {
    if (g_fname_B_Kick == 0) {
        uint32_t tmp = 0;
        reinterpret_cast<FindFNameAFn>(0x10D233A0)(&tmp, reinterpret_cast<const char*>(0x110E1C3C));
        g_fname_B_Kick = tmp;
    }
    return g_fname_B_Kick;
}

int __cdecl Hook_SetLocalSessionPlayerName(const wchar_t* name, int broadcast) {
    const wchar_t* use_name = (g_custom_name[0] != L'\0') ? g_custom_name : name;
    if (g_custom_name[0] != L'\0' && InterlockedCompareExchange(&g_logged_name, 1, 0) == 0) {
        char aname[32]{};
        WideCharToMultiByte(CP_UTF8, 0, g_custom_name, -1, aname, sizeof(aname), nullptr, nullptr);
        Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=custom_name_applied name=%s",
             GetCurrentProcessId(), aname);
    }
    return g_orig_SetLocalSessionPlayerName(use_name, broadcast);
}

void* __fastcall Hook_GetCurrentProfile(void* this_fstr, void*, void* out_fstr) {
    void* ret = g_orig_GetCurrentProfile(this_fstr, out_fstr);
    if (g_custom_name[0] != L'\0' && out_fstr != nullptr) {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (caller == 0x10B4BDC2u) {
            reinterpret_cast<FStringDtorFn>(0x1090BA70)(out_fstr);
            std::memset(out_fstr, 0, sizeof(UeFString));
            reinterpret_cast<FStringFromWStrFn>(0x1090BA00)(out_fstr, g_custom_name);
        }
    }
    return ret;
}

void __fastcall Hook_SessionOptionsReset(uint8_t* data, void*) {
    g_orig_SessionOptionsReset(data);
    if (RoomPtrOk(data)) {
        *reinterpret_cast<int32_t*>(data + 0x64) = (g_default_ffire != 0) ? 1 : 0;
        if (g_custom_name[0] != L'\0') {
            wcsncpy_s(reinterpret_cast<wchar_t*>(data + 4), 20, g_custom_name, 19);
        }
    }
}

void __fastcall Hook_MatchNameInitDefault(uint8_t* opt, void*, void* arg) {
    g_orig_MatchNameInitDefault(opt, arg);
    if (g_custom_name[0] != L'\0' && RoomPtrOk(opt)) {
        wcsncpy_s(reinterpret_cast<wchar_t*>(opt + 0xC), 20, g_custom_name, 19);
    }
}

void __cdecl Hook_OnKickReceived(void* kicked_do) {
    g_orig_OnKickReceived(kicked_do);
    void* local_do = *reinterpret_cast<void**>(0x111F55A8);
    if (kicked_do != nullptr && local_do != nullptr && kicked_do == local_do) {
        InterlockedExchange(&g_kicked_from_lan_session, 1);
        InterlockedExchange(&g_kicked_from_online_session, 1);
        char heure[40];
        NowText(heure, sizeof(heure));
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=session_kick_received",
             heure, GetCurrentProcessId());
    }
}

void UpdateOnlineIdentityBuffers() {
    const wchar_t* src_w = (g_custom_name[0] != L'\0') ? g_custom_name : L"Agent";
    wcsncpy_s(g_online_username_w, 64, src_w, _TRUNCATE);
    WideCharToMultiByte(CP_UTF8, 0, g_online_username_w, -1, g_online_username_a, sizeof(g_online_username_a), nullptr,
                        nullptr);
    std::snprintf(reinterpret_cast<char*>(g_dummy_gameservice + 0x2406), 16, "%.15s", g_online_username_a);
    std::snprintf(reinterpret_cast<char*>(g_dummy_gameservice + 0x2416), 16, "%s", "password");
    std::snprintf(reinterpret_cast<char*>(g_dummy_gameservice + 0x2427), 32, "%s", "SCCT-COOP-ONLINE");
    std::snprintf(reinterpret_cast<char*>(g_dummy_gameservice + 0x1FDE), 64, "%s", g_online_username_a);
    std::snprintf(reinterpret_cast<char*>(g_dummy_gameservice + 0x21DF), 64, "%s", "COOP ONLINE");
    *reinterpret_cast<void**>(0x111FA6DC) = g_dummy_gameservice;
    *reinterpret_cast<uint8_t*>(0x111FA6E0) = 1;
}

void SetUeEditBoxText(void* edit_box, const wchar_t* text) {
    if (!RoomPtrOk(edit_box) || !text) {
        return;
    }
    UeStdWString wstr{};
    wstr.cap = 7;
    wstr.len = 0;
    wstr.u.buf[0] = L'\0';
    const uint32_t n = static_cast<uint32_t>(std::wcslen(text));
    using WStrAssignFn = void(__thiscall*)(void*, const wchar_t*, uint32_t);
    using EditSetTextFn = void(__thiscall*)(void*, const void*);
    using WStrDtorFn = void(__thiscall*)(void*);
    reinterpret_cast<WStrAssignFn>(0x10AD8220)(&wstr, text, n);
    reinterpret_cast<EditSetTextFn>(0x10D34E10)(edit_box, &wstr);
    reinterpret_cast<WStrDtorFn>(0x10AD6460)(&wstr);
}

void KickSecondPlayerFromSession(const char* source_tag) {
    if (!reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)() || !reinterpret_cast<IsLanHostFn>(0x1095B880)()) {
        return;
    }
    SessionPlayerArray arr{};
    reinterpret_cast<GetSessionPlayersFn>(0x10963370)(&arr);
    if (arr.data != nullptr && arr.count > 0) {
        for (int i = 0; i < arr.count; ++i) {
            uint8_t* entry = arr.data + i * 0xB0;
            const uint8_t is_host = entry[0x2A];
            if (is_host == 0) {
                const uint8_t participant_id = entry[0x30];
                const wchar_t* wname = reinterpret_cast<const wchar_t*>(entry);
                char aname[32]{};
                WideCharToMultiByte(CP_UTF8, 0, wname, -1, aname, sizeof(aname), nullptr, nullptr);
                const int ok = reinterpret_cast<LanKickByIdFn>(0x10962CC0)(participant_id);
                char heure[40];
                NowText(heure, sizeof(heure));
                Logf("heure=%s pid=%lu dir=etat local=- pair=- len=%d hex= seq=0 tronque=0 "
                     "note=%s_kick_sent player=%s id=%u ok=%d",
                     heure, GetCurrentProcessId(), arr.count, source_tag, aname, static_cast<unsigned>(participant_id),
                     ok);
                break;
            }
        }
        reinterpret_cast<EmptyPlayerArrayFn>(0x10AA8E50)(&arr, 0, arr.count, 0xB0);
        if (arr.data != nullptr) {
            GMallocObj* gmalloc = *reinterpret_cast<GMallocObj**>(0x111C88A0);
            if (gmalloc && gmalloc->vtbl && gmalloc->vtbl->Free) {
                gmalloc->vtbl->Free(gmalloc, arr.data);
            }
        }
    }
}

void KickSecondPlayerInLanLobby() {
    KickSecondPlayerFromSession("lan");
}

void __fastcall OnCustomQuazalBrowseEvent(void*, void*, int event_id) {
    if (event_id != 2) {
        return;
    }
    if (!RoomPtrOk(g_lobby_online_ctx) || g_online_room_found_cb == nullptr) {
        return;
    }
    if (g_online_join_requested || reinterpret_cast<QuazalPollFn>(0x1095B8A0)()) {
        return;
    }
    SessionPlayerArray arr{};
    reinterpret_cast<QuazalGetBrowseResultsFn>(0x109621C0)(&arr);
    if (arr.data != nullptr && arr.count > 0) {
        void** ptrs = reinterpret_cast<void**>(arr.data);
        for (int i = 0; i < arr.count; ++i) {
            const uint8_t* desc = reinterpret_cast<const uint8_t*>(ptrs[i]);
            if (!RoomPtrOk(desc)) {
                continue;
            }
            std::memcpy(g_active_online_desc, desc, sizeof(g_active_online_desc));
            *reinterpret_cast<uint32_t*>(g_active_online_desc + 0x8BC) = 0;
            g_has_active_online_desc = true;

            char room_name[128]{};
            const wchar_t* wname = reinterpret_cast<const wchar_t*>(desc + 0x400);
            if (wname[0] != L'\0') {
                WideCharToMultiByte(CP_UTF8, 0, wname, -1, room_name, sizeof(room_name), nullptr, nullptr);
            }
            if (room_name[0] == '\0') {
                std::snprintf(room_name, sizeof(room_name), "COOP MATCH");
            }
            *reinterpret_cast<int32_t*>(g_lobby_online_ctx + 0x60) = 1;
            uint8_t* filter_page = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + 0x54);
            if (RoomPtrOk(filter_page)) {
                filter_page[0x71] = 0;
            }
            char heure[40];
            NowText(heure, sizeof(heure));
            Logf("heure=%s pid=%lu dir=etat local=- pair=- len=%d hex= seq=0 tronque=0 "
                 "note=online_browse_found room=%s ffire=%d",
                 heure, GetCurrentProcessId(), arr.count, room_name,
                 *reinterpret_cast<const int32_t*>(desc + 0x474));
#pragma pack(push, 1)
            struct OnlineRoomFoundData {
                uint32_t has_password;
                uint8_t desc[0x8CC];
                uint32_t ping;
            };
#pragma pack(pop)
            OnlineRoomFoundData room_data{};
            room_data.has_password = 0;
            const size_t copy_sz = sizeof(room_data.desc) < sizeof(g_active_online_desc)
                                       ? sizeof(room_data.desc)
                                       : sizeof(g_active_online_desc);
            std::memcpy(room_data.desc, g_active_online_desc, copy_sz);
            room_data.ping = 10;
            g_online_room_found_cb(g_lobby_online_ctx, 1, 1, room_name, 1, &room_data, "0", "");
            uint8_t* lst_server = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + 0x0C);
            if (RoomPtrOk(lst_server) && *reinterpret_cast<int16_t*>(lst_server + 0xA8) < 0) {
                using ListSetSelFn = void(__thiscall*)(void*, int, int, int);
                reinterpret_cast<ListSetSelFn>(0x10D2A830)(lst_server, 0, 1, 0);
            }
            // Offset +0x2C is B_CancelSearch (_CANCEL_) and +0x30 is A_Searching; keep both hidden after browse results arrive.
            uint8_t* btn_cancel_search = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + 0x2C);
            if (RoomPtrOk(btn_cancel_search)) {
                btn_cancel_search[0x18] &= static_cast<uint8_t>(~1);
            }
            uint8_t* lbl_searching = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + 0x30);
            if (RoomPtrOk(lbl_searching)) {
                lbl_searching[0x18] &= static_cast<uint8_t>(~1);
            }
        }
    }
    if (arr.data != nullptr) {
        GMallocObj* gmalloc = *reinterpret_cast<GMallocObj**>(0x111C88A0);
        if (gmalloc && gmalloc->vtbl && gmalloc->vtbl->Free) {
            gmalloc->vtbl->Free(gmalloc, arr.data);
        }
    }
}

void* g_quazal_browse_vtbl[1] = {reinterpret_cast<void*>(&OnCustomQuazalBrowseEvent)};
struct CustomQuazalListener {
    void** vtbl;
} g_quazal_browse_listener = {g_quazal_browse_vtbl};

// --- Quazal Direct UDP Mode Hooks (keep Net-Z in LAN/UDP transport while navigating Online UI) ---
void* __fastcall Hook_QuazalSessionDescCtor(void* self, void*) {
    void* ret = g_orig_QuazalSessionDescCtor(self);
    if (RoomPtrOk(self)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(self) + 0x8BC) = 0;
    }
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    return ret;
}

int __cdecl Hook_QuazalInit(void* engine, int is_online) {
    (void)is_online;
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    const int ret = g_orig_QuazalInit(engine, 0);
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    return ret;
}

int __cdecl Hook_IsOnlineSession() {
    if (g_online_logged_in) {
        return 1;
    }
    return g_orig_IsOnlineSession ? g_orig_IsOnlineSession() : 0;
}

int __cdecl Hook_QuazalCreateSession(void* desc) {
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    if (RoomPtrOk(desc)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(desc) + 0x8BC) = 0;
        std::memcpy(g_active_online_desc, desc, sizeof(g_active_online_desc));
        g_has_active_online_desc = true;
    }
    const int ret = g_orig_QuazalCreateSession(desc);
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
         "note=online_quazal_create ret=%d ffire=%d",
         heure, GetCurrentProcessId(), ret, RoomPtrOk(desc) ? *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(desc) + 0x474) : -1);
    return ret;
}

int __cdecl Hook_QuazalJoinSession(void* desc) {
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    if (RoomPtrOk(desc)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(desc) + 0x8BC) = 0;
    }
    if (InterlockedExchange(&g_online_browse_registered, 0) != 0) {
        reinterpret_cast<QuazalUnregisterListenerFn>(0x10961F90)(&g_quazal_browse_listener);
    }
    const uint32_t pre_state = *reinterpret_cast<uint32_t*>(0x111F55B8);
    if (pre_state == 1 || pre_state == 3 || pre_state == 5) {
        // Stale session state after a kick/leave: fully leave before joining again.
        using QuazalLeaveFn = void(__cdecl*)(int);
        reinterpret_cast<QuazalLeaveFn>(0x10964310)(1);
        char hs[40];
        NowText(hs, sizeof(hs));
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_join_stale_state state=%u after=%u",
             hs, GetCurrentProcessId(), pre_state, *reinterpret_cast<uint32_t*>(0x111F55B8));
    }
    if (g_custom_name[0] != L'\0') {
        Hook_SetLocalSessionPlayerName(g_custom_name, 0);
    }
    const int ret = g_orig_QuazalJoinSession(desc);
    if (ret != 0 && g_custom_name[0] != L'\0') {
        Hook_SetLocalSessionPlayerName(g_custom_name, 1);
    }
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_quazal_join ret=%d pre_state=%u",
         heure, GetCurrentProcessId(), ret, pre_state);
    return ret;
}

int __cdecl Hook_QuazalStartBrowse() {
    *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
    return g_orig_QuazalStartBrowse();
}

// --- GameService (0x10AAxxxx) Online Service Detours ---
bool __cdecl Hook_GS_Init() {
    UpdateOnlineIdentityBuffers();
    return true;
}
bool __cdecl Hook_GS_Disconnect() {
    g_online_logged_in = false;
    g_online_in_lobby_group = false;
    g_online_room_created = false;
    g_online_join_requested = false;
    g_online_active_room_id = 0;
    return true;
}
void __cdecl Hook_GS_NoOp0() {}
void __cdecl Hook_GS_NoOpPtr(const void*) {}
void __cdecl Hook_GS_SetUsername(const wchar_t* name) {
    if (name && name[0] != L'\0' && g_custom_name[0] == L'\0') {
        wcsncpy_s(g_online_username_w, 64, name, _TRUNCATE);
    }
    UpdateOnlineIdentityBuffers();
}
bool __cdecl Hook_GS_ValidateName(const wchar_t*, int) {
    return true;
}
bool __cdecl Hook_GS_StartLogin() {
    UpdateOnlineIdentityBuffers();
    g_online_logged_in = true;
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_login_success user=%s",
         heure, GetCurrentProcessId(), g_online_username_a);
    return true;
}
bool __cdecl Hook_GS_IsLoginStep1Done() {
    return true;
}
bool __cdecl Hook_GS_IsLoggedIn() {
    return g_online_logged_in;
}
bool __cdecl Hook_GS_PollError(void*) {
    return false;
}
const wchar_t* __cdecl Hook_GS_GetUsername() {
    UpdateOnlineIdentityBuffers();
    return g_online_username_w;
}
const wchar_t* __cdecl Hook_GS_GetPassword() {
    return g_online_password_w;
}
const wchar_t* __cdecl Hook_GS_GetKey() {
    return g_online_key_w;
}
bool __cdecl Hook_GS_IsAutoJoin() {
    return false;
}
bool __cdecl Hook_GS_GetStatusText1(char* out) {
    if (!out) {
        return false;
    }
    UpdateOnlineIdentityBuffers();
    std::snprintf(out, 0x200, "%s", g_online_username_a);
    return true;
}
bool __cdecl Hook_GS_GetStatusText2(char* out) {
    if (!out) {
        return false;
    }
    std::snprintf(out, 0x200, "%s", "COOP ONLINE");
    return true;
}
void __cdecl Hook_GS_RegisterLobbyCb(void* ctx, OnlineLobbyCbFn cb) {
    if (ctx && cb) {
        cb(ctx, 1, "COOP ONLINE", 15);
    }
}
void __cdecl Hook_GS_RegisterMotdCb(void* ctx, OnlineMotdCbFn cb) {
    if (ctx && cb) {
        cb(ctx, "SPLINTER CELL CHAOS THEORY", "CO-OP ONLINE SERVER");
    }
}
bool __cdecl Hook_GS_JoinLobbyGroup(int) {
    g_online_in_lobby_group = true;
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_lobby_group_joined",
         heure, GetCurrentProcessId());
    return true;
}
bool __cdecl Hook_GS_IsInLobbyGroup() {
    return g_online_in_lobby_group;
}
bool __cdecl Hook_GS_True0() {
    return true;
}
bool __cdecl Hook_GS_False0() {
    return false;
}
int __cdecl Hook_GS_Zero0() {
    return 0;
}
bool __cdecl Hook_GS_True1(const void*) {
    return true;
}
bool __cdecl Hook_GS_True2(int, int) {
    return true;
}
bool __cdecl Hook_GS_GetCurrentLobbyIds(int* out_chat_id, int* out_room_group_id, int* out_c, char* out_name) {
    if (out_chat_id) {
        *out_chat_id = 1;
    }
    if (out_room_group_id) {
        *out_room_group_id = 1;
    }
    if (out_c) {
        *out_c = 1;
    }
    if (out_name) {
        std::snprintf(out_name, 64, "COOP ONLINE");
    }
    return true;
}
void __cdecl Hook_GS_RegisterMemberJoinCb(void* ctx, OnlineMemberJoinCbFn cb) {
    if (ctx && cb == reinterpret_cast<OnlineMemberJoinCbFn>(0x10B47C60)) {
        UpdateOnlineIdentityBuffers();
        cb(ctx, g_online_username_a, 1);
    }
}
void __cdecl Hook_GS_NoOp2(void*, void*) {}

// --- Two-Way In-Game <-> Launcher Friends Sync ---
using FriendMsgCbFn = void(__cdecl*)(void* ctx, const char* name, const char* msg);
using FriendStatusCbFn = void(__cdecl*)(void* ctx, const char* name, int status, const char* info);
using FriendListCbFn = void(__cdecl*)(void* ctx, const char* name, int status, const char* info);
using FriendActionCbFn = void(__cdecl*)(void* ctx, const char* name);

struct IngameFriendEntry {
    int id = 0;
    int kind = 0;    // 0 = friend, 1 = incoming, 2 = outgoing
    int status = 0;  // 0 = offline, 1 = online, 2 = hosting
    std::string username;
    std::string info;
};

CRITICAL_SECTION g_friends_lock{};
bool g_friends_lock_init = false;
wchar_t g_launcher_api_url[256]{};
wchar_t g_ingame_api_token[128]{};
std::vector<IngameFriendEntry> g_cached_friends;
volatile LONG g_friends_dirty = 0;
HANDLE g_friends_sync_event = nullptr;
HANDLE g_friends_sync_thread = nullptr;

void* g_friend_msg_ctx = nullptr;
FriendMsgCbFn g_friend_msg_cb = nullptr;
void* g_friend_status_ctx = nullptr;
FriendStatusCbFn g_friend_status_cb = nullptr;
void* g_friend_list_ctx = nullptr;
FriendListCbFn g_friend_list_cb = nullptr;
void* g_friend_added_ctx = nullptr;
FriendActionCbFn g_friend_added_cb = nullptr;
void* g_friend_removed_ctx = nullptr;
FriendActionCbFn g_friend_removed_cb = nullptr;

void EnsureFriendsLock() {
    if (!g_friends_lock_init) {
        InitializeCriticalSection(&g_friends_lock);
        g_friends_lock_init = true;
    }
}

bool FriendsHttpRequest(const wchar_t* method, const wchar_t* path, const std::string& body, std::string* out_resp) {
    if (g_launcher_api_url[0] == L'\0' || g_ingame_api_token[0] == L'\0') {
        return false;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[128]{};
    wchar_t url_path[256]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = url_path;
    uc.dwUrlPathLength = _countof(url_path);
    if (!WinHttpCrackUrl(g_launcher_api_url, 0, 0, &uc)) {
        return false;
    }
    HINTERNET session = WinHttpOpen(L"SCCT-CoopNet/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 2000, 2000, 3000, 3000);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return false;
    }
    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    wchar_t headers[320]{};
    _snwprintf_s(headers, _countof(headers), _TRUNCATE, L"X-Ingame-Token: %ls\r\nContent-Type: application/json\r\n",
                 g_ingame_api_token);
    const LPVOID body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD body_len = static_cast<DWORD>(body.size());
    bool ok = WinHttpSendRequest(req, headers, static_cast<DWORD>(std::wcslen(headers)), body_ptr, body_len, body_len, 0) != FALSE;
    if (ok) {
        ok = WinHttpReceiveResponse(req, nullptr) != FALSE;
    }
    DWORD status_code = 0;
    DWORD sc_sz = sizeof(status_code);
    if (ok) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status_code, &sc_sz, WINHTTP_NO_HEADER_INDEX);
        ok = (status_code >= 200 && status_code < 300);
    }
    if (ok && out_resp) {
        out_resp->clear();
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(req, chunk.data(), avail, &read) && read > 0) {
                out_resp->append(chunk.data(), read);
            } else {
                break;
            }
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

void ParseFriendsTsv(const std::string& raw, std::vector<IngameFriendEntry>* out) {
    out->clear();
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) {
            nl = raw.size();
        }
        std::string line = raw.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> cols;
        size_t cpos = 0;
        while (cpos <= line.size()) {
            size_t tab = line.find('\t', cpos);
            if (tab == std::string::npos) {
                cols.push_back(line.substr(cpos));
                break;
            }
            cols.push_back(line.substr(cpos, tab - cpos));
            cpos = tab + 1;
        }
        if (cols.size() >= 2 && cols[0] == "MIC") {
            const int mic_id = std::atoi(cols[1].c_str());
            if (mic_id != InterlockedCompareExchange(&g_mic_device_id, 0, 0)) {
                InterlockedExchange(&g_mic_device_id, mic_id);
                if (InterlockedCompareExchange(&g_mic_started, 0, 0) != 0) {
                    StopMicCapture();
                    StartMicCapture();
                }
            }
            continue;
        }
        if (cols.size() < 3) {
            continue;
        }
        IngameFriendEntry e{};
        e.id = std::atoi(cols[1].c_str());
        e.username = cols[2];
        if (cols[0] == "FRIEND") {
            e.kind = 0;
            const std::string st = cols.size() > 3 ? cols[3] : "offline";
            const std::string rm = cols.size() > 4 ? cols[4] : "";
            if (st == "hosting" && !rm.empty()) {
                e.status = 2;
                e.info = "Hosting " + rm;
            } else if (st == "online" || st == "hosting") {
                e.status = 1;
                e.info = "Online";
            } else {
                e.status = 0;
                e.info = "Offline";
            }
        } else if (cols[0] == "INCOMING") {
            e.kind = 1;
            e.status = 1;
            e.info = "Pending Request";
        } else if (cols[0] == "OUTGOING") {
            e.kind = 2;
            e.status = 0;
            e.info = "Request Sent";
        } else {
            continue;
        }
        out->push_back(std::move(e));
    }
}

DWORD WINAPI FriendsSyncThreadProc(LPVOID) {
    for (;;) {
        std::string body;
        if (FriendsHttpRequest(L"GET", L"/api/ingame/friends", "", &body)) {
            std::vector<IngameFriendEntry> parsed;
            ParseFriendsTsv(body, &parsed);
            EnsureFriendsLock();
            EnterCriticalSection(&g_friends_lock);
            g_cached_friends.swap(parsed);
            LeaveCriticalSection(&g_friends_lock);
            InterlockedExchange(&g_friends_dirty, 1);
        }
        if (g_friends_sync_event) {
            WaitForSingleObject(g_friends_sync_event, 5000);
        } else {
            Sleep(5000);
        }
    }
    return 0;
}

void StartFriendsSyncThread() {
    if (g_launcher_api_url[0] == L'\0' || g_ingame_api_token[0] == L'\0') {
        return;
    }
    EnsureFriendsLock();
    if (!g_friends_sync_event) {
        g_friends_sync_event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    }
    if (!g_friends_sync_thread) {
        g_friends_sync_thread = CreateThread(nullptr, 0, &FriendsSyncThreadProc, nullptr, 0, nullptr);
    }
}

struct AsyncFriendOp {
    std::wstring path;
    std::string body;
};

DWORD WINAPI FriendOpThreadProc(LPVOID param) {
    auto* op = reinterpret_cast<AsyncFriendOp*>(param);
    if (op) {
        FriendsHttpRequest(L"POST", op->path.c_str(), op->body, nullptr);
        delete op;
        if (g_friends_sync_event) {
            SetEvent(g_friends_sync_event);
        }
    }
    return 0;
}

void DispatchFriendOpAsync(const wchar_t* path, const std::string& json_body) {
    auto* op = new AsyncFriendOp{path, json_body};
    HANDLE h = CreateThread(nullptr, 0, &FriendOpThreadProc, op, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        delete op;
    }
}

void SelectMicDevice(int dev_id, bool notify_launcher) {
    const int num_devs = VoiceAudioGetCaptureDeviceCount();
    if (dev_id >= num_devs) {
        dev_id = -1;
    }
    InterlockedExchange(&g_mic_device_id, dev_id);
    if (InterlockedCompareExchange(&g_mic_started, 0, 0) != 0) {
        StopMicCapture();
        StartMicCapture();
    }
    char dev_name[96] = "System Default";
    if (dev_id >= 0) {
        wchar_t wname[96]{};
        if (VoiceAudioGetCaptureDeviceName(dev_id, wname, 96)) {
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, dev_name, sizeof(dev_name), nullptr, nullptr);
        }
    }
    Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=mic_device_selected id=%d name=%s",
         GetCurrentProcessId(), dev_id, dev_name);
    if (notify_launcher) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "{\"mic_device\":%d}", dev_id);
        DispatchFriendOpAsync(L"/api/ingame/mic", buf);
    }
}

void PollMicHotkey() {
    static bool s_f8_down = false;
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !s_f8_down) {
        const int num_devs = VoiceAudioGetCaptureDeviceCount();
        const int cur = static_cast<int>(InterlockedCompareExchange(&g_mic_device_id, 0, 0));
        int next = cur + 1;
        if (next >= num_devs) {
            next = -1;
        }
        SelectMicDevice(next, true);
        MessageBeep(MB_OK);
    }
    s_f8_down = down;
}

std::string JsonEscape(const char* s) {
    std::string out;
    if (!s) {
        return out;
    }
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(*p) >= 0x20) {
            out.push_back(*p);
        }
    }
    return out;
}

void FlushFriendsToUi() {
    if (!g_friend_status_cb || !g_friend_status_ctx) {
        return;
    }
    if (InterlockedExchange(&g_friends_dirty, 0) == 0) {
        return;
    }
    EnsureFriendsLock();
    std::vector<IngameFriendEntry> snap;
    EnterCriticalSection(&g_friends_lock);
    snap = g_cached_friends;
    LeaveCriticalSection(&g_friends_lock);

    for (const auto& e : snap) {
        if (e.kind == 0 || e.kind == 1) {
            g_friend_status_cb(g_friend_status_ctx, e.username.c_str(), e.status, e.info.c_str());
        }
    }
}

void __cdecl Hook_GS_RegisterFriendMsgCb(void* ctx, FriendMsgCbFn cb) {
    g_friend_msg_ctx = ctx;
    g_friend_msg_cb = cb;
}
void __cdecl Hook_GS_RegisterFriendStatusCb(void* ctx, FriendStatusCbFn cb) {
    g_friend_status_ctx = ctx;
    g_friend_status_cb = cb;
    if (cb && ctx) {
        InterlockedExchange(&g_friends_dirty, 1);
        FlushFriendsToUi();
    }
}
void __cdecl Hook_GS_RegisterFriendListCb(void* ctx, FriendListCbFn cb) {
    g_friend_list_ctx = ctx;
    g_friend_list_cb = cb;
}
void __cdecl Hook_GS_RegisterFriendAddedCb(void* ctx, FriendActionCbFn cb) {
    g_friend_added_ctx = ctx;
    g_friend_added_cb = cb;
}
void __cdecl Hook_GS_RegisterFriendRemovedCb(void* ctx, FriendActionCbFn cb) {
    g_friend_removed_ctx = ctx;
    g_friend_removed_cb = cb;
}
bool __cdecl Hook_GS_RequestFriendsList() {
    if (g_friends_sync_event) {
        SetEvent(g_friends_sync_event);
    }
    InterlockedExchange(&g_friends_dirty, 1);
    return true;
}
bool __cdecl Hook_GS_AddFriend(const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }
    int incoming_id = 0;
    EnsureFriendsLock();
    EnterCriticalSection(&g_friends_lock);
    for (const auto& e : g_cached_friends) {
        if (e.kind == 1 && _stricmp(e.username.c_str(), name) == 0) {
            incoming_id = e.id;
            break;
        }
    }
    LeaveCriticalSection(&g_friends_lock);

    if (incoming_id > 0) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "{\"id\":%d}", incoming_id);
        DispatchFriendOpAsync(L"/api/ingame/friends/accept", buf);
    } else {
        DispatchFriendOpAsync(L"/api/ingame/friends/add", std::string("{\"username\":\"") + JsonEscape(name) + "\"}");
    }
    if (g_friend_added_cb && g_friend_added_ctx) {
        g_friend_added_cb(g_friend_added_ctx, name);
    }
    return true;
}
bool __cdecl Hook_GS_RemoveFriend(const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }
    int target_id = 0;
    EnsureFriendsLock();
    EnterCriticalSection(&g_friends_lock);
    for (const auto& e : g_cached_friends) {
        if (_stricmp(e.username.c_str(), name) == 0) {
            target_id = e.id;
            break;
        }
    }
    LeaveCriticalSection(&g_friends_lock);

    if (target_id > 0) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "{\"id\":%d}", target_id);
        DispatchFriendOpAsync(L"/api/ingame/friends/remove", buf);
    }
    if (g_friend_removed_cb && g_friend_removed_ctx) {
        g_friend_removed_cb(g_friend_removed_ctx, name);
    }
    return true;
}
bool __cdecl Hook_GS_HasFriend() {
    EnsureFriendsLock();
    EnterCriticalSection(&g_friends_lock);
    const bool any = !g_cached_friends.empty();
    LeaveCriticalSection(&g_friends_lock);
    return any;
}
void __cdecl Hook_GS_RegisterGameRoomCb(void* lobby_online, OnlineRoomFoundCbFn cb) {
    if (cb != reinterpret_cast<OnlineRoomFoundCbFn>(0x10B48CB0) && cb != nullptr) {
        return;
    }
    if (cb == nullptr) {
        if (RoomPtrOk(lobby_online) && lobby_online == g_lobby_online_ctx) {
            if (InterlockedExchange(&g_online_browse_registered, 0) != 0) {
                reinterpret_cast<QuazalUnregisterListenerFn>(0x10961F90)(&g_quazal_browse_listener);
            }
        }
        return;
    }
    g_lobby_online_ctx = reinterpret_cast<uint8_t*>(lobby_online);
    g_online_room_found_cb = cb;
    if (RoomPtrOk(lobby_online)) {
        *reinterpret_cast<uint32_t*>(0x111F55DC) = 0;
        if (!reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)()) {
            void* engine = nullptr;
            uint8_t* vb = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + 4);
            if (RoomPtrOk(vb)) {
                int32_t vb_off = *reinterpret_cast<int32_t*>(vb + 4);
                uint8_t* page_obj = *reinterpret_cast<uint8_t**>(g_lobby_online_ctx + vb_off + 0x18);
                if (RoomPtrOk(page_obj)) {
                    void* obj4 = *reinterpret_cast<void**>(page_obj + 4);
                    if (RoomPtrOk(obj4)) {
                        using GetEngineFn = void*(__thiscall*)(void*);
                        engine = reinterpret_cast<GetEngineFn>(0x10A0DCE0)(obj4);
                    }
                }
            }
            if (RoomPtrOk(engine)) {
                Hook_QuazalInit(engine, 0);
            }
        }
        if (InterlockedCompareExchange(&g_online_browse_registered, 1, 0) == 0) {
            reinterpret_cast<QuazalRegisterListenerFn>(0x10961F00)(&g_quazal_browse_listener);
        }
        g_last_online_browse_ms = GetTickCount64();
        Hook_QuazalStartBrowse();
    }
}
bool __cdecl Hook_GS_RegisterCreatedRoom(void*, void*, const void* desc) {
    if (RoomPtrOk(desc)) {
        std::memcpy(g_active_online_desc, desc, sizeof(g_active_online_desc));
        *reinterpret_cast<uint32_t*>(g_active_online_desc + 0x8BC) = 0;
        g_has_active_online_desc = true;
    }
    g_online_room_created = true;
    g_online_active_room_id = 1;
    return true;
}
bool __cdecl Hook_GS_PollRoomInfo(int* out_room_id, int* out_chat_id, int* out_max_players, char** out_str,
                                  void* out_desc) {
    const bool ready = g_online_room_created || g_online_join_requested ||
                       (g_online_active_room_id != 0 && reinterpret_cast<QuazalPollFn>(0x1095B8A0)());
    if (!ready) {
        if (out_room_id) {
            *out_room_id = 0;
        }
        return false;
    }
    g_online_active_room_id = 1;
    if (out_room_id) {
        *out_room_id = 1;
    }
    if (out_chat_id) {
        *out_chat_id = 1;
    }
    if (out_max_players) {
        *out_max_players = 2;
    }
    if (out_str && *out_str) {
        (*out_str)[0] = '\0';
    }
    if (RoomPtrOk(out_desc)) {
        if (reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)() && reinterpret_cast<QuazalPollFn>(0x1095B8A0)()) {
            reinterpret_cast<QuazalGetSessionDescFn>(0x1095D700)(out_desc);
            std::memcpy(g_active_online_desc, out_desc, sizeof(g_active_online_desc));
            g_has_active_online_desc = true;
        } else if (g_has_active_online_desc) {
            std::memcpy(out_desc, g_active_online_desc, sizeof(g_active_online_desc));
        }
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(out_desc) + 0x8BC) = 0;
    }
    return true;
}
bool __cdecl Hook_GS_RequestJoinRoom(int room_id, const char*) {
    g_online_active_room_id = room_id != 0 ? room_id : 1;
    g_online_join_requested = true;
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_join_requested id=%d",
         heure, GetCurrentProcessId(), g_online_active_room_id);
    return true;
}
bool __cdecl Hook_GS_LeaveRoomOrJoinFinish(int, int) {
    g_online_join_requested = false;
    g_online_room_created = false;
    g_online_active_room_id = 0;
    return true;
}
bool __cdecl Hook_GS_GetIpPortForId(int, char* out_ip, int* out_flag) {
    if (out_ip) {
        out_ip[0] = '\0';
    }
    if (out_flag) {
        *out_flag = 0;
    }
    return false;
}
bool __cdecl Hook_GS_GetRoomPwd(int, char* out_a, char* out_b) {
    if (out_a) {
        out_a[0] = '\0';
    }
    if (out_b) {
        out_b[0] = '\0';
    }
    return false;
}
void __cdecl Hook_GS_WriteKeyZero(void* out) {
    if (out) {
        std::snprintf(static_cast<char*>(out), 32, "0");
    }
}
bool __cdecl Hook_GS_KickPlayerRecord(const uint8_t* record) {
    if (!RoomPtrOk(record)) {
        return false;
    }
    const uint8_t participant_id = record[0x30];
    const int ok = reinterpret_cast<LanKickByIdFn>(0x10962CC0)(participant_id);
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
         "note=online_kick_record_sent id=%u ok=%d",
         heure, GetCurrentProcessId(), static_cast<unsigned>(participant_id), ok);
    return ok != 0;
}

// --- Online UI Page Hooks (P_Login, P_LobbyOnline, P_RoomOnline) ---
char __fastcall Hook_LoginOnShow(uint8_t* login_plus_8, void*, void* page) {
    UpdateOnlineIdentityBuffers();
    const char ok = g_orig_LoginOnShow(login_plus_8, page);
    if (RoomPtrOk(login_plus_8) && RoomPtrOk(page)) {
        uint8_t* login_page = *reinterpret_cast<uint8_t**>(login_plus_8 + 0x30);
        if (RoomPtrOk(login_page) &&
            *reinterpret_cast<uint32_t*>(login_page + 0x14) ==
                *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(page) + 0x14)) {
            SetUeEditBoxText(*reinterpret_cast<void**>(login_plus_8 + 0x44), g_online_username_w);
            SetUeEditBoxText(*reinterpret_cast<void**>(login_plus_8 + 0x48), g_online_password_w);
        }
    }
    return ok;
}

char __fastcall Hook_LobbyOnlineTick(uint8_t* lobby_plus_e8, void*, void* arg1, float dt) {
    const char handled = g_orig_LobbyOnlineTick(lobby_plus_e8, arg1, dt);
    uint8_t* lobby = RoomPtrOk(lobby_plus_e8) ? (lobby_plus_e8 - 0xE8) : nullptr;
    if (RoomPtrOk(lobby) && g_shot_dir[0] != L'\0' && g_lobby_online_ctx == lobby) {
        wchar_t join_req[MAX_PATH]{};
        _snwprintf_s(join_req, MAX_PATH, _TRUNCATE, L"%ls\\join_%lu.req", g_shot_dir, GetCurrentProcessId());
        if (GetFileAttributesW(join_req) != INVALID_FILE_ATTRIBUTES) {
            uint8_t* lst_server = *reinterpret_cast<uint8_t**>(lobby + 0x0C);
            if (RoomPtrOk(lst_server) && *reinterpret_cast<int16_t*>(lst_server + 0xA8) >= 0) {
                DeleteFileW(join_req);
                using OnJoinFn = void(__thiscall*)(void*);
                reinterpret_cast<OnJoinFn>(0x10B47960)(lobby);
                char heure[40];
                NowText(heure, sizeof(heure));
                Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_join_trigger",
                     heure, GetCurrentProcessId());
            }
        }
    }
    if (RoomPtrOk(lobby) && g_lobby_online_ctx == lobby && g_online_room_found_cb != nullptr && !g_online_join_requested &&
        !reinterpret_cast<QuazalPollFn>(0x1095B8A0)()) {
        const uint64_t now = GetTickCount64();
        if (now - g_last_online_browse_ms >= 1500) {
            g_last_online_browse_ms = now;
            Hook_QuazalStartBrowse();
        }
    }
    FlushFriendsToUi();
    PollMicHotkey();
    return handled;
}

char __fastcall Hook_RoomOnlineTick(uint8_t* room_plus_80, void*, void* arg1, float dt) {
    if (RoomPtrOk(room_plus_80) && InterlockedExchange(&g_kicked_from_online_session, 0) != 0) {
        g_online_join_requested = false;
        g_online_room_created = false;
        g_online_active_room_id = 0;
        reinterpret_cast<RoomOnlineOnMsgBoxFn>(0x10B40B30)(room_plus_80 + 0x08, 0x38);
        char heure[40];
        NowText(heure, sizeof(heure));
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_kick_executed",
             heure, GetCurrentProcessId());
        return 1;
    }
    uint8_t* room = RoomPtrOk(room_plus_80) ? (room_plus_80 - 0x80) : nullptr;
    if (RoomPtrOk(room) && reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)()) {
        SessionPlayerArray arr{};
        reinterpret_cast<GetSessionPlayersFn>(0x10963370)(&arr);
        *reinterpret_cast<int32_t*>(room + 0x60) = arr.count;
        static bool s_logged_guest_ready = false;
        if (arr.count < 2) {
            s_logged_guest_ready = false;
        } else if (reinterpret_cast<IsLanHostFn>(0x1095B880)() && !s_logged_guest_ready && arr.data != nullptr) {
            bool has_non_host = false;
            bool all_non_host_ready = true;
            for (int i = 0; i < arr.count; ++i) {
                const uint8_t* entry = arr.data + i * 0xB0;
                if (entry[0x2A] == 0) {
                    has_non_host = true;
                    if (entry[0x28] == 0) {
                        all_non_host_ready = false;
                        break;
                    }
                }
            }
            if (has_non_host && all_non_host_ready) {
                s_logged_guest_ready = true;
                char heure[40];
                NowText(heure, sizeof(heure));
                Logf("heure=%s pid=%lu dir=etat local=- pair=- len=%d hex= seq=0 tronque=0 note=online_guest_ready",
                     heure, GetCurrentProcessId(), arr.count);
            }
        }
        if (arr.data != nullptr) {
            reinterpret_cast<EmptyPlayerArrayFn>(0x10AA8E50)(&arr, 0, arr.count, 0xB0);
            GMallocObj* gmalloc = *reinterpret_cast<GMallocObj**>(0x111C88A0);
            if (gmalloc && gmalloc->vtbl && gmalloc->vtbl->Free) {
                gmalloc->vtbl->Free(gmalloc, arr.data);
            }
        }
    }
    const char handled = g_orig_RoomOnlineTick(room_plus_80, arg1, dt);
    if (RoomPtrOk(room_plus_80) && g_shot_dir[0] != L'\0' && reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)() &&
        reinterpret_cast<IsLanHostFn>(0x1095B880)()) {
        wchar_t kick_req[MAX_PATH]{};
        _snwprintf_s(kick_req, MAX_PATH, _TRUNCATE, L"%ls\\kick_%lu.req", g_shot_dir, GetCurrentProcessId());
        if (GetFileAttributesW(kick_req) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(kick_req);
            KickSecondPlayerFromSession("online");
        }
    }
    return handled;
}

char __fastcall Hook_RoomOnlineLaunchGame(uint8_t* room_online, void*) {
    uint8_t desc[0x8CC]{};
    int ffire = g_default_ffire;
    if (reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)()) {
        reinterpret_cast<QuazalSessionDescCtorFn>(0x1095C840)(desc);
        reinterpret_cast<QuazalGetSessionDescFn>(0x1095D700)(desc);
        ffire = *reinterpret_cast<int32_t*>(desc + 0x474);
    }
    char heure[40];
    NowText(heure, sizeof(heure));
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=online_launch_game ffire=%d",
         heure, GetCurrentProcessId(), ffire);
    return g_orig_RoomOnlineLaunchGame(room_online);
}

char __fastcall Hook_SessionOptionsInit(uint8_t* page, void*, int a1, int a2, int a3) {
    const char ok = g_orig_SessionOptionsInit(page, a1, a2, a3);
    GMallocObj* gmalloc = *reinterpret_cast<GMallocObj**>(0x111C88A0);
    if (RoomPtrOk(page) && RoomPtrOk(gmalloc) && RoomPtrOk(gmalloc->vtbl) && RoomPtrOk(gmalloc->vtbl->Malloc)) {
        void* mem = gmalloc->vtbl->Malloc(gmalloc, 0x10, reinterpret_cast<const void*>(0x110E30BC));
        if (mem != nullptr) {
            auto* obj = static_cast<uint32_t*>(mem);
            obj[0] = reinterpret_cast<uint32_t>(&g_ffire_vtable[0]);
            obj[1] = static_cast<uint32_t>(page[0xB4]);
            obj[2] = 0;
            obj[3] = (*reinterpret_cast<int32_t*>(page + 0x94) != 0) ? 1u : 0u;
            reinterpret_cast<AddOptionFn>(0x10B762D0)(page + 0x14, mem);
            if (InterlockedCompareExchange(&g_logged_ffire, 1, 0) == 0) {
                Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
                     "note=friendly_fire_option_added default=%u is_modify=%u",
                     GetCurrentProcessId(), obj[3], obj[1]);
            }
        }
    }
    return ok;
}

void __fastcall Hook_LobbyLanTick(uint8_t* lobby, void*, float dt) {
    if (RoomPtrOk(lobby) && InterlockedExchange(&g_kicked_from_lan_session, 0) != 0) {
        reinterpret_cast<QuazalLeaveSessionFn>(0x109620C0)();
        reinterpret_cast<LobbyLanLeaveConfirmedFn>(0x10B4C0E0)(lobby + 0x68, 0x38);
        char heure[40];
        NowText(heure, sizeof(heure));
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=lan_kick_executed",
             heure, GetCurrentProcessId());
        return;
    }
    g_orig_LobbyLanTick(lobby, dt);
    if (!RoomPtrOk(lobby)) {
        return;
    }
    if (reinterpret_cast<IsInLanSessionFn>(0x1095D1C0)() && reinterpret_cast<IsLanHostFn>(0x1095B880)()) {
        const uint32_t fname_kick = EnsureBKickFName();
        int32_t* vbptr = *reinterpret_cast<int32_t**>(lobby + 0x04);
        if (fname_kick != 0 && RoomPtrOk(vbptr)) {
            void* page_container = *reinterpret_cast<void**>(lobby + vbptr[1] + 0x3C);
            if (RoomPtrOk(page_container)) {
                void* kick_btn = reinterpret_cast<FindChildWidgetFn>(0x10D30670)(page_container, fname_kick);
                if (RoomPtrOk(kick_btn)) {
                    SessionPlayerArray arr{};
                    reinterpret_cast<GetSessionPlayersFn>(0x10963370)(&arr);
                    const int count = arr.count;
                    if (arr.data != nullptr) {
                        reinterpret_cast<EmptyPlayerArrayFn>(0x10AA8E50)(&arr, 0, arr.count, 0xB0);
                        GMallocObj* gmalloc = *reinterpret_cast<GMallocObj**>(0x111C88A0);
                        if (gmalloc && gmalloc->vtbl && gmalloc->vtbl->Free) {
                            gmalloc->vtbl->Free(gmalloc, arr.data);
                        }
                    }
                    auto* btn_bytes = static_cast<uint8_t*>(kick_btn);
                    if (count >= 2) {
                        btn_bytes[0x18] |= 1;
                    } else {
                        btn_bytes[0x18] &= static_cast<uint8_t>(~1);
                    }
                    reinterpret_cast<SetWidgetVisibleFn>(0x10D32E70)(kick_btn, 1);
                }
            }
        }
        if (g_shot_dir[0] != L'\0') {
            wchar_t kick_req[MAX_PATH]{};
            _snwprintf_s(kick_req, MAX_PATH, _TRUNCATE, L"%ls\\kick_%lu.req", g_shot_dir, GetCurrentProcessId());
            if (GetFileAttributesW(kick_req) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(kick_req);
                KickSecondPlayerInLanLobby();
            }
        }
    }
}

char __fastcall Hook_LobbyLanOnCommand(uint8_t* lobby_plus_58, void*, uint8_t* cmd) {
    if (RoomPtrOk(cmd)) {
        const uint32_t fname_kick = EnsureBKickFName();
        const uint32_t widget_fname = *reinterpret_cast<uint32_t*>(cmd + 4);
        if (fname_kick != 0 && widget_fname == fname_kick && reinterpret_cast<IsLanHostFn>(0x1095B880)()) {
            KickSecondPlayerInLanLobby();
            return 1;
        }
    }
    return g_orig_LobbyLanOnCommand(lobby_plus_58, cmd);
}

int __fastcall Hook_EngineInitVoiceChat(uint8_t* engine, void*) {
    if (g_enable_voice != 0) {
        uint8_t* opts = *reinterpret_cast<uint8_t**>(0x111F9F64);
        if (RoomPtrOk(opts)) {
            *reinterpret_cast<uint32_t*>(opts + 0x30) |= 1u;
            if (opts[0x2A] == 0) {
                opts[0x2A] = 50;
            }
        }
    }
    const int ok = g_orig_EngineInitVoiceChat(engine);
    if (g_enable_voice != 0) {
        using GetVoiceWrapperFn = uint8_t*(__cdecl*)();
        using CreateVoiceWrapperFn = uint8_t*(__cdecl*)();
        using SetVoiceNoiseParamsFn = void(__stdcall*)(int, float);
        uint8_t* wrapper = reinterpret_cast<GetVoiceWrapperFn>(0x10AE72E0)();
        if (!wrapper) {
            wrapper = reinterpret_cast<CreateVoiceWrapperFn>(0x10AE79A0)();
        }
        if (RoomPtrOk(wrapper)) {
            *reinterpret_cast<void**>(wrapper + 0x18) = engine;
        }
        uint8_t* opts = *reinterpret_cast<uint8_t**>(0x111F9F64);
        const float sens = RoomPtrOk(opts) ? static_cast<float>(opts[0x2A]) : 50.0f;
        reinterpret_cast<SetVoiceNoiseParamsFn>(0x10AE73E0)(1, sens > 0.0f ? sens : 50.0f);
        StartMicCapture();
        if (InterlockedCompareExchange(&g_logged_voice_init, 1, 0) == 0) {
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
                 "note=voice_chat_enabled sens=%.0f wrapper=0x%08lX mic_devs=%d",
                 GetCurrentProcessId(), sens > 0.0f ? sens : 50.0f,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(wrapper)), VoiceAudioGetCaptureDeviceCount());
        }
    }
    return ok;
}

void __fastcall Hook_VoiceTick(uint8_t* wrapper, void*) {
    g_orig_VoiceTick(wrapper);
    PollMicHotkey();
    if (g_enable_voice == 0 || !RoomPtrOk(wrapper)) {
        return;
    }
    uint8_t* opts = *reinterpret_cast<uint8_t**>(0x111F9F64);
    if (RoomPtrOk(opts)) {
        *reinterpret_cast<uint32_t*>(opts + 0x30) |= 1u;
    }

    const uint64_t now = GetTickCount64();
    if (g_shot_dir[0] != L'\0') {
        wchar_t voice_req[MAX_PATH]{};
        _snwprintf_s(voice_req, MAX_PATH, _TRUNCATE, L"%ls\\voice_%lu.req", g_shot_dir, GetCurrentProcessId());
        if (GetFileAttributesW(voice_req) != INVALID_FILE_ATTRIBUTES) {
            char buf[64]{};
            DWORD got = 0;
            HANDLE vf = CreateFileW(voice_req, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (vf != INVALID_HANDLE_VALUE) {
                ReadFile(vf, buf, sizeof(buf) - 1, &got, nullptr);
                CloseHandle(vf);
            }
            DeleteFileW(voice_req);
            if (got > 0) {
                const int parsed = std::atoi(buf);
                g_voice_hold_level = static_cast<uint32_t>(parsed < 0 ? 0 : (parsed > 100 ? 100 : parsed));
                g_voice_hold_until = now + 4000;
            }
        }
    }

    uint32_t level = static_cast<uint32_t>(InterlockedExchangeAdd(&g_mic_peak, 0));
    if (g_voice_test_level > 0 && static_cast<uint32_t>(g_voice_test_level) > level) {
        level = static_cast<uint32_t>(g_voice_test_level);
    }
    if (now < g_voice_hold_until) {
        level = g_voice_hold_level;
    }

    uint8_t* net_mgr = *reinterpret_cast<uint8_t**>(0x111F5554);
    if (!RoomPtrOk(net_mgr)) {
        return;
    }
    uint8_t* engine = *reinterpret_cast<uint8_t**>(wrapper + 0x18);
    if (!RoomPtrOk(engine)) {
        engine = *reinterpret_cast<uint8_t**>(net_mgr + 0x44);
    }
    if (!RoomPtrOk(engine)) {
        return;
    }

    uint8_t* client = *reinterpret_cast<uint8_t**>(engine + 0x44);
    uint8_t* vps = RoomPtrOk(client) ? *reinterpret_cast<uint8_t**>(client + 0x2C) : nullptr;
    uint8_t* vp0 = RoomPtrOk(vps) ? *reinterpret_cast<uint8_t**>(vps) : nullptr;
    uint8_t* player_mgr = RoomPtrOk(vp0) ? *reinterpret_cast<uint8_t**>(vp0 + 0x64) : nullptr;
    uint8_t* controller = RoomPtrOk(player_mgr) ? reinterpret_cast<GetLocalControllerFn>(0x10A45AF0)(player_mgr) : nullptr;
    if (!RoomPtrOk(controller)) {
        return;
    }
    const uint32_t threshold = *reinterpret_cast<uint32_t*>(0x11223780);
    if (level < threshold) {
        if (now - g_last_voice_emit_ms > 250) {
            *reinterpret_cast<float*>(controller + 0x440) = 0.0f;
        }
        return;
    }
    uint8_t* pawn = *reinterpret_cast<uint8_t**>(controller + 0x2B4);
    if (!RoomPtrOk(pawn)) {
        return;
    }

    // Replicate voice noise event over PRUDP 9103 every ~120ms and reflect directly on local meter.
    if (now - g_last_voice_emit_ms >= 120) {
        g_last_voice_emit_ms = now;
        *reinterpret_cast<uint32_t*>(wrapper + 0x0C) = 0;
        reinterpret_cast<OnLocalVoiceNoiseFn>(0x10AE7720)(wrapper, level, 0);
    }

    const float coeff = *reinterpret_cast<float*>(0x1122378C);
    const float noise_val = static_cast<float>(level) * coeff;
    *reinterpret_cast<float*>(controller + 0x440) = noise_val;

    // Trigger local MakeNoise so the local Pawn's stealth sound meter and AI react immediately.
    void* level_obj = *reinterpret_cast<void**>(net_mgr + 0x48);
    if (RoomPtrOk(level_obj) && *reinterpret_cast<int32_t*>(pawn + 0x420) > 0) {
        void** lvl_vtbl = *reinterpret_cast<void***>(level_obj);
        if (RoomPtrOk(lvl_vtbl) && RoomPtrOk(lvl_vtbl[0x194 / 4])) {
            float scaled = 0.0f;
            if (noise_val <= 60.0f) {
                scaled = static_cast<float>(static_cast<double>(noise_val) * 3.333);
            } else {
                const double diff = static_cast<double>(noise_val) - 60.0;
                scaled = static_cast<float>((std::pow(diff, 2.0) * 0.0088888888888 + 2.0) * 100.0);
            }
            const uint32_t low_max = *reinterpret_cast<uint32_t*>(0x111F553C);
            const uint32_t med_max = *reinterpret_cast<uint32_t*>(0x111F5540);
            int radius_cat = (noise_val <= static_cast<float>(low_max)) ? 0x3B
                             : (noise_val > static_cast<float>(med_max) ? 0x6C : 0x5B);
            float pos[3] = {
                *reinterpret_cast<float*>(pawn + 0xE8),
                *reinterpret_cast<float*>(pawn + 0xEC),
                *reinterpret_cast<float*>(pawn + 0xF0),
            };
            reinterpret_cast<MakeNoiseFn>(lvl_vtbl[0x194 / 4])(level_obj, pawn, scaled, radius_cat, pos, pawn, pos, 0);
        }
    }

    if (now - g_last_voice_log_ms >= 500) {
        g_last_voice_log_ms = now;
        char heure[40];
        NowText(heure, sizeof(heure));
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=voice_meter_active level=%u controller_440=%.1f",
             heure, GetCurrentProcessId(), level, *reinterpret_cast<float*>(controller + 0x440));
    }
}

void ScdaLogSink(const char* msg) {
    Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=%s",
         GetCurrentProcessId(), msg ? msg : "");
}

void InstallFeatureHooks() {
    struct FeatureHook {
        const char* name;
        uintptr_t addr;
        void* detour;
        void** original;
    } hooks[] = {
        {"SetLocalSessionPlayerName", 0x1095D8D0u, reinterpret_cast<void*>(&Hook_SetLocalSessionPlayerName),
         reinterpret_cast<void**>(&g_orig_SetLocalSessionPlayerName)},
        {"GetCurrentProfile", 0x10AD6950u, reinterpret_cast<void*>(&Hook_GetCurrentProfile),
         reinterpret_cast<void**>(&g_orig_GetCurrentProfile)},
        {"SessionOptionsReset", 0x10B51290u, reinterpret_cast<void*>(&Hook_SessionOptionsReset),
         reinterpret_cast<void**>(&g_orig_SessionOptionsReset)},
        {"SessionOptionsInit", 0x10B51350u, reinterpret_cast<void*>(&Hook_SessionOptionsInit),
         reinterpret_cast<void**>(&g_orig_SessionOptionsInit)},
        {"LobbyLanTick", 0x10B4D1C0u, reinterpret_cast<void*>(&Hook_LobbyLanTick),
         reinterpret_cast<void**>(&g_orig_LobbyLanTick)},
        {"LobbyLanOnCommand", 0x10B4CA10u, reinterpret_cast<void*>(&Hook_LobbyLanOnCommand),
         reinterpret_cast<void**>(&g_orig_LobbyLanOnCommand)},
        {"EngineInitVoiceChat", 0x10A3C7F0u, reinterpret_cast<void*>(&Hook_EngineInitVoiceChat),
         reinterpret_cast<void**>(&g_orig_EngineInitVoiceChat)},
        {"VoiceTick", 0x10AE73A0u, reinterpret_cast<void*>(&Hook_VoiceTick),
         reinterpret_cast<void**>(&g_orig_VoiceTick)},
        {"MatchNameInitDefault", 0x10B734E0u, reinterpret_cast<void*>(&Hook_MatchNameInitDefault),
         reinterpret_cast<void**>(&g_orig_MatchNameInitDefault)},
        {"OnKickReceived", 0x1095D860u, reinterpret_cast<void*>(&Hook_OnKickReceived),
         reinterpret_cast<void**>(&g_orig_OnKickReceived)},
        // Quazal direct UDP mode while inside Online UI
        {"QuazalSessionDescCtor", 0x1095C840u, reinterpret_cast<void*>(&Hook_QuazalSessionDescCtor),
         reinterpret_cast<void**>(&g_orig_QuazalSessionDescCtor)},
        {"QuazalInit", 0x10963FE0u, reinterpret_cast<void*>(&Hook_QuazalInit),
         reinterpret_cast<void**>(&g_orig_QuazalInit)},
        {"IsOnlineSession", 0x1095B7F0u, reinterpret_cast<void*>(&Hook_IsOnlineSession),
         reinterpret_cast<void**>(&g_orig_IsOnlineSession)},
        {"QuazalCreateSession", 0x10960AF0u, reinterpret_cast<void*>(&Hook_QuazalCreateSession),
         reinterpret_cast<void**>(&g_orig_QuazalCreateSession)},
        {"QuazalJoinSession", 0x1095FEE0u, reinterpret_cast<void*>(&Hook_QuazalJoinSession),
         reinterpret_cast<void**>(&g_orig_QuazalJoinSession)},
        {"QuazalStartBrowse", 0x109620C0u, reinterpret_cast<void*>(&Hook_QuazalStartBrowse),
         reinterpret_cast<void**>(&g_orig_QuazalStartBrowse)},
        // Online UI page hooks
        {"LoginOnShow", 0x10B42D30u, reinterpret_cast<void*>(&Hook_LoginOnShow),
         reinterpret_cast<void**>(&g_orig_LoginOnShow)},
        {"LobbyOnlineTick", 0x10B4A240u, reinterpret_cast<void*>(&Hook_LobbyOnlineTick),
         reinterpret_cast<void**>(&g_orig_LobbyOnlineTick)},
        {"RoomOnlineTick", 0x10B41CD0u, reinterpret_cast<void*>(&Hook_RoomOnlineTick),
         reinterpret_cast<void**>(&g_orig_RoomOnlineTick)},
        {"RoomOnlineLaunchGame", 0x10B40220u, reinterpret_cast<void*>(&Hook_RoomOnlineLaunchGame),
         reinterpret_cast<void**>(&g_orig_RoomOnlineLaunchGame)},
        // GameService Online wrapper hooks (0x10AA3F90..0x10AA4B70)
        {"GS_Init", 0x10AA4A80u, reinterpret_cast<void*>(&Hook_GS_Init), nullptr},
        {"GS_Disconnect", 0x10AA4020u, reinterpret_cast<void*>(&Hook_GS_Disconnect), nullptr},
        {"GS_SetProfileName", 0x10AA4240u, reinterpret_cast<void*>(&Hook_GS_NoOpPtr), nullptr},
        {"GS_SetUsername", 0x10AA4260u, reinterpret_cast<void*>(&Hook_GS_SetUsername), nullptr},
        {"GS_SetPassword", 0x10AA4280u, reinterpret_cast<void*>(&Hook_GS_NoOpPtr), nullptr},
        {"GS_ClearError", 0x10AA4100u, reinterpret_cast<void*>(&Hook_GS_NoOp0), nullptr},
        {"GS_ValidateName", 0x10AA4AF0u, reinterpret_cast<void*>(&Hook_GS_ValidateName), nullptr},
        {"GS_StartLogin", 0x10AA3F90u, reinterpret_cast<void*>(&Hook_GS_StartLogin), nullptr},
        {"GS_IsLoginStep1Done", 0x10AA41E0u, reinterpret_cast<void*>(&Hook_GS_IsLoginStep1Done), nullptr},
        {"GS_LoginStep2a", 0x10AA4000u, reinterpret_cast<void*>(&Hook_GS_NoOp0), nullptr},
        {"GS_LoginStep2b", 0x10AA3FB0u, reinterpret_cast<void*>(&Hook_GS_NoOp0), nullptr},
        {"GS_LoginStep2c", 0x10AA4010u, reinterpret_cast<void*>(&Hook_GS_NoOp0), nullptr},
        {"GS_IsLoggedIn", 0x10AA41C0u, reinterpret_cast<void*>(&Hook_GS_IsLoggedIn), nullptr},
        {"GS_PollError", 0x10AA40D0u, reinterpret_cast<void*>(&Hook_GS_PollError), nullptr},
        {"GS_GetUsername", 0x10AA42A0u, reinterpret_cast<void*>(&Hook_GS_GetUsername), nullptr},
        {"GS_GetPassword", 0x10AA42C0u, reinterpret_cast<void*>(&Hook_GS_GetPassword), nullptr},
        {"GS_GetKey", 0x10AA42E0u, reinterpret_cast<void*>(&Hook_GS_GetKey), nullptr},
        {"GS_IsAutoJoin", 0x10AA4860u, reinterpret_cast<void*>(&Hook_GS_IsAutoJoin), nullptr},
        {"GS_GetStatusText1", 0x10AA4080u, reinterpret_cast<void*>(&Hook_GS_GetStatusText1), nullptr},
        {"GS_GetStatusText2", 0x10AA4050u, reinterpret_cast<void*>(&Hook_GS_GetStatusText2), nullptr},
        {"GS_RegisterLobbyCb", 0x10AA4430u, reinterpret_cast<void*>(&Hook_GS_RegisterLobbyCb), nullptr},
        {"GS_RegisterMotdCb", 0x10AA40B0u, reinterpret_cast<void*>(&Hook_GS_RegisterMotdCb), nullptr},
        {"GS_JoinLobbyGroup", 0x10AA4380u, reinterpret_cast<void*>(&Hook_GS_JoinLobbyGroup), nullptr},
        {"GS_IsInLobbyGroup", 0x10AA4210u, reinterpret_cast<void*>(&Hook_GS_IsInLobbyGroup), nullptr},
        {"GS_OnLobbyGroupJoined", 0x10AA43F0u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_IsChatConnected", 0x10AA4620u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_GetCurrentLobbyIds", 0x10AA43C0u, reinterpret_cast<void*>(&Hook_GS_GetCurrentLobbyIds), nullptr},
        {"GS_RegisterMemberJoinCb", 0x10AA44B0u, reinterpret_cast<void*>(&Hook_GS_RegisterMemberJoinCb), nullptr},
        {"GS_RegisterMemberLeaveCb", 0x10AA44D0u, reinterpret_cast<void*>(&Hook_GS_NoOp2), nullptr},
        {"GS_RefreshMembers", 0x10AA4410u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_RegisterChatCb", 0x10AA45E0u, reinterpret_cast<void*>(&Hook_GS_NoOp2), nullptr},
        {"GS_SendChat", 0x10AA4600u, reinterpret_cast<void*>(&Hook_GS_NoOpPtr), nullptr},
        {"GS_RegisterGameRoomCb", 0x10AA4450u, reinterpret_cast<void*>(&Hook_GS_RegisterGameRoomCb), nullptr},
        {"GS_RegisterRemoveRoomCb", 0x10AA4470u, reinterpret_cast<void*>(&Hook_GS_NoOp2), nullptr},
        {"GS_HasPendingRoomList", 0x10AA4490u, reinterpret_cast<void*>(&Hook_GS_False0), nullptr},
        {"GS_IsCreateRoomReady", 0x10AA43A0u, reinterpret_cast<void*>(&Hook_GS_False0), nullptr},
        {"GS_RegisterCreatedRoom", 0x10AA44F0u, reinterpret_cast<void*>(&Hook_GS_RegisterCreatedRoom), nullptr},
        {"GS_PollRoomInfo", 0x10AA4510u, reinterpret_cast<void*>(&Hook_GS_PollRoomInfo), nullptr},
        {"GS_RequestJoinRoom", 0x10AA4540u, reinterpret_cast<void*>(&Hook_GS_RequestJoinRoom), nullptr},
        {"GS_IsJoinApproved", 0x10AA4190u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_HasFriend1", 0x10AA4130u, reinterpret_cast<void*>(&Hook_GS_HasFriend), nullptr},
        {"GS_HasFriend2", 0x10AA4160u, reinterpret_cast<void*>(&Hook_GS_HasFriend), nullptr},
        {"GS_RequestFriendsList", 0x10AA4660u, reinterpret_cast<void*>(&Hook_GS_RequestFriendsList), nullptr},
        {"GS_AddFriend", 0x10AA4680u, reinterpret_cast<void*>(&Hook_GS_AddFriend), nullptr},
        {"GS_RemoveFriend", 0x10AA46A0u, reinterpret_cast<void*>(&Hook_GS_RemoveFriend), nullptr},
        {"GS_RegisterFriendMsgCb", 0x10AA4740u, reinterpret_cast<void*>(&Hook_GS_RegisterFriendMsgCb), nullptr},
        {"GS_RegisterFriendStatusCb", 0x10AA4760u, reinterpret_cast<void*>(&Hook_GS_RegisterFriendStatusCb), nullptr},
        {"GS_RegisterFriendListCb", 0x10AA4780u, reinterpret_cast<void*>(&Hook_GS_RegisterFriendListCb), nullptr},
        {"GS_RegisterFriendAddedCb", 0x10AA47C0u, reinterpret_cast<void*>(&Hook_GS_RegisterFriendAddedCb), nullptr},
        {"GS_RegisterFriendRemovedCb", 0x10AA47E0u, reinterpret_cast<void*>(&Hook_GS_RegisterFriendRemovedCb), nullptr},
        {"GS_LeaveRoomOrJoinFinish", 0x10AA4560u, reinterpret_cast<void*>(&Hook_GS_LeaveRoomOrJoinFinish), nullptr},
        {"GS_UpdateRoomDesc", 0x10AA4580u, reinterpret_cast<void*>(&Hook_GS_True1), nullptr},
        {"GS_RequestLaunchGame", 0x10AA45A0u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_IsLaunchApproved", 0x10AA45C0u, reinterpret_cast<void*>(&Hook_GS_True0), nullptr},
        {"GS_SendKeepalive", 0x10AA48E0u, reinterpret_cast<void*>(&Hook_GS_True1), nullptr},
        {"GS_GetIpPortForId", 0x10AA4970u, reinterpret_cast<void*>(&Hook_GS_GetIpPortForId), nullptr},
        {"GS_GetRoomPwd", 0x10AA4990u, reinterpret_cast<void*>(&Hook_GS_GetRoomPwd), nullptr},
        {"GS_SetRoomExtra", 0x10AA49B0u, reinterpret_cast<void*>(&Hook_GS_NoOpPtr), nullptr},
        {"GS_WriteKeyZero", 0x10AA49D0u, reinterpret_cast<void*>(&Hook_GS_WriteKeyZero), nullptr},
        {"GS_KickPlayerRecord", 0x10AA4A10u, reinterpret_cast<void*>(&Hook_GS_KickPlayerRecord), nullptr},
        {"GS_GetUserId", 0x10AA4B70u, reinterpret_cast<void*>(&Hook_GS_Zero0), nullptr},
        {"GS_SetStateFlag", 0x10AA4920u, reinterpret_cast<void*>(&Hook_GS_NoOp0), nullptr},
    };
    int ok_count = 0;
    for (FeatureHook& h : hooks) {
        void* target = reinterpret_cast<void*>(h.addr);
        const MH_STATUS c = MH_CreateHook(target, h.detour, h.original);
        const MH_STATUS e = (c == MH_OK) ? MH_EnableHook(target) : c;
        if (e == MH_OK) {
            ++ok_count;
        } else {
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=feature_hook_%s_err (%s)",
                 GetCurrentProcessId(), h.name, MH_StatusToString(e));
        }
    }
    Logf("heure=0 pid=%lu dir=etat local=- pair=- len=%d hex= seq=0 tronque=0 "
         "note=feature_hooks_ready count=%d ffire_default=%d voice_enabled=%d",
         GetCurrentProcessId(), ok_count, ok_count, g_default_ffire, g_enable_voice);
    InstallScdaHooks(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), &ScdaLogSink);
}

void ShutdownHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_hooks = false;
    g_journal = false;
}

void RemoveHooks(void** targets, int count) {
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < count; ++i) {
        if (targets[i]) {
            MH_RemoveHook(targets[i]);
        }
    }
    MH_Uninitialize();
    g_hooks = false;
}

bool InstallHooks() {
    if (MH_Initialize() != MH_OK) {
        Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=MinHook init a echoue, hooks non poses",
             GetCurrentProcessId());
        return false;
    }
    struct Slot {
        const char* name;
        void* detour;
        void** original;
        void* target;
    };
    Slot slots[] = {
        {"bind", reinterpret_cast<void*>(&Hook_bind), reinterpret_cast<void**>(&g_bind), nullptr},
        {"sendto", reinterpret_cast<void*>(&Hook_sendto), reinterpret_cast<void**>(&g_sendto), nullptr},
        {"recvfrom", reinterpret_cast<void*>(&Hook_recvfrom), reinterpret_cast<void**>(&g_recvfrom), nullptr},
        {"WSASendTo", reinterpret_cast<void*>(&Hook_WSASendTo), reinterpret_cast<void**>(&g_WSASendTo), nullptr},
        {"WSARecvFrom", reinterpret_cast<void*>(&Hook_WSARecvFrom), reinterpret_cast<void**>(&g_WSARecvFrom), nullptr},
        {"WSAGetOverlappedResult", reinterpret_cast<void*>(&Hook_WSAGetOverlappedResult),
         reinterpret_cast<void**>(&g_WSAGetOverlappedResult), nullptr},
    };
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) {
        ws2 = LoadLibraryW(L"ws2_32.dll");
    }
    const auto ws2_base = reinterpret_cast<uintptr_t>(ws2);
    void* created[9] = {};
    int created_count = 0;
    for (Slot& slot : slots) {
        const auto proc = ws2 ? reinterpret_cast<uint8_t*>(GetProcAddress(ws2, slot.name)) : nullptr;
        char before[24] = "absente";
        if (proc) {
            uint8_t raw[8]{};
            if (CopyBytes(proc, 8, raw, 8) > 0) {
                HexBytes(raw, 8, before, sizeof(before));
            }
        }
        Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex=%s seq=0 tronque=0 note=avant_hook %s ws2_32=0x%08lX export=0x%08lX",
             GetCurrentProcessId(), before, slot.name, static_cast<unsigned long>(ws2_base),
             static_cast<unsigned long>(reinterpret_cast<uintptr_t>(proc)));
        const MH_STATUS status = MH_CreateHookApi(L"ws2_32.dll", slot.name, slot.detour, slot.original);
        if (status != MH_OK) {
            Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=hook %s refuse (%s), aucun hook laisse",
                 GetCurrentProcessId(), slot.name, MH_StatusToString(status));
            RemoveHooks(created, created_count);
            return false;
        }
        created[created_count++] = proc;
    }
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        struct KernelSlot {
            const char* name;
            void* detour;
            void** original;
        } kslots[] = {
            {"CreateMutexW", reinterpret_cast<void*>(&Hook_CreateMutexW), reinterpret_cast<void**>(&g_CreateMutexW)},
            {"CreateMutexA", reinterpret_cast<void*>(&Hook_CreateMutexA), reinterpret_cast<void**>(&g_CreateMutexA)},
        };
        for (KernelSlot& ks : kslots) {
            const auto proc = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, ks.name)) : nullptr;
            const MH_STATUS kstatus = MH_CreateHookApi(L"kernel32.dll", ks.name, ks.detour, ks.original);
            if (kstatus != MH_OK) {
                Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=hook %s refuse (%s), aucun hook laisse",
                     GetCurrentProcessId(), ks.name, MH_StatusToString(kstatus));
                RemoveHooks(created, created_count);
                return false;
            }
            created[created_count++] = proc;
        }
    }
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        d3d9 = LoadLibraryW(L"d3d9.dll");
    }
    if (d3d9 != nullptr) {
        g_Direct3DCreate9Ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));
        if (g_Direct3DCreate9Ex != nullptr) {
            void* proc = reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"));
            if (MH_CreateHookApi(L"d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&Hook_Direct3DCreate9),
                                 reinterpret_cast<void**>(&g_Direct3DCreate9)) == MH_OK) {
                created[created_count++] = proc;
            }
        }
    }
    const MH_STATUS enabled = MH_EnableHook(MH_ALL_HOOKS);
    if (enabled != MH_OK) {
        Logf("heure=0 pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=activation refusee (%s), hooks retires",
             GetCurrentProcessId(), MH_StatusToString(enabled));
        RemoveHooks(created, created_count);
        return false;
    }
    g_hooks = true;
    return true;
}

void LoadLaunchSessionIfNeeded() {
    wchar_t existing[16]{};
    if (GetEnvironmentVariableW(L"SCCT_ENABLED", existing, _countof(existing)) > 0) {
        return;
    }
    wchar_t local_app[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    const std::wstring session_path = std::wstring(local_app) + L"\\SCCTCoopLauncher\\data\\launch_session.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, session_path.c_str(), L"rb") != 0 || !f) {
        return;
    }
    char buf[8192]{};
    const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[got] = '\0';

    long long ts = 0;
    std::vector<std::pair<std::string, std::string>> kvs;
    size_t i = 0;
    while (i < got) {
        size_t end = i;
        while (end < got && buf[end] != '\n' && buf[end] != '\r') {
            ++end;
        }
        std::string line(buf + i, end - i);
        i = end + 1;
        while (i < got && (buf[i] == '\n' || buf[i] == '\r')) {
            ++i;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "timestamp") {
            ts = _atoi64(v.c_str());
        } else if (!k.empty()) {
            kvs.emplace_back(k, v);
        }
    }
    const long long now_sec = static_cast<long long>(std::time(nullptr));
    if (ts <= 0 || now_sec - ts > 180 || ts - now_sec > 60) {
        return;
    }
    static const char* const kAllowedKeys[] = {
        "SCCT_ENABLED", "SCCT_TRANSPORT", "SCCT_RELAY", "SCCT_LOCAL_IP",
        "SCCT_NAME", "SCCT_FFIRE", "SCCT_VOICE", "SCCT_MIC_DEVICE",
        "SCCT_MAX_FPS", "SCCT_LAUNCHER_API", "SCCT_INGAME_TOKEN",
        "SCCT_SHOW", "SCCT_LOG",
    };
    for (const auto& kv : kvs) {
        bool allowed = false;
        for (const char* ak : kAllowedKeys) {
            if (kv.first == ak) {
                allowed = true;
                break;
            }
        }
        if (!allowed) continue;
        int wlen_k = MultiByteToWideChar(CP_UTF8, 0, kv.first.c_str(), -1, nullptr, 0);
        int wlen_v = MultiByteToWideChar(CP_UTF8, 0, kv.second.c_str(), -1, nullptr, 0);
        if (wlen_k > 0 && wlen_v > 0) {
            std::wstring wk(static_cast<size_t>(wlen_k), L'\0');
            std::wstring wv(static_cast<size_t>(wlen_v), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, kv.first.c_str(), -1, &wk[0], wlen_k);
            MultiByteToWideChar(CP_UTF8, 0, kv.second.c_str(), -1, &wv[0], wlen_v);
            wchar_t dummy[8]{};
            if (GetEnvironmentVariableW(wk.c_str(), dummy, _countof(dummy)) == 0) {
                SetEnvironmentVariableW(wk.c_str(), wv.c_str());
            }
        }
    }
}

}  // namespace

extern "C" __declspec(dllexport) int ScctCoopNet_Install() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) {
        return 5;
    }

    LoadLaunchSessionIfNeeded();

    const std::wstring exe = ModuleFile(nullptr);
    const std::wstring dll_dir = DirectoryOf(ModuleFile(g_module));
    wchar_t env_log[MAX_PATH]{};
    const DWORD env_log_len = GetEnvironmentVariableW(L"SCCT_LOG", env_log, MAX_PATH);
    std::wstring log_file_name = L"scct-coop-net.log";
    if (env_log_len > 0 && env_log_len < MAX_PATH && std::wcschr(env_log, L'\\') == nullptr &&
        std::wcschr(env_log, L'/') == nullptr && std::wcschr(env_log, L':') == nullptr) {
        log_file_name = env_log;
    }
    bool log_ok = OpenLog(dll_dir + L"\\" + log_file_name);
    if (!log_ok) {
        wchar_t local_app[MAX_PATH]{};
        const DWORD la_len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
        if (la_len > 0 && la_len < MAX_PATH) {
            const std::wstring dir1 = std::wstring(local_app) + L"\\SCCTCoopLauncher";
            const std::wstring dir2 = dir1 + L"\\data";
            CreateDirectoryW(dir1.c_str(), nullptr);
            CreateDirectoryW(dir2.c_str(), nullptr);
            log_ok = OpenLog(dir2 + L"\\" + log_file_name);
        }
    }
    if (!log_ok) {
        wchar_t tmp_dir[MAX_PATH]{};
        const DWORD td_len = GetTempPathW(MAX_PATH, tmp_dir);
        if (td_len > 0 && td_len < MAX_PATH) {
            log_ok = OpenLog(std::wstring(tmp_dir) + log_file_name);
        }
    }
    char heure[40];
    NowText(heure, sizeof(heure));

    const DWORD shot_len = GetEnvironmentVariableW(L"SCCT_SHOT_DIR", g_shot_dir, MAX_PATH);
    if (shot_len == 0 || shot_len >= MAX_PATH) {
        g_shot_dir[0] = L'\0';
    }

    char env_local_ip[64]{};
    const DWORD env_ip_len = GetEnvironmentVariableA("SCCT_LOCAL_IP", env_local_ip, sizeof(env_local_ip));
    if (env_ip_len > 0 && env_ip_len < sizeof(env_local_ip)) {
        in_addr ia{};
        if (inet_pton(AF_INET, env_local_ip, &ia) != 1 || ia.s_addr == 0) {
            Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=SCCT_LOCAL_IP invalide (%s)",
                 heure, GetCurrentProcessId(), env_local_ip);
            return 3;
        }
        g_local_ip_be = ia.s_addr;
    }

    uint8_t md5[16]{};
    unsigned long long size = 0;
    const bool hashed = FileMd5(exe, md5, &size);
    char md5_text[33] = "indisponible";
    if (hashed) {
        Md5Text(md5, md5_text);
    }
    const bool name_ok = EqualsIgnoreCase(FileNameOf(exe), L"splintercell3.exe");
    const bool is_retail = hashed && size == kExpectedSizeRetail && std::memcmp(md5, kExpectedMd5Retail, 16) == 0;
    const bool is_ubi = hashed && size == kExpectedSizeUbi && std::memcmp(md5, kExpectedMd5Ubi, 16) == 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const bool base_ok = base == kExpectedBase;
    const bool is_v105_compat = base_ok && IsCompatibleV105Image(base);
    const bool size_ok = hashed && (size == kExpectedSizeRetail || size == kExpectedSizeUbi || is_v105_compat);
    const bool md5_ok = is_retail || is_ubi || is_v105_compat;
    const bool signature_ok = name_ok && md5_ok && base_ok;
    g_signature_ok = signature_ok;

    wchar_t env_name[32]{};
    const DWORD env_name_len = GetEnvironmentVariableW(L"SCCT_NAME", env_name, 32);
    if (env_name_len > 0) {
        const size_t copy_len = env_name_len < 15 ? env_name_len : 15;
        std::wmemcpy(g_custom_name, env_name, copy_len);
        g_custom_name[copy_len] = L'\0';
    }
    char env_ffire[16]{};
    if (GetEnvironmentVariableA("SCCT_FFIRE", env_ffire, sizeof(env_ffire)) > 0) {
        g_default_ffire = (env_ffire[0] == '1' || env_ffire[0] == 't' || env_ffire[0] == 'T' || env_ffire[0] == 'y' ||
                           env_ffire[0] == 'Y')
                              ? 1
                              : 0;
    }
    char env_voice[16]{};
    if (GetEnvironmentVariableA("SCCT_VOICE", env_voice, sizeof(env_voice)) > 0) {
        g_enable_voice = (env_voice[0] == '0' || env_voice[0] == 'f' || env_voice[0] == 'F' || env_voice[0] == 'n' ||
                          env_voice[0] == 'N')
                             ? 0
                             : 1;
    }
    char env_vlevel[16]{};
    if (GetEnvironmentVariableA("SCCT_VOICE_TEST_LEVEL", env_vlevel, sizeof(env_vlevel)) > 0) {
        const int v = std::atoi(env_vlevel);
        g_voice_test_level = (v > 100) ? 100 : (v < 0 ? 0 : v);
    }
    char env_mic[16]{};
    if (GetEnvironmentVariableA("SCCT_MIC_DEVICE", env_mic, sizeof(env_mic)) > 0) {
        InterlockedExchange(&g_mic_device_id, std::atoi(env_mic));
    }
    GetEnvironmentVariableW(L"SCCT_LAUNCHER_API", g_launcher_api_url, _countof(g_launcher_api_url));
    GetEnvironmentVariableW(L"SCCT_INGAME_TOKEN", g_ingame_api_token, _countof(g_ingame_api_token));
    StartFriendsSyncThread();

    NetConfig config = ReadConfig(dll_dir + L"\\scct-coop-net.ini");

    char env_flag[16]{};
    if (GetEnvironmentVariableA("SCCT_ENABLED", env_flag, sizeof(env_flag)) == 1 &&
        (env_flag[0] == '0' || env_flag[0] == '1')) {
        config.enabled = env_flag[0] == '1' ? 1 : 0;
    }
    if (GetEnvironmentVariableA("SCCT_TRANSPORT", env_flag, sizeof(env_flag)) == 1 &&
        (env_flag[0] == '0' || env_flag[0] == '1')) {
        config.transport = env_flag[0] == '1' ? 1 : 0;
    }
    char env_relay[256]{};
    const DWORD env_relay_len = GetEnvironmentVariableA("SCCT_RELAY", env_relay, sizeof(env_relay));
    if (env_relay_len > 0 && env_relay_len < sizeof(config.relay)) {
        std::memcpy(config.relay, env_relay, env_relay_len + 1);
    }

    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    const std::string d3d9_path = d3d9 ? Narrow(ModuleFile(d3d9)) : std::string("-");
    const int widescreen_loaded = GetModuleHandleW(L"SplinterCellChaosTheory.WidescreenFix.asi") != nullptr ? 1 : 0;

    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=%llu hex=%s seq=0 tronque=0 "
         "note=md5_ok=%d taille_ok=%d base_ok=%d nom_ok=%d base=0x%08lX enabled=%d transport=%d local_ip=%s d3d9=%s widescreen=%d",
         heure, GetCurrentProcessId(), size, md5_text, md5_ok ? 1 : 0, size_ok ? 1 : 0, base_ok ? 1 : 0,
         name_ok ? 1 : 0, static_cast<unsigned long>(base), config.enabled, config.transport,
         env_ip_len > 0 ? env_local_ip : "-", d3d9_path.c_str(), widescreen_loaded);

    if (!signature_ok) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex=%s seq=0 tronque=0 "
             "note=DLL chargee, signature differente des exe 1.05 attendus (base 0x10900000), aucun hook",
             heure, GetCurrentProcessId(), md5_text);
        return 2;
    }

    char relay_host[256]{};
    unsigned short relay_port = 0;
    bool want_transport = config.transport == 1;
    if (want_transport && !ParseRelay(config.relay, relay_host, sizeof(relay_host), &relay_port)) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=transport=1 sans adresse relay valable, transport coupe",
             heure, GetCurrentProcessId());
        want_transport = false;
    }
    const bool want_journal = config.enabled == 1;
    if (!want_journal && !want_transport) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=DLL chargee, enabled=0 transport=0, aucun hook",
             heure, GetCurrentProcessId());
        return 1;
    }
    g_journal = want_journal;
    if (!InstallHooks()) {
        g_journal = false;
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=DLL chargee, aucun hook",
             heure, GetCurrentProcessId());
        return 3;
    }
    Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
         "note=DLL chargee, hooks Winsock poses: bind sendto recvfrom WSASendTo WSARecvFrom WSAGetOverlappedResult",
         heure, GetCurrentProcessId());
    if (g_signature_ok) {
        InstallFeatureHooks();
    }
    if (want_transport &&
        !TransportStart(relay_host, relay_port, g_sendto, g_recvfrom, &TransportSink, g_local_ip_be)) {
        want_transport = false;
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=transport=1 demarrage impossible, copies non faites",
             heure, GetCurrentProcessId());
        if (!want_journal) {
            ShutdownHooks();
            Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 note=enabled=0, aucun hook",
                 heure, GetCurrentProcessId());
            return 3;
        }
    }
    wchar_t show_window[8]{};
    const bool leave_visible = GetEnvironmentVariableW(L"SCCT_SHOW", show_window, 8) == 1 && show_window[0] == L'1';
    if (want_journal && leave_visible) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=gardes focus non posees, fenetre laissee visible",
             heure, GetCurrentProcessId());
    } else if (want_journal) {
        InstallFocusGuards();
    }
    if (want_journal && !want_transport) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=enabled=1, hooks Winsock en journal seulement, gardes focus actives, plafond hex %d octets, "
             "decouverte port %u jusqu'a %d octets, plafond %ld lignes, transport=0",
             heure, GetCurrentProcessId(), kHexBytes, kDiscoveryPort, kDiscoveryHex, kMaxLines);
    } else if (want_journal && want_transport) {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=enabled=1 transport=1 relay=%s:%u, journal sans reecriture des adresses LAN, "
             "copie 9100/9103 telle quelle",
             heure, GetCurrentProcessId(), relay_host, relay_port);
    } else {
        Logf("heure=%s pid=%lu dir=etat local=- pair=- len=0 hex= seq=0 tronque=0 "
             "note=enabled=0 transport=1 relay=%s:%u, copie 9100/9103 telle quelle, port du socket du jeu inchange",
             heure, GetCurrentProcessId(), relay_host, relay_port);
    }
    return 0;
}

extern "C" __declspec(dllexport) int ScctCoopNet_AttachRelay(const char* host, unsigned short port) {
    if (host == nullptr || host[0] == '\0' || port == 0 || port == 65535) {
        return 2;
    }
    g_journal = false;
    if (!g_hooks && !InstallHooks()) {
        return 3;
    }
    if (!TransportStart(host, port, g_sendto, g_recvfrom, &TransportSink)) {
        return 4;
    }
    return 0;
}

extern "C" void ScctCoopNet_Stop() {
    TransportStop();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (s_time_period_active) {
            timeEndPeriod(1);
            s_time_period_active = false;
        }
        TransportShutdown();
    }
    return TRUE;
}
