// lin_dumper.cpp
// Injected x64 DLL for SCDA.exe (recompiled Splinter Cell: Double Agent Xbox)
// Hooks FArchiveLinear and ULinkerLoad::Preload to record the exact
// (Linker, ExportIndex, StreamOffset, RawBytes) mapping for every serialized
// object in a .lin linearized archive.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

struct alignas(64) X86Context {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t cf, zf, sf, of, pf;
    uint32_t df;
    uint32_t xflags;
    uint32_t fs_base;
    uint32_t top;
    uint16_t fcw, fsw;
    uint32_t mxcsr;
    double st[8];
    uint64_t mm[8];
    uint8_t xmm[8][16];
    uint32_t thread_handle;
    uint32_t last_icall;
    void* host;
};

using X86Func = void(*)(X86Context&, uint8_t*);

// RVAs inside SCDA.exe (ImageBase = 0x140000000)
static constexpr uintptr_t RVA_FArchiveLinear_ctor = 0x2064C0; // 15 bytes
static constexpr uintptr_t RVA_FArchiveLinear_ser  = 0x2069B0; // 15 bytes
static constexpr uintptr_t RVA_FArchiveLinear_dtor = 0x206AE0; // 15 bytes
static constexpr uintptr_t RVA_WinMain             = 0x21BC40; // 14 bytes
static constexpr uintptr_t RVA_UObject_EndLoad     = 0x2A1B20; // 14 bytes
static constexpr uintptr_t RVA_appErrorf           = 0x2D2900; // 14 bytes
static constexpr uintptr_t RVA_ULinkerLoad_Preload = 0x315C60; // 16 bytes
static constexpr uintptr_t RVA_ULinkerLoad_ctor    = 0x31B730; // 14 bytes
static constexpr uintptr_t RVA_SetCurrentMap       = 0x839D40; // 16 bytes

// Guest global addresses in SCDA.exe guest memory (base + VA)
static constexpr uint32_t GUEST_MapNameBuf = 0x0062A9F0; // wchar_t[32] filled by FArchiveLinear ctor
static constexpr uint32_t GUEST_CmdLineBuf = 0x005E3E30; // wchar_t[] CmdLine ("MAPNAME=...")

static X86Func g_orig_ar_ctor   = nullptr;
static X86Func g_orig_ar_ser    = nullptr;
static X86Func g_orig_ar_dtor   = nullptr;
static X86Func g_orig_winmain   = nullptr;
static X86Func g_orig_end_load  = nullptr;
static X86Func g_orig_errorf    = nullptr;
static X86Func g_orig_preload   = nullptr;
static X86Func g_orig_lnk_ctor  = nullptr;
static X86Func g_orig_set_map   = nullptr;

static std::mutex g_mtx;
static FILE* g_evt_file = nullptr;
static FILE* g_log_file = nullptr;
static wchar_t g_target_map[128] = {};
static char g_out_dir[MAX_PATH] = {};

static std::atomic<uint32_t> g_target_ar_va{0};
static std::atomic<uint32_t> g_common_ar_va{0};
static std::atomic<uint64_t> g_start_tick{0};
static std::atomic<uint64_t> g_last_any_tick{0};
static std::atomic<uint64_t> g_common_bytes{0};
static std::atomic<uint64_t> g_target_bytes{0};
static std::atomic<uint32_t> g_target_tell{0};
static std::atomic<uint32_t> g_linker_count{0};
static std::atomic<bool> g_flushed{false};
static uint8_t* g_guest_base = nullptr;

struct PendingRead {
    bool active = false;
    uint32_t ar_va = 0;
    uint32_t tell_before = 0;
    uint32_t active_lnk = 0;
    int32_t active_exp = -1;
    std::vector<uint8_t> data;
};
static PendingRead g_pending;

static void FlushPendingLocked() {
    if (!g_pending.active || g_pending.data.empty()) {
        g_pending.active = false;
        g_pending.data.clear();
        return;
    }
    if (g_evt_file) {
#pragma pack(push, 1)
        struct SHdr {
            uint8_t rec;
            uint32_t ar_va;
            uint32_t tell_before;
            uint32_t length;
            uint32_t active_lnk;
            int32_t active_exp;
        };
#pragma pack(pop)
        SHdr hdr;
        hdr.rec = 'S';
        hdr.ar_va = g_pending.ar_va;
        hdr.tell_before = g_pending.tell_before;
        hdr.length = static_cast<uint32_t>(g_pending.data.size());
        hdr.active_lnk = g_pending.active_lnk;
        hdr.active_exp = g_pending.active_exp;
        fwrite(&hdr, sizeof(hdr), 1, g_evt_file);
        fwrite(g_pending.data.data(), 1, g_pending.data.size(), g_evt_file);
    }
    g_pending.active = false;
    g_pending.data.clear();
}

// Binary record types written to dump_events.bin:
// 'L' (1B) + linker_va(4) + name_cnt(4) + exp_cnt(4) + imp_cnt(4) + lic_ver(4) + fn_len(4) + fn_utf8[fn_len]
// 'P' (1B) + enter(1) + linker_va(4) + export_idx(4) + obj_va(4) + obj_flags(4) + tell(4)
// 'S' (1B) + ar_va(4) + tell_before(4) + length(4) + active_linker(4) + active_export(4) + data[length]
// 'A' (1B) + ar_va(4) + pool0(4) + pool1(4) + pool2(4) + pool3(4) + pool4(4) + map_len(4) + map_utf8[map_len]

struct PreloadFrame {
    uint32_t linker_va;
    int32_t export_idx;
    uint32_t obj_va;
};
static thread_local std::vector<PreloadFrame> t_stack;

static inline uint32_t R32(uint8_t* base, uint32_t va) {
    return *reinterpret_cast<const uint32_t*>(base + va);
}
static inline uint16_t R16(uint8_t* base, uint32_t va) {
    return *reinterpret_cast<const uint16_t*>(base + va);
}

static std::string ReadGuestFStringUtf8(uint8_t* base, uint32_t fstr_va) {
    uint32_t data_va = R32(base, fstr_va);
    int32_t num = static_cast<int32_t>(R32(base, fstr_va + 4));
    if (!data_va || num <= 1) return "";
    if (num > 512) num = 512;
    std::string out;
    out.reserve(num);
    for (int32_t i = 0; i < num - 1; i++) {
        uint16_t ch = R16(base, data_va + i * 2);
        if (!ch) break;
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

static std::string ReadGuestWStrUtf8(uint8_t* base, uint32_t va, int max_chars = 128) {
    std::string out;
    for (int i = 0; i < max_chars; i++) {
        uint16_t ch = R16(base, va + i * 2);
        if (!ch) break;
        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return out;
}

static void LogPrintf(const char* fmt, ...) {
    if (!g_log_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_file, fmt, ap);
    va_end(ap);
    fflush(g_log_file);
}

static void FlushAndExit(const char* reason) {
    if (g_flushed.exchange(true)) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    FlushPendingLocked();
    LogPrintf("[DONE] FlushAndExit(%s): common_bytes=%llu target_ar=0x%08X target_bytes=%llu last_tell=%u linkers=%u\n",
              reason,
              (unsigned long long)g_common_bytes.load(),
              g_target_ar_va.load(),
              (unsigned long long)g_target_bytes.load(),
              g_target_tell.load(),
              g_linker_count.load());
    if (g_evt_file) {
        fflush(g_evt_file);
        fclose(g_evt_file);
        g_evt_file = nullptr;
    }
    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = nullptr;
    }
    char done_path[MAX_PATH];
    snprintf(done_path, sizeof(done_path), "%s\\dump_complete.txt", g_out_dir);
    FILE* df = fopen(done_path, "wb");
    if (df) {
        fprintf(df, "reason=%s\ncommon_bytes=%llu\ntarget_bytes=%llu\ntell=%u\nlinkers=%u\n",
                reason,
                (unsigned long long)g_common_bytes.load(),
                (unsigned long long)g_target_bytes.load(),
                g_target_tell.load(),
                g_linker_count.load());
        fclose(df);
    }
    ExitProcess(0);
}

static void Hook_FArchiveLinear_ctor(X86Context& ctx, uint8_t* base) {
    g_guest_base = base;
    uint32_t ar_va = ctx.ecx;
    g_orig_ar_ctor(ctx, base);

    uint32_t p0 = R32(base, ar_va + 0x48);
    uint32_t p1 = R32(base, ar_va + 0x4C);
    uint32_t p2 = R32(base, ar_va + 0x50);
    uint32_t p3 = R32(base, ar_va + 0x54);
    uint32_t p4 = R32(base, ar_va + 0x58);
    std::string mname = ReadGuestWStrUtf8(base, GUEST_MapNameBuf, 64);

    std::lock_guard<std::mutex> lk(g_mtx);
    FlushPendingLocked();
    LogPrintf("[AR_OPEN] ar=0x%08X map='%s' pools=(%u, %u, %u, %u, %u)\n",
              ar_va, mname.c_str(), p0, p1, p2, p3, p4);

    if (g_evt_file) {
        uint8_t rec = 'A';
        uint32_t mlen = static_cast<uint32_t>(mname.size());
        fwrite(&rec, 1, 1, g_evt_file);
        fwrite(&ar_va, 4, 1, g_evt_file);
        fwrite(&p0, 4, 1, g_evt_file);
        fwrite(&p1, 4, 1, g_evt_file);
        fwrite(&p2, 4, 1, g_evt_file);
        fwrite(&p3, 4, 1, g_evt_file);
        fwrite(&p4, 4, 1, g_evt_file);
        fwrite(&mlen, 4, 1, g_evt_file);
        if (mlen) fwrite(mname.data(), 1, mlen, g_evt_file);
    }

    if (mname.empty() || _stricmp(mname.c_str(), "common") == 0) {
        g_common_ar_va.store(ar_va);
    } else {
        g_target_ar_va.store(ar_va);
    }
    g_last_any_tick.store(GetTickCount64());
}

static void Hook_FArchiveLinear_ser(X86Context& ctx, uint8_t* base) {
    uint32_t ar_va = ctx.ecx;
    uint32_t buf_va = R32(base, ctx.esp + 4);
    uint32_t length = R32(base, ctx.esp + 8);
    uint32_t tell_before = R32(base, ar_va + 0x44);

    g_orig_ar_ser(ctx, base);

    if (length == 0) return;
    if (ar_va == g_target_ar_va.load()) {
        g_target_tell.store(R32(base, ar_va + 0x44));
        g_target_bytes.fetch_add(length);
    } else if (ar_va == g_common_ar_va.load()) {
        g_common_bytes.fetch_add(length);
    }
    g_last_any_tick.store(GetTickCount64());

    uint32_t active_lnk = 0;
    int32_t active_exp = -1;
    if (!t_stack.empty()) {
        active_lnk = t_stack.back().linker_va;
        active_exp = t_stack.back().export_idx;
    }

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_pending.active || g_pending.ar_va != ar_va ||
        g_pending.active_lnk != active_lnk || g_pending.active_exp != active_exp ||
        g_pending.data.size() + length > (4u << 20)) {
        FlushPendingLocked();
        g_pending.active = true;
        g_pending.ar_va = ar_va;
        g_pending.tell_before = tell_before;
        g_pending.active_lnk = active_lnk;
        g_pending.active_exp = active_exp;
    }
    const uint8_t* src = base + buf_va;
    g_pending.data.insert(g_pending.data.end(), src, src + length);
}

static void Hook_FArchiveLinear_dtor(X86Context& ctx, uint8_t* base) {
    uint32_t ar_va = ctx.ecx;
    uint32_t tell = R32(base, ar_va + 0x44);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        FlushPendingLocked();
        LogPrintf("[AR_CLOSE] ar=0x%08X tell=%u target_bytes=%llu\n",
                  ar_va, tell, (unsigned long long)g_target_bytes.load());
    }
    g_orig_ar_dtor(ctx, base);
    if (ar_va == g_target_ar_va.load() && g_target_bytes.load() > 100000) {
        FlushAndExit("FArchiveLinear_dtor");
    }
}

static std::string ReadGuestFName(uint8_t* base, uint32_t fn_idx) {
    uint32_t names_obj = R32(base, 0x0062A4C8);
    if (!names_obj || names_obj >= 0x40000000u) return "";
    uint32_t names_arr = R32(base, names_obj);
    if (!names_arr || names_arr >= 0x40000000u || fn_idx >= 500000u) return "";
    uint32_t entry = R32(base, names_arr + fn_idx * 4);
    if (!entry || entry >= 0x40000000u) return "";
    std::string out;
    for (int i = 0; i < 128; i++) {
        uint8_t c = base[entry + 0xC + i];
        if (!c) break;
        out.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    }
    return out;
}

static void Hook_ULinkerLoad_ctor(X86Context& ctx, uint8_t* base) {
    uint32_t linker_va = ctx.ecx;
    uint32_t root_arg  = R32(base, ctx.esp + 4);
    uint32_t fn_va     = R32(base, ctx.esp + 8);
    std::string arg_fn = fn_va ? ReadGuestWStrUtf8(base, fn_va, 256) : "";

    {
        uint32_t com_enter = static_cast<uint32_t>(g_common_bytes.load());
        uint32_t tgt_enter = static_cast<uint32_t>(g_target_bytes.load());
        std::lock_guard<std::mutex> lk(g_mtx);
        FlushPendingLocked();
        if (g_evt_file) {
#pragma pack(push, 1)
            struct ERec { uint8_t rec; uint32_t linker_va, com_enter, tgt_enter; };
#pragma pack(pop)
            ERec er = {'E', linker_va, com_enter, tgt_enter};
            fwrite(&er, sizeof(er), 1, g_evt_file);
        }
    }

    g_orig_lnk_ctor(ctx, base);

    uint32_t root_va  = R32(base, linker_va + 0x2C);
    if (!root_va) root_va = root_arg;
    std::string pkg_name = (root_va && root_va < 0x40000000u)
        ? ReadGuestFName(base, R32(base, root_va + 0x20)) : "";

    uint32_t name_cnt = R32(base, linker_va + 0x40);
    uint32_t exp_cnt  = R32(base, linker_va + 0x48);
    uint32_t imp_cnt  = R32(base, linker_va + 0x50);
    uint32_t lic_ver  = R16(base, linker_va + 0x36); // PackageSummary LicenseeVersion
    std::string lnk_fn = ReadGuestFStringUtf8(base, linker_va + 0x9C);
    if (lnk_fn.empty()) lnk_fn = arg_fn;

    g_linker_count.fetch_add(1);
    g_last_any_tick.store(GetTickCount64());

    std::lock_guard<std::mutex> lk(g_mtx);
    FlushPendingLocked();
    LogPrintf("[LINKER] lnk=0x%08X pkg=%s fn=%s N=%u E=%u I=%u lic=%u\n",
              linker_va, pkg_name.c_str(), lnk_fn.c_str(), name_cnt, exp_cnt, imp_cnt, lic_ver);
    if (g_evt_file) {
        uint8_t rec = 'L';
        uint32_t flen = static_cast<uint32_t>(lnk_fn.size());
        fwrite(&rec, 1, 1, g_evt_file);
        fwrite(&linker_va, 4, 1, g_evt_file);
        fwrite(&name_cnt, 4, 1, g_evt_file);
        fwrite(&exp_cnt, 4, 1, g_evt_file);
        fwrite(&imp_cnt, 4, 1, g_evt_file);
        fwrite(&lic_ver, 4, 1, g_evt_file);
        fwrite(&flen, 4, 1, g_evt_file);
        if (flen) fwrite(lnk_fn.data(), 1, flen, g_evt_file);
        uint32_t plen = static_cast<uint32_t>(pkg_name.size());
        uint8_t rrec = 'N';
        fwrite(&rrec, 1, 1, g_evt_file);
        fwrite(&linker_va, 4, 1, g_evt_file);
        fwrite(&plen, 4, 1, g_evt_file);
        if (plen) fwrite(pkg_name.data(), 1, plen, g_evt_file);
    }
}

static void Hook_ULinkerLoad_Preload(X86Context& ctx, uint8_t* base) {
    uint32_t linker_va = ctx.ecx - 0xAC;
    uint32_t obj_va = R32(base, ctx.esp + 4);
    uint32_t obj_linker = R32(base, obj_va + 0x10);
    int32_t  export_idx = static_cast<int32_t>(R32(base, obj_va + 0x14));
    uint32_t obj_flags  = R32(base, obj_va + 0x1C);
    bool will_load = (obj_linker == linker_va) && ((obj_flags & 0x20000u) != 0);

    if (will_load) {
        t_stack.push_back({linker_va, export_idx, obj_va});
        g_last_any_tick.store(GetTickCount64());

        uint32_t ar_va = g_target_ar_va.load();
        uint32_t tell = ar_va ? R32(base, ar_va + 0x44) : 0;
        std::lock_guard<std::mutex> lk(g_mtx);
        FlushPendingLocked();
        if (g_evt_file) {
#pragma pack(push, 1)
            struct PRec {
                uint8_t rec, enter;
                uint32_t linker_va;
                int32_t export_idx;
                uint32_t obj_va, obj_flags, tell;
            };
#pragma pack(pop)
            PRec pr = {'P', 1, linker_va, export_idx, obj_va, obj_flags, tell};
            fwrite(&pr, sizeof(pr), 1, g_evt_file);
        }
    }

    g_orig_preload(ctx, base);

    if (will_load) {
        g_last_any_tick.store(GetTickCount64());
        uint32_t ar_va = g_target_ar_va.load();
        uint32_t tell = ar_va ? R32(base, ar_va + 0x44) : 0;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            FlushPendingLocked();
            if (g_evt_file) {
#pragma pack(push, 1)
                struct PRec {
                    uint8_t rec, enter;
                    uint32_t linker_va;
                    int32_t export_idx;
                    uint32_t obj_va, obj_flags, tell;
                };
#pragma pack(pop)
                PRec pr = {'P', 0, linker_va, export_idx, obj_va, obj_flags, tell};
                fwrite(&pr, sizeof(pr), 1, g_evt_file);
            }
        }
        t_stack.pop_back();
    }
}

static void Hook_WinMain(X86Context& ctx, uint8_t* base) {
    g_guest_base = base;
    if (g_target_map[0]) {
        wchar_t buf[256];
        swprintf(buf, 256, L"MAPNAME=%ls", g_target_map);
        uint16_t* dst = reinterpret_cast<uint16_t*>(base + GUEST_CmdLineBuf);
        size_t n = wcslen(buf);
        for (size_t i = 0; i <= n; i++) dst[i] = static_cast<uint16_t>(buf[i]);
        std::lock_guard<std::mutex> lk(g_mtx);
        LogPrintf("[WINMAIN] Forced CmdLine='MAPNAME=%ls'\n", g_target_map);
    }
    g_orig_winmain(ctx, base);
}

static void Hook_SetCurrentMap(X86Context& ctx, uint8_t* base) {
    uint32_t gameflow_va = ctx.ecx;
    uint32_t arg_va = R32(base, ctx.esp + 4);
    std::string arg_s = arg_va ? ReadGuestWStrUtf8(base, arg_va, 128) : "<null>";
    g_orig_set_map(ctx, base);
    uint32_t ret = ctx.eax;
    std::lock_guard<std::mutex> lk(g_mtx);
    LogPrintf("[SET_MAP] arg='%s' -> ret=%u (num_maps=%u)\n",
              arg_s.c_str(), ret, R32(base, gameflow_va + 0x68));
    if (ret == 0) {
        uint32_t cnt = R32(base, gameflow_va + 0x68);
        uint32_t arr = R32(base, gameflow_va + 0x64);
        for (uint32_t i = 0; i < cnt && i < 64; i++) {
            uint32_t item = R32(base, arr + i * 4);
            std::string nm = ReadGuestFStringUtf8(base, item + 4);
            LogPrintf("  map[%u]='%s'\n", i, nm.c_str());
        }
    }
}

static void Hook_appErrorf(X86Context& ctx, uint8_t* base) {
    uint32_t fmt_va = R32(base, ctx.esp + 4);
    std::string fmt = fmt_va ? ReadGuestWStrUtf8(base, fmt_va, 256) : "<null>";
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        LogPrintf("[APP_ERRORF] fmt='%s'\n", fmt.c_str());
        if (g_log_file) fflush(g_log_file);
    }
    g_orig_errorf(ctx, base);
}

static void Hook_UObject_EndLoad(X86Context& ctx, uint8_t* base) {
    g_orig_end_load(ctx, base);
    if (g_target_bytes.load() > 0 && t_stack.empty()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        FlushPendingLocked();
        LogPrintf("[ENDLOAD] common=%llu target=%llu linkers=%u tell=%u\n",
                  (unsigned long long)g_common_bytes.load(),
                  (unsigned long long)g_target_bytes.load(),
                  g_linker_count.load(),
                  g_target_ar_va.load() ? R32(base, g_target_ar_va.load() + 0x44) : 0u);
        if (g_evt_file) fflush(g_evt_file);
    }
}

static bool InstallDetour(uintptr_t target_addr, size_t steal_len, void* hook_fn, X86Func* out_orig) {
    uint8_t* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;

    memcpy(tramp, reinterpret_cast<void*>(target_addr), steal_len);
    uintptr_t ret_addr = target_addr + steal_len;
    tramp[steal_len + 0] = 0xFF;
    tramp[steal_len + 1] = 0x25;
    *reinterpret_cast<uint32_t*>(tramp + steal_len + 2) = 0;
    *reinterpret_cast<uint64_t*>(tramp + steal_len + 6) = ret_addr;

    *out_orig = reinterpret_cast<X86Func>(tramp);

    DWORD old_prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target_addr), steal_len, PAGE_EXECUTE_READWRITE, &old_prot))
        return false;

    uint8_t* dst = reinterpret_cast<uint8_t*>(target_addr);
    dst[0] = 0xFF;
    dst[1] = 0x25;
    *reinterpret_cast<uint32_t*>(dst + 2) = 0;
    *reinterpret_cast<uint64_t*>(dst + 6) = reinterpret_cast<uint64_t>(hook_fn);
    for (size_t i = 14; i < steal_len; i++) dst[i] = 0x90;

    VirtualProtect(reinterpret_cast<void*>(target_addr), steal_len, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target_addr), steal_len);
    return true;
}

static DWORD WINAPI WatchdogThread(LPVOID) {
    uint64_t last_progress_log = GetTickCount64();
    for (;;) {
        Sleep(250);
        uint64_t now = GetTickCount64();
        uint64_t last_act = g_last_any_tick.load();
        uint64_t idle_ms = last_act ? (now - last_act) : 0;
        uint64_t tb = g_target_bytes.load();
        if (now - last_progress_log >= 5000) {
            last_progress_log = now;
            std::lock_guard<std::mutex> lk(g_mtx);
            LogPrintf("[PROGRESS] common_bytes=%llu target_bytes=%llu linkers=%u idle_ms=%llu\n",
                      (unsigned long long)g_common_bytes.load(),
                      (unsigned long long)tb,
                      g_linker_count.load(),
                      (unsigned long long)idle_ms);
        }
        if (tb > 1000000 && last_act != 0 && idle_ms >= 4000) {
            FlushAndExit("idle_after_target_4000ms");
        }
        uint64_t started = g_start_tick.load();
        if (started != 0 && (now - started) >= 105000) {
            FlushAndExit("safety_timeout");
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);

        uint64_t now = GetTickCount64();
        g_start_tick.store(now);
        g_last_any_tick.store(now);

        GetEnvironmentVariableA("SCDA_DUMP_OUT", g_out_dir, MAX_PATH);
        if (!g_out_dir[0]) strcpy_s(g_out_dir, ".");
        GetEnvironmentVariableW(L"SCDA_DUMP_MAP", g_target_map, 128);

        char log_path[MAX_PATH], evt_path[MAX_PATH];
        snprintf(log_path, sizeof(log_path), "%s\\dump.log", g_out_dir);
        snprintf(evt_path, sizeof(evt_path), "%s\\dump_events.bin", g_out_dir);
        g_log_file = fopen(log_path, "wb");
        g_evt_file = fopen(evt_path, "wb");
        if (g_evt_file) setvbuf(g_evt_file, nullptr, _IOFBF, 4 << 20);

        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        bool ok = true;
        ok &= InstallDetour(base + RVA_FArchiveLinear_ctor, 15, reinterpret_cast<void*>(&Hook_FArchiveLinear_ctor), &g_orig_ar_ctor);
        ok &= InstallDetour(base + RVA_FArchiveLinear_ser,  15, reinterpret_cast<void*>(&Hook_FArchiveLinear_ser),  &g_orig_ar_ser);
        ok &= InstallDetour(base + RVA_FArchiveLinear_dtor, 15, reinterpret_cast<void*>(&Hook_FArchiveLinear_dtor), &g_orig_ar_dtor);
        ok &= InstallDetour(base + RVA_WinMain,             14, reinterpret_cast<void*>(&Hook_WinMain),             &g_orig_winmain);
        ok &= InstallDetour(base + RVA_UObject_EndLoad,     14, reinterpret_cast<void*>(&Hook_UObject_EndLoad),     &g_orig_end_load);
        ok &= InstallDetour(base + RVA_appErrorf,           14, reinterpret_cast<void*>(&Hook_appErrorf),           &g_orig_errorf);
        ok &= InstallDetour(base + RVA_ULinkerLoad_Preload, 16, reinterpret_cast<void*>(&Hook_ULinkerLoad_Preload), &g_orig_preload);
        ok &= InstallDetour(base + RVA_ULinkerLoad_ctor,    14, reinterpret_cast<void*>(&Hook_ULinkerLoad_ctor),    &g_orig_lnk_ctor);
        ok &= InstallDetour(base + RVA_SetCurrentMap,       16, reinterpret_cast<void*>(&Hook_SetCurrentMap),       &g_orig_set_map);
        LogPrintf("[INIT] modBase=0x%llX hooks_ok=%d out=%s\n", (unsigned long long)base, ok ? 1 : 0, g_out_dir);

        HANDLE th = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
