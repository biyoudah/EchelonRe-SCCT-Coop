"""
iso_extract.py

Extracts all 14 SCDA Xbox co-op .lin maps, common.lin, co-op audio .uax files,
and companion metadata (.ini, .occ, .int) from DynamicXbox.umd into
SCDA_Coop_Maps/extracted/, decompressing all .lin files into .lin.dec.
"""

import os
import struct
import zlib

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ISO_PATH = r"D:\Jeux\Splinter Cell Double Agent\assets\Tom Clancy's Splinter Cell - Double Agent (USA, Europe) (En,Fr,De,Es,It).iso"
UMD_PATH = r"D:\Jeux\Splinter Cell Double Agent\hdd\Z\Default\DynamicXbox.umd"
ISO_LIST_PATH = os.path.join(REPO_ROOT, "iso_file_list.txt")
OUT_DIR = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "extracted")

COOP_MAPS = [
    "x01_coop_ice_a", "x01_coop_ice_b", "x01_coop_ice_c", "x01_coop_ice_d", "x01_coop_ice_e",
    "x02_coop_pris_a", "x02_coop_pris_b", "x02_coop_pris_c", "x02_coop_pris_d", "x02_coop_pris_e",
    "x03_coop_kin_a", "x03_coop_kin_b", "x03_coop_kin_c", "x03_coop_kin_f",
]


def parse_iso_list():
    entries = {}
    with open(ISO_LIST_PATH, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.strip().split("|")
            if len(parts) != 3:
                continue
            size = int(parts[0].strip())
            off = int(parts[1].strip(), 16)
            rel_path = parts[2].strip()
            entries[rel_path.lower()] = (size, off, rel_path)
    return entries


def decompress_lin(raw_bytes):
    pos = 0
    out = []
    n = len(raw_bytes)
    while pos + 8 <= n:
        uncomp_sz, comp_sz = struct.unpack_from("<II", raw_bytes, pos)
        pos += 8
        if pos + comp_sz > n:
            raise ValueError(f"Truncated zlib chunk at 0x{pos-8:X}")
        dec = zlib.decompress(raw_bytes[pos:pos + comp_sz])
        if len(dec) != uncomp_sz:
            raise ValueError(f"Chunk size mismatch: expected {uncomp_sz}, got {len(dec)}")
        out.append(dec)
        pos += comp_sz
    if pos != n:
        raise ValueError(f"Trailing bytes after zlib stream: {n - pos}")
    return b"".join(out)


def read_umd_compact_index(f):
    b0 = f.read(1)[0]
    neg = (b0 & 0x80) != 0
    val = b0 & 0x3F
    if b0 & 0x40:
        shift = 6
        while True:
            b = f.read(1)[0]
            val |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                break
    return -val if neg else val


def extract_from_umd(out_meta_dir):
    os.makedirs(out_meta_dir, exist_ok=True)
    extracted = []
    with open(UMD_PATH, "rb") as f:
        f.seek(-20, 2)
        magic, dir_off, size, flags = struct.unpack("<IIII", f.read(16))
        if magic != 0x9FE3C5A3:
            raise ValueError(f"Unexpected UMD magic: 0x{magic:08X}")
        f.seek(dir_off)
        cnt = read_umd_compact_index(f)
        entries = []
        for _ in range(cnt):
            slen = read_umd_compact_index(f)
            name = f.read(slen)[:-1].decode("latin1", errors="replace")
            off, sz, fl = struct.unpack("<III", f.read(12))
            entries.append((name, off, sz))

        for name, off, sz in entries:
            nlo = name.lower()
            base = os.path.basename(name)
            blo = base.lower()
            want = False
            if any(m in blo for m in COOP_MAPS):
                want = True
            elif blo in ("splintercell4xbox.ini", "splintercell4platform.ini", "equipments.ini"):
                want = True
            if not want:
                continue
            f.seek(off)
            blob = f.read(sz)
            dest = os.path.join(out_meta_dir, base)
            with open(dest, "wb") as out:
                out.write(blob)
            extracted.append((base, sz))
    return extracted


def main():
    lin_dir = os.path.join(OUT_DIR, "lin")
    dec_dir = os.path.join(OUT_DIR, "lin_dec")
    snd_dir = os.path.join(OUT_DIR, "sounds")
    meta_dir = os.path.join(OUT_DIR, "meta")
    for d in (lin_dir, dec_dir, snd_dir, meta_dir):
        os.makedirs(d, exist_ok=True)

    iso_entries = parse_iso_list()
    wanted_iso = [("default/dataxb/lmaps/common.lin", lin_dir, dec_dir)]
    for m in COOP_MAPS:
        wanted_iso.append((f"default/dataxb/lmaps/{m}/{m}.lin", lin_dir, dec_dir))

    # Also grab coop-specific .uax sound files from Default/DataXb/sounds/
    for key, (sz, off, orig_path) in iso_entries.items():
        if key.startswith("default/dataxb/sounds/") and ("coop" in key or "x01_" in key or "x02_" in key or "x03_" in key):
            if key.endswith(".uax"):
                wanted_iso.append((key, snd_dir, None))

    print(f"[*] Extracting {len(wanted_iso)} files from ISO...")
    with open(ISO_PATH, "rb") as iso:
        for key, raw_dest_dir, dec_dest_dir in wanted_iso:
            if key not in iso_entries:
                print(f"  [WARN] Missing in ISO list: {key}")
                continue
            sz, off, orig_path = iso_entries[key]
            base = os.path.basename(orig_path)
            raw_path = os.path.join(raw_dest_dir, base)
            iso.seek(off)
            raw = iso.read(sz)
            with open(raw_path, "wb") as out:
                out.write(raw)
            if dec_dest_dir is not None:
                dec = decompress_lin(raw)
                dec_path = os.path.join(dec_dest_dir, base + ".dec")
                with open(dec_path, "wb") as out:
                    out.write(dec)
                print(f"  [OK] {base:24s} raw={sz:10,d} -> dec={len(dec):10,d}")
            else:
                print(f"  [OK] {base:24s} raw={sz:10,d}")

    print("[*] Extracting co-op .ini / .occ / .int / .dds metadata from DynamicXbox.umd...")
    umd_items = extract_from_umd(meta_dir)
    print(f"  [OK] Extracted {len(umd_items)} files from DynamicXbox.umd into {meta_dir}")


if __name__ == "__main__":
    main()
