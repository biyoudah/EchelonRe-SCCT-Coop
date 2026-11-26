#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        fwprintf(stderr, L"Usage: ct_launcher <exe> <cwd> <dll> [args...]\n");
        return 10;
    }
    const wchar_t* exe = argv[1];
    const wchar_t* cwd = argv[2];
    const wchar_t* dll = argv[3];

    wchar_t cmd[2048] = {0};
    swprintf_s(cmd, 2048, L"\"%s\"", exe);
    for (int i = 4; i < argc; i++) {
        wcscat_s(cmd, 2048, L" ");
        wcscat_s(cmd, 2048, argv[i]);
    }

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, cwd, &si, &pi)) {
        fwprintf(stderr, L"CreateProcessW failed: %lu\n", GetLastError());
        return 11;
    }

    size_t sz = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(pi.hProcess, NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError());
        return 12;
    }
    WriteProcessMemory(pi.hProcess, mem, dll, sz, NULL);

    LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, ll, mem, 0, NULL);
    if (!th) {
        fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
        return 13;
    }
    WaitForSingleObject(th, 15000);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);

    ResumeThread(pi.hThread);
    DWORD wait_res = WaitForSingleObject(pi.hProcess, 65000);
    DWORD exit_code = 99;
    if (wait_res == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    } else {
        fwprintf(stderr, L"WaitForSingleObject returned %lu\n", wait_res);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    wprintf(L"CT exited code=%lu\n", exit_code);
    return (int)exit_code;
}
