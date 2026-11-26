// Starts splintercell3.exe and loads ScctCoopNet.dll before the original entry runs.
// The entry bytes are restored by the stub before that entry executes.
// This launcher never attaches to a process it did not create.

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

struct RemoteData {
    uint32_t status;
    uint32_t protOut;
    uint8_t original[16];
    char name[32];
    wchar_t path[MAX_PATH];
};

constexpr uint32_t kDataOffset = 0x180;
constexpr uint32_t kOrigLen = 16;
bool g_hold = false;
bool g_visible = false;
DWORD g_child_pid = 0;

BOOL CALLBACK ParkEnum(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != static_cast<DWORD>(param)) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return TRUE;
    }
    if (rect.left <= -8000 && rect.top <= -8000) {
        return TRUE;
    }
    SetWindowPos(hwnd, HWND_NOTOPMOST, -32000, -32000, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
    return TRUE;
}

void WaitVisible(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return;
    }
    std::printf("fenetre visible pid=%lu\n", pid);
    std::fflush(stdout);
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
}

void WatchOffscreen(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return;
    }
    std::printf("surveillance=%lu\n", pid);
    std::fflush(stdout);
    while (WaitForSingleObject(process, 200) == WAIT_TIMEOUT) {
        EnumWindows(ParkEnum, static_cast<LPARAM>(pid));
    }
    CloseHandle(process);
}

std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
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

DWORD ForegroundPid() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid;
}

bool ReadMem(HANDLE process, uint32_t address, void* out, size_t bytes) {
    SIZE_T got = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), out, bytes, &got) &&
           got == bytes;
}

bool WriteMem(HANDLE process, uint32_t address, const void* data, size_t bytes) {
    SIZE_T wrote = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), data, bytes, &wrote) &&
           wrote == bytes;
}

uint32_t ProcAddress(const wchar_t* module, const char* name) {
    const HMODULE loaded = GetModuleHandleW(module);
    const auto addr = loaded ? GetProcAddress(loaded, name) : nullptr;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(addr));
}

class Asm {
public:
    std::vector<uint8_t> bytes;

    size_t size() const { return bytes.size(); }
    void u8(uint8_t value) { bytes.push_back(value); }
    void u32(uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }
    void mov_eax(uint32_t value) { u8(0xB8); u32(value); }
    void mov_edx(uint32_t value) { u8(0xBA); u32(value); }
    void push_imm(uint32_t value) { u8(0x68); u32(value); }
    void call_eax() { u8(0xFF); u8(0xD0); }
    void test_eax() { u8(0x85); u8(0xC0); }
    size_t jz_near() {
        u8(0x0F);
        u8(0x84);
        const size_t at = bytes.size();
        u32(0);
        return at;
    }
    size_t jmp_near() {
        u8(0xE9);
        const size_t at = bytes.size();
        u32(0);
        return at;
    }
    void patch_rel32(size_t rel_at, size_t target) {
        const int32_t rel = static_cast<int32_t>(target) - static_cast<int32_t>(rel_at + 4);
        bytes[rel_at] = static_cast<uint8_t>(rel);
        bytes[rel_at + 1] = static_cast<uint8_t>(rel >> 8);
        bytes[rel_at + 2] = static_cast<uint8_t>(rel >> 16);
        bytes[rel_at + 3] = static_cast<uint8_t>(rel >> 24);
    }
};

std::vector<uint8_t> BuildStub(uint32_t cave, uint32_t data, uint32_t entry, uint32_t old_protect) {
    const uint32_t status_addr = data + offsetof(RemoteData, status);
    const uint32_t prot_addr = data + offsetof(RemoteData, protOut);
    const uint32_t original_addr = data + offsetof(RemoteData, original);
    const uint32_t name_addr = data + offsetof(RemoteData, name);
    const uint32_t path_addr = data + offsetof(RemoteData, path);
    const uint32_t load_library = ProcAddress(L"kernel32.dll", "LoadLibraryW");
    const uint32_t get_proc = ProcAddress(L"kernel32.dll", "GetProcAddress");
    const uint32_t virtual_protect = ProcAddress(L"kernel32.dll", "VirtualProtect");
    const uint32_t flush_cache = ProcAddress(L"kernel32.dll", "FlushInstructionCache");

    Asm asm_code;
    asm_code.u8(0x60);  // pushad
    asm_code.u8(0x9C);  // pushfd
    asm_code.mov_eax(status_addr);
    asm_code.u8(0xC7);
    asm_code.u8(0x00);
    asm_code.u32(3);  // status = stub started

    asm_code.mov_eax(load_library);
    asm_code.push_imm(path_addr);
    asm_code.call_eax();
    asm_code.test_eax();
    const size_t jz_load = asm_code.jz_near();

    asm_code.push_imm(name_addr);
    asm_code.u8(0x50);  // push hmodule
    asm_code.mov_eax(get_proc);
    asm_code.call_eax();
    asm_code.test_eax();
    const size_t jz_proc = asm_code.jz_near();
    asm_code.call_eax();  // ScctCoopNet_Install(), cdecl, no arguments

    asm_code.push_imm(1);
    const size_t jmp_join = asm_code.jmp_near();

    const size_t fail = asm_code.size();
    asm_code.patch_rel32(jz_load, fail);
    asm_code.patch_rel32(jz_proc, fail);
    asm_code.push_imm(2);

    const size_t join = asm_code.size();
    asm_code.patch_rel32(jmp_join, join);
    asm_code.u8(0xFC);  // cld
    asm_code.u8(0xB9);
    asm_code.u32(kOrigLen);  // mov ecx, 16
    asm_code.u8(0xBE);
    asm_code.u32(original_addr);  // mov esi, original
    asm_code.u8(0xBF);
    asm_code.u32(entry);  // mov edi, entry
    asm_code.u8(0xF3);
    asm_code.u8(0xA4);  // rep movsb

    asm_code.push_imm(kOrigLen);
    asm_code.push_imm(entry);
    asm_code.push_imm(0xFFFFFFFFu);
    asm_code.mov_eax(flush_cache);
    asm_code.call_eax();

    asm_code.push_imm(prot_addr);
    asm_code.push_imm(old_protect);
    asm_code.push_imm(kOrigLen);
    asm_code.push_imm(entry);
    asm_code.mov_eax(virtual_protect);
    asm_code.call_eax();

    asm_code.u8(0x58);  // pop eax (1 = loaded, 2 = failed)
    asm_code.mov_edx(status_addr);
    asm_code.u8(0x89);
    asm_code.u8(0x02);  // mov [edx], eax
    asm_code.u8(0x9D);  // popfd
    asm_code.u8(0x61);  // popad

    const size_t jmp_at = asm_code.size();
    asm_code.u8(0xE9);
    const uint32_t remote_jmp = cave + static_cast<uint32_t>(jmp_at);
    const int32_t rel = static_cast<int32_t>(entry - (remote_jmp + 5));
    asm_code.u32(static_cast<uint32_t>(rel));
    return asm_code.bytes;
}

using NtQueryInformationProcessFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ProcessBasicInformation {
    PVOID reserved1;
    PVOID peb;
    PVOID reserved2[2];
    ULONG_PTR pid;
    PVOID reserved3;
};

bool ImageBase(HANDLE process, uint32_t* base) {
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) {
        return false;
    }
    ProcessBasicInformation info{};
    if (query(process, 0, &info, sizeof(info), nullptr) != 0 || !info.peb) {
        return false;
    }
    return ReadMem(process, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.peb)) + 8, base, sizeof(*base));
}

bool EntryPoint(HANDLE process, uint32_t base, uint32_t* entry) {
    uint32_t lfanew = 0;
    uint32_t signature = 0;
    uint16_t magic = 0;
    uint32_t rva = 0;
    if (!ReadMem(process, base + 0x3C, &lfanew, sizeof(lfanew)) ||
        !ReadMem(process, base + lfanew, &signature, sizeof(signature)) ||
        signature != 0x4550 ||
        !ReadMem(process, base + lfanew + 24, &magic, sizeof(magic)) ||
        magic != 0x10B ||
        !ReadMem(process, base + lfanew + 24 + 16, &rva, sizeof(rva))) {
        return false;
    }
    *entry = base + rva;
    return rva != 0;
}

int WaitForStatus(HANDLE process, uint32_t status_addr) {
    for (int i = 0; i < 1200; ++i) {
        DWORD exit_code = STILL_ACTIVE;
        GetExitCodeProcess(process, &exit_code);
        if (exit_code != STILL_ACTIVE) {
            std::fprintf(stderr, "processus termine code=%lu\n", exit_code);
            return 4;
        }
        uint32_t status = 0;
        if (ReadMem(process, status_addr, &status, sizeof(status)) && (status == 1 || status == 2)) {
            std::printf("statut_cave=%u\n", status);
            return status == 1 ? 0 : 3;
        }
        Sleep(50);
    }
    std::fprintf(stderr, "delai depasse avant le retour du stub\n");
    return 5;
}

int Launch(const std::wstring& exe, const std::wstring& dll) {
    std::printf("fg_avant=%lu\n", ForegroundPid());

    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> command_buf(command.begin(), command.end());
    command_buf.push_back(L'\0');
    const std::wstring work = DirectoryOf(exe);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = g_visible ? SW_SHOWNORMAL : SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command_buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        work.c_str(), &startup, &info)) {
        std::fwprintf(stderr, L"CreateProcess a echoue (%lu) pour %s\n", GetLastError(), exe.c_str());
        return 1;
    }

    g_child_pid = info.dwProcessId;
    std::printf("pid=%lu\n", info.dwProcessId);
    std::fflush(stdout);
    int result = 1;
    uint8_t original[kOrigLen]{};
    DWORD old_protect = 0;
    bool entry_open = false;
    uint32_t entry = 0;
    uint8_t* remote = nullptr;

    uint32_t base = 0;
    if (!ImageBase(info.hProcess, &base) || !EntryPoint(info.hProcess, base, &entry)) {
        std::fprintf(stderr, "entree PE illisible, reprise sans DLL\n");
        goto resume;
    }
    std::printf("base=0x%08lX entree=0x%08lX\n", base, entry);

    if (!VirtualProtectEx(info.hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(entry)), kOrigLen,
                          PAGE_EXECUTE_READWRITE, &old_protect) ||
        !ReadMem(info.hProcess, entry, original, kOrigLen)) {
        std::fprintf(stderr, "lecture de l'entree impossible, reprise sans DLL\n");
        goto resume;
    }
    entry_open = true;

    remote = static_cast<uint8_t*>(VirtualAllocEx(info.hProcess, nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_EXECUTE_READWRITE));
    if (!remote) {
        std::fprintf(stderr, "allocation du stub impossible, reprise sans DLL\n");
        goto restore;
    }

    {
        const uint32_t cave = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(remote));
        RemoteData data{};
        data.status = 0;
        std::memcpy(data.original, original, kOrigLen);
        std::strncpy(data.name, "ScctCoopNet_Install", sizeof(data.name) - 1);
        if (dll.size() >= MAX_PATH) {
            std::fprintf(stderr, "chemin de DLL trop long\n");
            goto restore;
        }
        std::wmemcpy(data.path, dll.c_str(), dll.size() + 1);
        const std::vector<uint8_t> stub = BuildStub(cave, cave + kDataOffset, entry, old_protect);
        if (stub.empty() || stub.size() > kDataOffset ||
            !ProcAddress(L"kernel32.dll", "LoadLibraryW") ||
            !ProcAddress(L"kernel32.dll", "GetProcAddress")) {
            std::fprintf(stderr, "stub incomplet\n");
            goto restore;
        }
        if (!WriteMem(info.hProcess, cave + kDataOffset, &data, sizeof(data)) ||
            !WriteMem(info.hProcess, cave, stub.data(), stub.size())) {
            std::fprintf(stderr, "ecriture du stub impossible\n");
            goto restore;
        }
        const int32_t rel = static_cast<int32_t>(cave - (entry + 5));
        uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
        jump[1] = static_cast<uint8_t>(rel);
        jump[2] = static_cast<uint8_t>(rel >> 8);
        jump[3] = static_cast<uint8_t>(rel >> 16);
        jump[4] = static_cast<uint8_t>(rel >> 24);
        if (!WriteMem(info.hProcess, entry, jump, sizeof(jump))) {
            std::fprintf(stderr, "branchement d'entree impossible\n");
            goto restore;
        }
        FlushInstructionCache(info.hProcess, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(entry)), kOrigLen);
        ResumeThread(info.hThread);
        result = WaitForStatus(info.hProcess, cave + kDataOffset + offsetof(RemoteData, status));
        std::printf("fg_apres=%lu\n", ForegroundPid());
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return result;
    }

restore:
    if (entry_open) {
        DWORD ignored = 0;
        WriteMem(info.hProcess, entry, original, kOrigLen);
        VirtualProtectEx(info.hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(entry)), kOrigLen, old_protect,
                         &ignored);
    }
resume:
    ResumeThread(info.hThread);
    std::printf("fg_apres=%lu\n", ForegroundPid());
    std::printf("statut_cave=0\n");
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

struct WinScan {
    DWORD pid;
    int visible;
};

BOOL CALLBACK CountVisible(HWND hwnd, LPARAM param) {
    WinScan* scan = reinterpret_cast<WinScan*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == scan->pid && IsWindowVisible(hwnd)) {
        ++scan->visible;
    }
    return TRUE;
}

int VisibleWindows(DWORD pid) {
    WinScan scan{pid, 0};
    EnumWindows(CountVisible, reinterpret_cast<LPARAM>(&scan));
    return scan.visible;
}

void PrintIni(const std::wstring& path) {
    std::wprintf(L"ini=%s\n", path.c_str());
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        std::printf("ini_enabled=absent ini_transport=absent ini_relay=absent\n");
        std::fflush(stdout);
        return;
    }
    char buffer[8192];
    const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[got] = '\0';
    std::string enabled = "0";
    std::string transport = "0";
    std::string relay;
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
        const auto semi = line.find(';');
        if (semi != std::string::npos) {
            line.resize(semi);
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        size_t start = 0;
        while (start < key.size() && (key[start] == ' ' || key[start] == '\t')) {
            ++start;
        }
        key = key.substr(start);
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (key == "enabled") {
            enabled = value == "1" ? "1" : "0";
        } else if (key == "transport") {
            transport = value == "1" ? "1" : "0";
        } else if (key == "relay") {
            relay = value;
        }
    }
    std::printf("ini_enabled=%s\nini_transport=%s\nini_relay=%s\n", enabled.c_str(), transport.c_str(), relay.c_str());
    std::fflush(stdout);
}

std::vector<uint8_t> BuildHoldStub(uint32_t data) {
    const uint32_t status_addr = data + offsetof(RemoteData, status);
    const uint32_t name_addr = data + offsetof(RemoteData, name);
    const uint32_t path_addr = data + offsetof(RemoteData, path);
    const uint32_t load_library = ProcAddress(L"kernel32.dll", "LoadLibraryW");
    const uint32_t get_proc = ProcAddress(L"kernel32.dll", "GetProcAddress");
    const uint32_t sleep_fn = ProcAddress(L"kernel32.dll", "Sleep");

    Asm asm_code;
    asm_code.u8(0x60);
    asm_code.u8(0x9C);
    asm_code.mov_eax(status_addr);
    asm_code.u8(0xC7);
    asm_code.u8(0x00);
    asm_code.u32(3);

    asm_code.mov_eax(load_library);
    asm_code.push_imm(path_addr);
    asm_code.call_eax();
    asm_code.test_eax();
    const size_t jz_load = asm_code.jz_near();

    asm_code.push_imm(name_addr);
    asm_code.u8(0x50);
    asm_code.mov_eax(get_proc);
    asm_code.call_eax();
    asm_code.test_eax();
    const size_t jz_proc = asm_code.jz_near();
    asm_code.call_eax();

    asm_code.push_imm(1);
    const size_t jmp_join = asm_code.jmp_near();

    const size_t fail = asm_code.size();
    asm_code.patch_rel32(jz_load, fail);
    asm_code.patch_rel32(jz_proc, fail);
    asm_code.push_imm(2);

    const size_t join = asm_code.size();
    asm_code.patch_rel32(jmp_join, join);
    asm_code.u8(0x58);
    asm_code.mov_edx(status_addr);
    asm_code.u8(0x89);
    asm_code.u8(0x02);
    asm_code.u8(0x9D);
    asm_code.u8(0x61);
    asm_code.mov_eax(sleep_fn);
    asm_code.push_imm(0xFFFFFFFFu);
    asm_code.call_eax();
    asm_code.u8(0xEB);
    asm_code.u8(0xFE);
    return asm_code.bytes;
}

int InjectHold(const std::wstring& exe, const std::wstring& dll) {
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> command_buf(command.begin(), command.end());
    command_buf.push_back(L'\0');
    const std::wstring work = DirectoryOf(exe);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command_buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        work.c_str(), &startup, &info)) {
        std::fwprintf(stderr, L"CreateProcess a echoue (%lu) pour %s\n", GetLastError(), exe.c_str());
        return 1;
    }
    std::printf("pid=%lu\ncreation=CREATE_SUSPENDED\n", info.dwProcessId);
    std::fflush(stdout);

    int result = 1;
    uint32_t entry = 0;
    DWORD old_protect = 0;
    bool entry_open = false;
    uint8_t* remote = nullptr;
    uint32_t base = 0;
    if (!ImageBase(info.hProcess, &base) || !EntryPoint(info.hProcess, base, &entry)) {
        std::fprintf(stderr, "entree PE illisible\n");
        TerminateProcess(info.hProcess, 1);
        goto done;
    }
    std::printf("base=0x%08lX entree=0x%08lX\n", base, entry);
    std::fflush(stdout);
    if (!VirtualProtectEx(info.hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(entry)), 5,
                          PAGE_EXECUTE_READWRITE, &old_protect)) {
        std::fprintf(stderr, "protection de l'entree impossible\n");
        TerminateProcess(info.hProcess, 1);
        goto done;
    }
    entry_open = true;
    remote = static_cast<uint8_t*>(VirtualAllocEx(info.hProcess, nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_EXECUTE_READWRITE));
    if (!remote || dll.size() >= MAX_PATH || !ProcAddress(L"kernel32.dll", "LoadLibraryW") ||
        !ProcAddress(L"kernel32.dll", "GetProcAddress") || !ProcAddress(L"kernel32.dll", "Sleep")) {
        std::fprintf(stderr, "stub incomplet\n");
        TerminateProcess(info.hProcess, 1);
        goto done;
    }
    {
        const uint32_t cave = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(remote));
        RemoteData data{};
        data.status = 0;
        std::strncpy(data.name, "ScctCoopNet_Install", sizeof(data.name) - 1);
        std::wmemcpy(data.path, dll.c_str(), dll.size() + 1);
        const std::vector<uint8_t> stub = BuildHoldStub(cave + kDataOffset);
        if (stub.empty() || stub.size() > kDataOffset ||
            !WriteMem(info.hProcess, cave + kDataOffset, &data, sizeof(data)) ||
            !WriteMem(info.hProcess, cave, stub.data(), stub.size())) {
            std::fprintf(stderr, "ecriture du stub impossible\n");
            TerminateProcess(info.hProcess, 1);
            goto done;
        }
        const int32_t rel = static_cast<int32_t>(cave - (entry + 5));
        uint8_t jump[5] = {0xE9, static_cast<uint8_t>(rel), static_cast<uint8_t>(rel >> 8),
                           static_cast<uint8_t>(rel >> 16), static_cast<uint8_t>(rel >> 24)};
        if (!WriteMem(info.hProcess, entry, jump, sizeof(jump))) {
            std::fprintf(stderr, "branchement d'entree impossible\n");
            TerminateProcess(info.hProcess, 1);
            goto done;
        }
        FlushInstructionCache(info.hProcess, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(entry)), 5);
        ResumeThread(info.hThread);
        std::printf("reprise=stub entree_jeu=non_atteinte\n");
        std::fflush(stdout);
        const uint32_t status_addr = cave + kDataOffset + offsetof(RemoteData, status);
        for (int i = 0; i < 200; ++i) {
            if (VisibleWindows(info.dwProcessId) > 0) {
                std::printf("fenetre_visible=1\n");
                std::fflush(stdout);
                TerminateProcess(info.hProcess, 1);
                result = 6;
                goto done;
            }
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(info.hProcess, &exit_code);
            if (exit_code != STILL_ACTIVE) {
                std::fprintf(stderr, "processus termine code=%lu\n", exit_code);
                result = 4;
                goto done;
            }
            uint32_t status = 0;
            if (ReadMem(info.hProcess, status_addr, &status, sizeof(status)) && (status == 1 || status == 2)) {
                const int visible = VisibleWindows(info.dwProcessId);
                std::printf("statut_injection=%u\nfenetre_visible=%d\n", status, visible);
                std::fflush(stdout);
                if (visible > 0) {
                    TerminateProcess(info.hProcess, 1);
                    result = 6;
                    goto done;
                }
                result = status == 1 ? 0 : 3;
                goto done;
            }
            Sleep(50);
        }
        std::fprintf(stderr, "delai depasse avant le retour du stub\n");
        TerminateProcess(info.hProcess, 1);
        result = 5;
    }

done:
    if (result != 0 && entry_open) {
        DWORD ignored = 0;
        VirtualProtectEx(info.hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(entry)), 5, old_protect,
                         &ignored);
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring self = ModulePath();
    const std::wstring dir = DirectoryOf(self);
    bool hold = false;
    std::wstring exe;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--suspendu") == 0) {
            hold = true;
        } else if (std::wcscmp(argv[i], L"--visible") == 0) {
            g_visible = true;
        } else if (std::wcscmp(argv[i], L"--relay") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_RELAY", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--local-ip") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_LOCAL_IP", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--log") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_LOG", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--enabled") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_ENABLED", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--transport") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_TRANSPORT", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--name") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_NAME", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--friendly-fire") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_FFIRE", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--voice") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_VOICE", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--mic-device") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_MIC_DEVICE", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--api-url") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_LAUNCHER_API", argv[++i]);
        } else if (std::wcscmp(argv[i], L"--api-token") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"SCCT_INGAME_TOKEN", argv[++i]);
        } else if (exe.empty()) {
            exe = argv[i];
        }
    }
    if (exe.empty()) {
        exe = dir + L"\\splintercell3.exe";
    }
    const std::wstring beside_exe = DirectoryOf(exe) + L"\\ScctCoopNet.dll";
    const std::wstring dll = GetFileAttributesW(beside_exe.c_str()) != INVALID_FILE_ATTRIBUTES ? beside_exe
                                                                                               : dir + L"\\ScctCoopNet.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"DLL absente: %s\n", dll.c_str());
        return 2;
    }
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"exe absent: %s\n", exe.c_str());
        return 2;
    }
    if (g_visible) {
        SetEnvironmentVariableW(L"SCCT_SHOW", L"1");
    }
    std::fwprintf(stdout, L"dll=%s\nexe=%s\n", dll.c_str(), exe.c_str());
    PrintIni(DirectoryOf(exe) + L"\\scct-coop-net.ini");
    std::fflush(stdout);
    g_hold = hold;
    const int code = hold ? InjectHold(exe, dll) : Launch(exe, dll);
    std::printf("lanceur=%d\n", code);
    std::fflush(stdout);
    if (!hold && g_child_pid != 0) {
        if (g_visible) {
            WaitVisible(g_child_pid);
        } else {
            WatchOffscreen(g_child_pid);
        }
    }
    return code;
}

