"""Single-command orchestrator for porting an SCDA Xbox co-op map to SCCT PC."""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
META_PATH = os.path.join(HERE, "map_metadata.json")
DEFAULT_CT_DIR = r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory"


def load_metadata():
    with open(META_PATH, "r", encoding="utf-8") as f:
        return json.load(f).get("maps", {})


def run_step(label, cmd):
    print(f"\n=== [{label}] {' '.join(cmd)} ===")
    res = subprocess.run(cmd, cwd=REPO_ROOT)
    if res.returncode != 0:
        raise RuntimeError(f"Step '{label}' failed with exit code {res.returncode}")


def main():
    meta = load_metadata()
    ap = argparse.ArgumentParser(description="Port an SCDA co-op map end-to-end into SCCT PC.")
    ap.add_argument("--map", help="Source SCDA map name (e.g. x01_coop_ice_a)")
    ap.add_argument("--out-name", help="Target SCCT map stem (defaults from map_metadata.json)")
    ap.add_argument("--ct-dir", default=DEFAULT_CT_DIR, help="Chaos Theory root directory")
    ap.add_argument("--stage", action="store_true", help="Stage converted packages into --ct-dir")
    ap.add_argument("--skip-audio", action="store_true", help="Skip DareAudio bank conversion")
    ap.add_argument("--skip-loading", action="store_true", help="Skip loading screen & icon conversion")
    ap.add_argument("--list", action="store_true", help="List known SCDA co-op maps and exit")
    args = ap.parse_args()

    if args.list:
        for k, v in meta.items():
            print(f"{k:18s} -> {v['out_name']:14s} ({v['campaign']}: {v['display_name']})")
        return

    if not args.map:
        ap.error("--map is required unless --list is passed")

    info = meta.get(args.map, {})
    out_name = args.out_name or info.get("out_name")
    if not out_name:
        ap.error(f"Unknown map '{args.map}'; pass --out-name explicitly")

    py = sys.executable
    pkg_cmd = [py, os.path.join(HERE, "convert_package.py"), args.map, "--out-name", out_name, "--ct-dir", args.ct_dir]
    if args.stage:
        pkg_cmd.append("--stage")
    run_step("convert_package", pkg_cmd)

    conv_dir = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "converted", out_name)
    run_step("validate_pkg", [py, os.path.join(HERE, "validate_pkg.py"), conv_dir])

    if not args.skip_audio:
        sounds_dir = os.path.join(args.ct_dir, "Data", "Sounds")
        run_step("convert_audio", [py, os.path.join(HERE, "convert_audio.py"), "--map", args.map, "--out-name", out_name, "--sounds", sounds_dir])

    if not args.skip_loading:
        run_step("convert_loading", [py, os.path.join(HERE, "convert_loading.py"), args.map, "--out-name", out_name, "--out-dir", args.ct_dir])

    print(f"\n[DONE] Ported {args.map} -> {out_name}")


if __name__ == "__main__":
    main()
