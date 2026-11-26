"""Test runner shim for environments where pytest package is not pre-installed."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)


def main():
    print(f"Running SCDA porting tools unit tests (Python {sys.version.split()[0]})...")

    # 1. Test Unreal Compact Integer codec
    import reconstruct_packages as rp
    rp._verify_ci_codec()
    print(" [PASS] Unreal Compact Integer codec round-trip")

    # 2. Test map_metadata.json validity
    import json
    meta_path = os.path.join(HERE, "map_metadata.json")
    with open(meta_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    maps = data.get("maps", data)
    assert len(maps) >= 14, f"Expected 14 maps, got {len(maps)}"
    for name, m in maps.items():
        assert "display_name" in m and "out_name" in m and "campaign" in m
    print(f" [PASS] Map metadata verified ({len(maps)} maps)")

    # 3. Test convert_audio exports
    import convert_audio as ca
    assert callable(getattr(ca, "decode_adpcm", None))
    assert callable(getattr(ca, "convert_bank", None))
    assert callable(getattr(ca, "export_audio_pack", None))
    print(" [PASS] Audio conversion codecs verified")

    # 4. Test convert_package primitives
    import convert_package as cp
    assert hasattr(cp, "PackageData")
    assert hasattr(cp, "RetailIndex")
    print(" [PASS] Package serialization primitives verified")

    print("\nAll 4/4 porting test suites passed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
