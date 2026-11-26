import subprocess
import sys

def main():
    maps = [
        "scda_ice_a", "scda_ice_b", "scda_ice_c", "scda_ice_d", "scda_ice_e",
        "scda_pris_a", "scda_pris_b", "scda_pris_c", "scda_pris_d", "scda_pris_e",
        "scda_kin_a", "scda_kin_b", "scda_kin_c", "scda_kin_f"
    ]

    results = {}
    for m in maps:
        res = subprocess.run(
            [sys.executable, "tools/scda_port/test_map_ct.py", m, "--auto-exit-ms", "1000", "--timeout-ms", "25000"],
            capture_output=True,
            text=True
        )
        ok = (res.returncode == 0) and ("LOADMAP_RETURN" in res.stdout)
        results[m] = ok
        status = "PASS" if ok else f"FAIL (code={res.returncode})"
        print(f"[{status}] {m}", flush=True)

    print("\n--- FINAL RESULTS ---")
    all_passed = True
    for m, ok in results.items():
        tag = "PASS" if ok else "FAIL"
        print(f"  {m}: {tag}")
        if not ok:
            all_passed = False

    if not all_passed:
        sys.exit(1)
    print("\nAll 14 maps booted successfully without errors!")

if __name__ == "__main__":
    main()

