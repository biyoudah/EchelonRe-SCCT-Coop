"""Build x86 probe/launcher into artifact scratch dir and test-load a map in splintercell3.exe."""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SCRATCH = os.environ.get("SCCT_PROBE_DIR", os.path.join(HERE, "probe_bin"))
VCVARS32 = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"


def ensure_built():
    probe_cpp = os.path.join(HERE, "ct_probe.cpp")
    launch_cpp = os.path.join(HERE, "ct_launcher.cpp")
    probe_dll = os.path.join(SCRATCH, "ct_probe.dll")
    launch_exe = os.path.join(SCRATCH, "ct_launcher.exe")
    os.makedirs(SCRATCH, exist_ok=True)

    need_probe = (not os.path.exists(probe_dll)) or (os.path.getmtime(probe_cpp) > os.path.getmtime(probe_dll))
    need_launch = (not os.path.exists(launch_exe)) or (os.path.getmtime(launch_cpp) > os.path.getmtime(launch_exe))
    if not need_probe and not need_launch:
        return probe_dll, launch_exe

    bat = os.path.join(SCRATCH, "build_ct_probe.bat")
    lines = [f'@call "{VCVARS32}" >nul']
    if need_probe:
        lines.append(
            f'cl /nologo /O2 /LD "{probe_cpp}" /Fe:"{probe_dll}" /Fo:"{SCRATCH}\\ct_probe.obj" /link user32.lib kernel32.lib || exit /b 1'
        )
    if need_launch:
        lines.append(
            f'cl /nologo /O2 "{launch_cpp}" /Fe:"{launch_exe}" /Fo:"{SCRATCH}\\ct_launcher.obj" /link user32.lib kernel32.lib || exit /b 1'
        )
    with open(bat, "w") as f:
        f.write("\r\n".join(lines) + "\r\n")
    res = subprocess.run(["cmd.exe", "/c", bat], capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise RuntimeError(f"x86 build failed with {res.returncode}")
    return probe_dll, launch_exe


def main():
    ap = argparse.ArgumentParser(description="Live test converted map in Chaos Theory PC")
    ap.add_argument("map_name", help="Map stem (e.g. scda_ice_a)")
    ap.add_argument("--auto-exit-ms", type=int, default=4000, help="Auto exit delay after LoadMap succeeds (0 = stay open)")
    ap.add_argument("--timeout-ms", type=int, default=40000, help="Watchdog timeout ms")
    ap.add_argument("--ct-dir", default=r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory", help="Target Chaos Theory root folder")
    ap.add_argument("--exe", default="splintercell3.exe", help="Game executable inside System/")
    ap.add_argument("--sound", action="store_true", help="Enable sound (omit -nosound)")
    ap.add_argument("--extra", nargs="*", default=[], help="Extra game cmdline args")
    args = ap.parse_args()

    probe_dll, launch_exe = ensure_built()
    sys_dir = os.path.join(args.ct_dir, "System")
    exe_path = os.path.join(sys_dir, args.exe)
    out_dir = os.path.join(REPO, "SCDA_Coop_Maps", "logs")
    os.makedirs(out_dir, exist_ok=True)
    ct_log = os.path.join(out_dir, f"{args.map_name}_ct.log")
    sys_log = os.path.join(sys_dir, f"{args.map_name}_ct.log")
    probe_log = os.path.join(out_dir, f"{args.map_name}_probe.log")
    for p in (ct_log, sys_log, probe_log):
        if os.path.exists(p):
            os.remove(p)

    env = os.environ.copy()
    env["CT_PROBE_LOG"] = probe_log
    env["CT_PROBE_AUTOEXIT_MS"] = str(args.auto_exit_ms)
    env["CT_PROBE_TIMEOUT_MS"] = str(args.timeout_ms)

    log_name = f"{args.map_name}_ct.log"
    cmd = [
        launch_exe,
        exe_path,
        sys_dir,
        probe_dll,
        args.map_name,
        f"LOG={log_name}",
        "-log",
    ] + ([] if args.sound else ["-nosound"]) + [
        "-window",
    ] + args.extra

    print("Launching:", " ".join(cmd))
    res = subprocess.run(cmd, env=env, capture_output=True, text=True)
    print(res.stdout)
    if res.stderr:
        print("STDERR:", res.stderr)

    if os.path.exists(sys_log):
        shutil.move(sys_log, ct_log)

    print("\n--- PROBE LOG ---")
    if os.path.exists(probe_log):
        print(open(probe_log, "r", errors="replace").read())
    else:
        print("(no probe log)")

    print("\n--- CT LOG (last 80 lines) ---")
    if os.path.exists(ct_log):
        lines = open(ct_log, "r", errors="replace").read().splitlines()
        for ln in lines[-80:]:
            print(ln)
    else:
        print("(no CT log)")
    sys.exit(res.returncode)


def test_live_map_probe():
    try:
        import pytest
        pytest.skip("Live CT test requires Windows MSVC and splintercell3.exe")
    except ImportError:
        pass


if __name__ == "__main__":
    main()
