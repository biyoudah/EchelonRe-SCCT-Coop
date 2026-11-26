"""End-to-end builder for SCDA-Coop-Maps-Pack.zip and content-manifest.json."""

import hashlib
import json
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
META_PATH = os.path.join(HERE, "map_metadata.json")
DUMP_ROOT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "dumped")
RECON_ROOT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "reconstructed")
CONV_ROOT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "converted")
AUDIO_STAGING = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "pack_staging", "Data", "Sounds", "scda_audio_pack")
ZIP_OUT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "SCDA-Coop-Maps-Pack.zip")
MANIFEST_OUT = os.path.join(REPO_ROOT, "content-manifest.json")

CREDITS_TEXT = (
    "Splinter Cell: Double Agent (Xbox) Co-op Maps Pack for Chaos Theory\n"
    "====================================================================\n\n"
    "SCDA assets were extracted and ported using SCDA-XBERecomp by TGP482:\n"
    "  - https://github.com/TGP482/SCDA-XBERecomp\n"
    "  - https://github.com/TGP482/XboxXBERecomp\n\n"
    "Widescreen Fix by ThirteenAG:\n"
    "  - https://github.com/ThirteenAG/WidescreenFixesPack\n"
)


def run_step(label, cmd):
    print(f"\n=== [{label}] {' '.join(cmd)} ===", flush=True)
    res = subprocess.run(cmd, cwd=REPO_ROOT)
    if res.returncode != 0:
        raise RuntimeError(f"Step '{label}' failed with exit code {res.returncode}")


def has_reconstructed(map_name):
    d = os.path.join(RECON_ROOT, map_name)
    if not os.path.isdir(d):
        return False
    return any(fn.lower().endswith(".scl") for fn in os.listdir(d))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().lower()


def main():
    with open(META_PATH, "r", encoding="utf-8") as f:
        meta = json.load(f).get("maps", {})

    py = sys.executable
    ordered_maps = []

    for map_name, info in meta.items():
        out_name = info["out_name"]
        ordered_maps.append({"map_name": map_name, **info})

        dump_done = os.path.join(DUMP_ROOT, map_name, "dump_complete.txt")
        if not os.path.isfile(dump_done):
            run_step(f"dump:{map_name}", [py, os.path.join(HERE, "dump_map.py"), map_name])
        else:
            print(f"[SKIP] dump:{map_name} (already dumped)", flush=True)

        if not has_reconstructed(map_name):
            run_step(f"reconstruct:{map_name}", [py, os.path.join(HERE, "reconstruct_packages.py"), map_name])
        else:
            print(f"[SKIP] reconstruct:{map_name} (already reconstructed)", flush=True)

        conv_dir = os.path.join(CONV_ROOT, out_name)
        run_step(
            f"convert_package:{map_name}",
            [py, os.path.join(HERE, "convert_package.py"), map_name, "--out-name", out_name],
        )
        run_step(f"validate_pkg:{out_name}", [py, os.path.join(HERE, "validate_pkg.py"), conv_dir])
        run_step(
            f"convert_loading:{map_name}",
            [py, os.path.join(HERE, "convert_loading.py"), map_name, "--out-name", out_name, "--out-dir", conv_dir],
        )

    run_step(
        "export_audio_pack",
        [py, os.path.join(HERE, "convert_audio.py"), "--export-pack", AUDIO_STAGING],
    )

    print(f"\n=== [zip] Writing {ZIP_OUT} ===", flush=True)
    os.makedirs(os.path.dirname(ZIP_OUT), exist_ok=True)
    seen = set()

    def add_file(zf, src_path, arcname):
        arc = arcname.replace("\\", "/").lstrip("/")
        key = arc.lower()
        if key in seen:
            return
        seen.add(key)
        zf.write(src_path, arc)

    def add_bytes(zf, data, arcname):
        arc = arcname.replace("\\", "/").lstrip("/")
        key = arc.lower()
        if key in seen:
            return
        seen.add(key)
        zf.writestr(arc, data)

    with zipfile.ZipFile(ZIP_OUT, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for item in ordered_maps:
            map_name = item["map_name"]
            out_name = item["out_name"]
            conv_dir = os.path.join(CONV_ROOT, out_name)
            for root, _, files in os.walk(conv_dir):
                for fn in files:
                    if fn.lower() == "convert_manifest.json":
                        continue
                    src = os.path.join(root, fn)
                    rel = os.path.relpath(src, conv_dir).replace("\\", "/")
                    add_file(zf, src, rel)

            loc_dir = os.path.join(conv_dir, "Data", "System", "Localization")
            for fn in (f"{out_name}.int", f"P_{map_name}.int"):
                src = os.path.join(loc_dir, fn)
                if os.path.isfile(src):
                    add_file(zf, src, f"System/Localization/{fn}")

        override_int = os.path.join(HERE, "scda_override.int")
        if os.path.isfile(override_int):
            add_file(zf, override_int, "System/Localization/scda_override.int")

        for fn in sorted(os.listdir(AUDIO_STAGING)):
            src = os.path.join(AUDIO_STAGING, fn)
            if os.path.isfile(src):
                add_file(zf, src, f"Data/Sounds/scda_audio_pack/{fn}")

        pack_manifest = {
            "version": "1.0.0",
            "credits": "SCDA assets ported using SCDA-XBERecomp by TGP482 (https://github.com/TGP482/SCDA-XBERecomp)",
            "maps": [item["out_name"] for item in ordered_maps],
            "display_names": {item["out_name"]: item["display_name"] for item in ordered_maps},
        }
        add_bytes(zf, json.dumps(pack_manifest, indent=2).encode("utf-8"), "Data/Maps/scda_pack_manifest.json")
        add_bytes(zf, CREDITS_TEXT.encode("utf-8"), "SCDA_CREDITS.txt")

    zip_bytes = os.path.getsize(ZIP_OUT)
    zip_mb = max(1, round(zip_bytes / (1024 * 1024)))
    zip_sha = sha256_file(ZIP_OUT)
    print(f"[DONE] {ZIP_OUT}: {zip_bytes} bytes ({zip_mb} MB), sha256={zip_sha}", flush=True)

    check_files = [f"Data/Maps/{item['out_name']}.scl" for item in ordered_maps] + [
        "Data/Sounds/SCDA_MAPS.SM0",
        "System/Localization/scda_override.int",
    ]
    content_manifest = {
        "packs": [
            {
                "id": "scda-coop-maps",
                "name": "SCDA Co-op Maps Pack",
                "version": "1.0.0",
                "description": (
                    "All 14 ported Splinter Cell: Double Agent Xbox co-op missions "
                    "(Iceland, Prison, Kinshasa) with DareAudio SFX/dialogue banks, loading screens, and localization."
                ),
                "credits": "SCDA assets ported using SCDA-XBERecomp by TGP482 (https://github.com/TGP482/SCDA-XBERecomp)",
                "size_mb": zip_mb,
                "download_url": "https://scct-revival-directory.echelonre.workers.dev/v2/content/scda-coop-maps",
                "sha256": zip_sha,
                "check_files": check_files,
            },
            {
                "id": "widescreen-fix",
                "name": "Widescreen Fix (ThirteenAG)",
                "version": "1.0",
                "description": "16:9 / ultrawide resolution, HUD scaling, and FOV support for Chaos Theory.",
                "credits": "By ThirteenAG (https://github.com/ThirteenAG/WidescreenFixesPack)",
                "size_mb": 2,
                "download_url": "https://github.com/ThirteenAG/WidescreenFixesPack/releases/latest/download/SplinterCellChaosTheory.WidescreenFix.zip",
                "sha256": "",
                "check_files": ["System/scripts/SplinterCellChaosTheory.WidescreenFix.asi"],
            },
        ]
    }
    with open(MANIFEST_OUT, "w", encoding="utf-8") as mf:
        json.dump(content_manifest, mf, indent=2)
    print(f"[DONE] Wrote {MANIFEST_OUT}", flush=True)


if __name__ == "__main__":
    main()
