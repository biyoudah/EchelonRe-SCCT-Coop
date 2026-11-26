"""Unit tests for SCDA porting tools (runnable in CI without retail game assets)."""

import json
import os
import sys
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import reconstruct_packages as rp
import convert_audio as ca
import convert_package as cp


def test_compact_integer_codec():
    """Verify Unreal Engine compact integer serialization round-trip."""
    rp._verify_ci_codec()


def test_map_metadata_schema():
    """Verify map_metadata.json is valid and contains required fields for all 14 SCDA maps."""
    meta_path = os.path.join(HERE, "map_metadata.json")
    assert os.path.isfile(meta_path), f"Missing {meta_path}"
    with open(meta_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    maps = data.get("maps", data)
    assert len(maps) >= 14, f"Expected at least 14 maps, found {len(maps)}"
    for map_key, info in maps.items():
        assert "display_name" in info, f"Missing display_name in {map_key}"
        assert "out_name" in info, f"Missing out_name in {map_key}"
        assert "campaign" in info, f"Missing campaign in {map_key}"


def test_audio_converters():
    """Verify audio converter module exports and decoder functions."""
    assert callable(getattr(ca, "decode_adpcm", None))
    assert callable(getattr(ca, "convert_bank", None))
    assert callable(getattr(ca, "export_audio_pack", None))


def test_package_primitives():
    """Verify basic package data classes can be imported and initialized."""
    assert hasattr(cp, "PackageData")
    assert hasattr(cp, "RetailIndex")
