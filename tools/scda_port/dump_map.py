r"""
dump_map.py

Compiles tools/scda_port/lin_dumper.dll (via MSVC x64), writes launch.bin into
D:\Jeux\Splinter Cell Double Agent\hdd\launch.bin for the requested co-op map,
launches SCDA.exe suspended, injects lin_dumper.dll, and waits for the dump
to complete cleanly.
"""

import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCDA_DIR = r"D:\Jeux\Splinter Cell Double Agent"
SCDA_EXE = os.path.join(SCDA_DIR, "SCDA.exe")
LAUNCH_BIN = os.path.join(SCDA_DIR, "hdd", "launch.bin")
VCVARS64 = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
DLL_SRC = os.path.join(REPO_ROOT, "tools", "scda_port", "lin_dumper.cpp")
DLL_OUT = os.path.join(REPO_ROOT, "tools", "scda_port", "lin_dumper.dll")
DUMP_ROOT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "dumped")


def ensure_dll_built():
    if os.path.exists(DLL_OUT) and os.path.getmtime(DLL_OUT) >= os.path.getmtime(DLL_SRC):
        return
    print("[*] Compiling lin_dumper.dll via MSVC x64...")
    bat_path = os.path.join(os.path.dirname(DLL_SRC), "_build_dumper.bat")
    with open(bat_path, "w") as bf:
        bf.write(f'@echo off\ncall "{VCVARS64}" >nul\ncl /nologo /O2 /LD /EHsc "{DLL_SRC}" /Fe:"{DLL_OUT}"\n')
    try:
        res = subprocess.run([bat_path], cwd=os.path.dirname(DLL_SRC), capture_output=True, text=True, shell=True)
    finally:
        if os.path.exists(bat_path):
            os.remove(bat_path)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise RuntimeError("Failed to compile lin_dumper.dll")
    print(f"[OK] Built {DLL_OUT}")


def write_launch_bin(map_name):
    # Xbox LAUNCH_DATA_PAGE (4096 bytes):
    # +0x000: dwLaunchDataType = 3 (launches directly into MAPNAME=<LaunchData>)
    # +0x004: dwTitleId = 0x5553005E
    # +0x008: szLaunchPath = "d:\\default.xbe"
    # +0x400: LaunchData = UTF-16LE map_name
    buf = bytearray(0x1000)
    struct.pack_into("<II", buf, 0, 3, 0x5553005E)
    path_bytes = b"d:\\default.xbe\x00"
    buf[8:8 + len(path_bytes)] = path_bytes
    u16 = (map_name + "\x00").encode("utf-16le")
    buf[0x400:0x400 + len(u16)] = u16
    os.makedirs(os.path.dirname(LAUNCH_BIN), exist_ok=True)
    with open(LAUNCH_BIN, "wb") as f:
        f.write(buf)
    print(f"[*] Wrote {LAUNCH_BIN} for map '{map_name}'")


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
        ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
        ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
        ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
        ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
        ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD),
    ]


def inject_and_run(map_name, timeout_sec=120):
    out_dir = os.path.join(DUMP_ROOT, map_name)
    os.makedirs(out_dir, exist_ok=True)
    done_file = os.path.join(out_dir, "dump_complete.txt")
    if os.path.exists(done_file):
        os.remove(done_file)

    if os.path.exists(LAUNCH_BIN):
        os.remove(LAUNCH_BIN)

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                   wt.BOOL, wt.DWORD, ctypes.c_void_p, wt.LPCWSTR,
                                   ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)]
    k32.CreateProcessW.restype = wt.BOOL
    k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
    k32.VirtualAllocEx.restype = ctypes.c_void_p
    k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    k32.WriteProcessMemory.restype = wt.BOOL
    k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
    k32.GetModuleHandleW.restype = wt.HMODULE
    k32.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]
    k32.GetProcAddress.restype = ctypes.c_void_p
    k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
    k32.CreateRemoteThread.restype = wt.HANDLE
    k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k32.WaitForSingleObject.restype = wt.DWORD
    k32.ResumeThread.argtypes = [wt.HANDLE]
    k32.ResumeThread.restype = wt.DWORD

    os.environ["SCDA_DUMP_OUT"] = out_dir
    os.environ["SCDA_DUMP_MAP"] = map_name

    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    pi = PROCESS_INFORMATION()
    CREATE_SUSPENDED = 0x00000004

    ok = k32.CreateProcessW(SCDA_EXE, ctypes.create_unicode_buffer(f'"{SCDA_EXE}"'), None, None, False,
                            CREATE_SUSPENDED, None, SCDA_DIR, ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        raise ctypes.WinError(ctypes.get_last_error())

    try:
        dll_w = (DLL_OUT + "\x00").encode("utf-16le")
        mem = k32.VirtualAllocEx(pi.hProcess, None, len(dll_w), 0x3000, 0x04)
        if not mem:
            raise ctypes.WinError(ctypes.get_last_error())
        written = ctypes.c_size_t()
        if not k32.WriteProcessMemory(pi.hProcess, mem, dll_w, len(dll_w), ctypes.byref(written)):
            raise ctypes.WinError(ctypes.get_last_error())
        h_k32 = k32.GetModuleHandleW("kernel32.dll")
        load_lib = k32.GetProcAddress(h_k32, b"LoadLibraryW")
        rt = k32.CreateRemoteThread(pi.hProcess, None, 0, load_lib, mem, 0, None)
        if not rt:
            raise ctypes.WinError(ctypes.get_last_error())
        k32.WaitForSingleObject(rt, 10000)
        k32.CloseHandle(rt)

        print(f"[*] Resuming SCDA.exe (PID {pi.dwProcessId}) to dump '{map_name}'...")
        k32.ResumeThread(pi.hThread)

        wait_res = k32.WaitForSingleObject(pi.hProcess, int(timeout_sec * 1000))
        if wait_res == 0:
            print(f"[OK] SCDA.exe exited cleanly! Checking {out_dir}")
        else:
            print(f"[WARN] Timed out after {timeout_sec}s; dump_complete exists={os.path.exists(done_file)}")
    finally:
        k32.CloseHandle(pi.hThread)
        k32.CloseHandle(pi.hProcess)


if __name__ == "__main__":
    map_name = sys.argv[1] if len(sys.argv) > 1 else "x01_coop_ice_a"
    ensure_dll_built()
    inject_and_run(map_name)
