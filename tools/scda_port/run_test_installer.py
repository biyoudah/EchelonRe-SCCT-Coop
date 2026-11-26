import os
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "coop-net", "Ouais"))

import server

def main():
    game_root = r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory"
    pack = next(p for p in server.DEFAULT_CONTENT_CATALOG if p["id"] == "scda-coop-maps")

    print("Testing map installer end-to-end...")
    server._install_pack_worker(pack, game_root)

    job = server._content_jobs.get("scda-coop-maps")
    print("Job status:", job)

    if not job or job.get("status") != "done":
        print("Installer failed with job:", job)
        sys.exit(1)

    # Check all check_files
    installed = server._pack_installed(pack, game_root)
    print("Pack installed verification:", installed)
    if not installed:
        for f in pack["check_files"]:
            full = os.path.join(game_root, f)
            if not os.path.isfile(full):
                print(f"Missing expected check file: {f}")
        sys.exit(1)

    print("Installer end-to-end test PASSED!")

if __name__ == "__main__":
    main()

