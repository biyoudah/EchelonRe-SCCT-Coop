"""
reconstruct_packages.py

Reconstructs standalone UE2.5 SCDA packages (.scl, .utx, .usx, .ukx, .uax, .u)
from a runtime dump produced by lin_dumper.dll + decompressed .lin archives.

Usage:
    python tools/scda_port/reconstruct_packages.py x01_coop_ice_a
"""

import collections
import glob
import json
import os
import struct
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEC_DIR = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "extracted", "lin_dec")
DUMP_DIR = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "dumped")
OUT_ROOT = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "reconstructed")


def read_ci(d, p):
    b0 = d[p]
    p += 1
    neg = (b0 & 0x80) != 0
    val = b0 & 0x3F
    if b0 & 0x40:
        b1 = d[p]
        p += 1
        val |= (b1 & 0x7F) << 6
        if b1 & 0x80:
            b2 = d[p]
            p += 1
            val |= (b2 & 0x7F) << 13
            if b2 & 0x80:
                b3 = d[p]
                p += 1
                val |= (b3 & 0x7F) << 20
                if b3 & 0x80:
                    b4 = d[p]
                    p += 1
                    val |= (b4 & 0x1F) << 27
    return (-val if neg else val), p


def write_ci(val):
    """Encodes a signed 32-bit integer as UE2 FCompactIndex."""
    neg = val < 0
    v = -val if neg else val
    b0 = (0x80 if neg else 0) | (v & 0x3F)
    v >>= 6
    if not v:
        return bytes([b0])
    out = [b0 | 0x40]
    while v:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
    return bytes(out)


def _verify_ci_codec():
    for v in (0, 1, -1, 63, -63, 64, -64, 8191, -8191, 8192, 1048575, 134217728, -123456):
        enc = write_ci(v)
        dec, p = read_ci(enc, 0)
        assert dec == v and p == len(enc), f"CI round-trip failed for {v}: {enc.hex()} -> {dec}"


def scan_lin_packages(buf):
    """Scans and parses all contiguous/embedded UE2.5 package headers in a .lin.dec buffer."""
    pkgs = {}
    pos = 0
    while True:
        pos = buf.find(b"\xc1\x83\x2a\x9e", pos)
        if pos == -1:
            break
        if pos + 40 <= len(buf):
            ver, lic, flags, unk, ncnt, noff, ecnt, eoff, icnt, ioff = struct.unpack_from(
                "<HHIIIIIIII", buf, pos + 4
            )
            if (
                ver == 100
                and 100 <= lic <= 130
                and 40 <= noff < 512
                and 0 < ncnt < 100000
                and ecnt < 100000
                and icnt < 100000
            ):
                cur = pos + noff
                names_start = cur
                names = []
                ok = True
                for _ in range(ncnt):
                    if cur >= len(buf):
                        ok = False
                        break
                    slen, cur = read_ci(buf, cur)
                    if slen < 0 or cur + abs(slen) + 4 > len(buf):
                        ok = False
                        break
                    if slen > 0:
                        if cur + slen < len(buf) and buf[cur + slen - 1] != 0 and buf[cur + slen] == 0:
                            # Echelon UE2.5: slen is strlen, followed by trailing NUL byte
                            s = buf[cur : cur + slen].decode("latin1", errors="replace")
                            cur += slen + 1
                        else:
                            # Standard UE2: slen includes trailing NUL byte
                            s = buf[cur : cur + slen - 1].decode("latin1", errors="replace")
                            cur += slen
                    else:
                        s = ""
                    cur += 4
                    names.append(s)
                if not ok:
                    pos += 4
                    continue
                names_end = cur

                imports_start = cur
                imports = []
                for _ in range(icnt):
                    cpkg, cur = read_ci(buf, cur)
                    cname, cur = read_ci(buf, cur)
                    outer = struct.unpack_from("<i", buf, cur)[0]
                    cur += 4
                    oname, cur = read_ci(buf, cur)
                    imports.append((
                        names[cpkg] if 0 <= cpkg < ncnt else "",
                        names[cname] if 0 <= cname < ncnt else "",
                        outer,
                        names[oname] if 0 <= oname < ncnt else "",
                    ))
                imports_end = cur

                exports_start = cur
                raw_exports = []
                for _ in range(ecnt):
                    cls, cur = read_ci(buf, cur)
                    sup, cur = read_ci(buf, cur)
                    outer = struct.unpack_from("<i", buf, cur)[0]
                    cur += 4
                    oname, cur = read_ci(buf, cur)
                    oflags = struct.unpack_from("<I", buf, cur)[0]
                    cur += 4
                    ssize, cur = read_ci(buf, cur)
                    soff = 0
                    if ssize > 0:
                        soff, cur = read_ci(buf, cur)
                    raw_exports.append((cls, sup, outer, oname, oflags, ssize, soff))
                exports_end = cur
                exports = []
                for cls, sup, outer, oname, oflags, ssize, soff in raw_exports:
                    if cls < 0 and -cls - 1 < len(imports):
                        cname = imports[-cls - 1][3]
                    elif 0 < cls <= len(raw_exports):
                        cname = names[raw_exports[cls - 1][3]] if 0 <= raw_exports[cls - 1][3] < ncnt else ""
                    else:
                        cname = "Class"
                    oname_s = names[oname] if 0 <= oname < ncnt else ""
                    exports.append((cls, sup, outer, oname, oflags, ssize, soff, oname_s, cname))

                pkgs[pos] = {
                    "offset": pos,
                    "end": exports_end,
                    "ver": ver,
                    "lic": lic,
                    "flags": flags,
                    "unk": unk,
                    "ncnt": ncnt,
                    "noff": noff,
                    "ecnt": ecnt,
                    "icnt": icnt,
                    "hdr_bytes": buf[pos : pos + noff],
                    "name_bytes": buf[names_start:names_end],
                    "import_bytes": buf[imports_start:imports_end],
                    "names": names,
                    "imports": imports,
                    "exports": exports,
                }
                pos = exports_end
                continue
        pos += 4
    return pkgs


def get_lin_ctor_size(buf):
    """Returns exact byte count consumed by FArchiveLinear ctor (5 pools + checksum + MapName FString)."""
    # 5 uint32 pool sizes (20B) + uint32 checksum (4B) + FString MapName (compact index + chars)
    slen, p = read_ci(buf, 24)
    if slen > 0:
        p += slen
    elif slen < 0:
        p += (-slen) * 2
    return p


def extract_havok_block(buf, ctor_len):
    """Extracts the prepended Havok hkWorld block from a map .lin.dec buffer."""
    if buf[ctor_len : ctor_len + 4] != b"\x0f\xf0\xad\xde":
        raise ValueError(f"Expected 0xDEAD0FF0 Havok header at {ctor_len:#x}, got {buf[ctor_len:ctor_len+8].hex()}")
    magic, hk_ver, hk_size = struct.unpack_from("<III", buf, ctor_len)
    payload = buf[ctor_len + 12 : ctor_len + 12 + hk_size]
    if len(payload) != hk_size:
        raise ValueError(f"Truncated Havok payload: expected {hk_size}, got {len(payload)}")
    return payload, hk_ver


def parse_dump_events(evt_path):
    """Parses dump_events.bin, returning enter-offsets, linkers, and first-invocation captured export bytes."""
    with open(evt_path, "rb") as f:
        data = f.read()

    i = 0
    enters = {}
    linkers = {}
    root_names = {}
    stack = []
    captured = {}  # (linker_va, exp_idx) -> bytes (first non-empty Preload invocation)
    stats = collections.Counter()

    while i < len(data):
        tag = chr(data[i])
        i += 1
        stats[tag] += 1
        if tag == "A":
            _, _, _, _, _, _, mlen = struct.unpack_from("<7I", data, i)
            i += 28 + mlen
        elif tag == "E":
            lva, ce, te = struct.unpack_from("<3I", data, i)
            i += 12
            enters[lva] = (ce, te)
        elif tag == "L":
            lva, nc, ec, ic, lic, flen = struct.unpack_from("<6I", data, i)
            i += 24
            fn = data[i : i + flen].decode("utf-8", errors="replace")
            i += flen
            linkers[lva] = (fn, nc, ec, ic, lic)
        elif tag == "N":
            lva, plen = struct.unpack_from("<2I", data, i)
            i += 8
            root_names[lva] = data[i : i + plen].decode("utf-8", errors="replace")
            i += plen
        elif tag == "P":
            ent, lva, eidx, ova, ofl, tell = struct.unpack_from("<BIi3I", data, i)
            i += 21
            if ent == 1:
                stack.append([lva, eidx, bytearray()])
            elif stack:
                fr = stack.pop()
                if fr[2] and (fr[0], fr[1]) not in captured:
                    captured[(fr[0], fr[1])] = bytes(fr[2])
        elif tag == "S":
            ar, tell, slen, al, aidx = struct.unpack_from("<4Ii", data, i)
            i += 20
            chunk = data[i : i + slen]
            i += slen
            if stack and al == stack[-1][0] and aidx == stack[-1][1]:
                stack[-1][2] += chunk
        else:
            raise ValueError(f"Unknown record tag {tag!r} at offset {i - 1}")

    return enters, linkers, root_names, captured, stats


def resolve_package_names(all_entries, map_name):
    """
    Recovers true Unreal package names for all entries via bipartite matching on
    the ImportTable dependency graph across all loaded packages.
    all_entries: list of (src, pkg_dict)
    """
    # Collect required (obj_name.lower(), cls_name.lower()) per root Package name
    # Preserve canonical mixed-case spelling from first occurrence
    canon_case = {}
    reqs = collections.defaultdict(set)
    for _, pk in all_entries:
        imps = pk["imports"]
        for cpkg, cname, outer, oname in imps:
            cur = outer
            while cur < 0:
                pimp = imps[-cur - 1]
                if pimp[2] == 0 and pimp[1].lower() == "package":
                    key = pimp[3].lower()
                    canon_case.setdefault(key, pimp[3])
                    reqs[key].add((oname.lower(), cname.lower()))
                    break
                cur = pimp[2]

    exp_sets = [set((e[7].lower(), e[8].lower()) for e in pk["exports"]) for _, pk in all_entries]
    matched = {}
    used_names = set()

    # Map pkg[0] is always <map_name>
    for idx, (src, pk) in enumerate(all_entries):
        if src == "map" and pk["offset"] == min(p["offset"] for s, p in all_entries if s == "map"):
            matched[idx] = map_name
            used_names.add(map_name.lower())

    # Iterative elimination: match packages whose subset candidates are unique among unused names
    changed = True
    while changed:
        changed = False
        for idx, (src, pk) in enumerate(all_entries):
            if idx in matched:
                continue
            es = exp_sets[idx]
            cands = [
                r for r, need in reqs.items()
                if r not in used_names and need.issubset(es)
            ]
            if len(cands) == 1:
                matched[idx] = canon_case[cands[0]]
                used_names.add(cands[0])
                changed = True

    # Fallback: match Core (highest overlap with 'core' if still unused) and remaining unique top exports
    for idx, (src, pk) in enumerate(all_entries):
        if idx in matched:
            continue
        es = exp_sets[idx]
        unused = [r for r in reqs if r not in used_names]
        if unused:
            scored = sorted(
                ((len(reqs[r] & es) / max(1, len(reqs[r])), len(reqs[r] & es), r) for r in unused),
                reverse=True,
            )
            if scored[0][1] > 0:
                best_r = scored[0][2]
                matched[idx] = canon_case[best_r]
                used_names.add(best_r)
                continue
        # Completely unimported package (e.g. SCMagmaInteraction): infer from first export name
        if any("magma" in e[7].lower() for e in pk["exports"][:5]):
            matched[idx] = "SCMagmaInteraction"
        else:
            matched[idx] = f"Unresolved_{src}_{pk['offset']:x}"

    return matched


def guess_extension(src, is_map_root, pk):
    if is_map_root:
        return ".scl"
    cls_set = {e[8].lower() for e in pk["exports"]}
    if any(c in cls_set for c in ("class", "struct", "function", "enum", "state")):
        return ".u"
    if "skeletalmesh" in cls_set or "meshanimation" in cls_set:
        return ".ukx"
    if "staticmesh" in cls_set:
        return ".usx"
    if "sound" in cls_set:
        return ".uax"
    return ".utx"


def build_standalone_package(pk, lva, captured):
    """
    Builds a standalone UE2.5 package with standard layout:
      [Header (0..NameOffset)] + [NameTable] + [ExportPayloads] + [ImportTable] + [ExportTable]
    Sets SerialSize=0 on uncaptured exports and recalculates SerialOffset/ImportOffset/ExportOffset.
    """
    hdr = bytearray(pk["hdr_bytes"])
    name_tbl = pk["name_bytes"]
    import_tbl = pk["import_bytes"]

    payload_base = len(hdr) + len(name_tbl)
    payload_buf = bytearray()
    exp_entries = bytearray()
    captured_cnt = 0
    exact_cnt = 0

    for eidx, exp in enumerate(pk["exports"]):
        cls, sup, outer, oname_idx, oflags, decl_ssize, _, _, _ = exp
        payload = captured.get((lva, eidx))
        if payload:
            captured_cnt += 1
            if len(payload) == decl_ssize:
                exact_cnt += 1
            ssize = len(payload)
            soff = payload_base + len(payload_buf)
            payload_buf += payload
        else:
            ssize = 0
            soff = 0

        exp_entries += write_ci(cls)
        exp_entries += write_ci(sup)
        exp_entries += struct.pack("<i", outer)
        exp_entries += write_ci(oname_idx)
        exp_entries += struct.pack("<I", oflags)
        exp_entries += write_ci(ssize)
        if ssize > 0:
            exp_entries += write_ci(soff)

    new_import_off = payload_base + len(payload_buf)
    new_export_off = new_import_off + len(import_tbl)

    # Patch ImportOffset (+0x24) and ExportOffset (+0x1C); NameOffset (+0x14) unchanged
    struct.pack_into("<I", hdr, 0x1C, new_export_off)
    struct.pack_into("<I", hdr, 0x24, new_import_off)

    out_bytes = bytes(hdr) + name_tbl + bytes(payload_buf) + import_tbl + bytes(exp_entries)
    return out_bytes, captured_cnt, exact_cnt


def get_ct_known_packages():
    import audit_maps
    return audit_maps.get_ct_umd_packages() | audit_maps.get_ct_classes()[1]


def reconstruct_map(map_name):
    _verify_ci_codec()

    com_path = os.path.join(DEC_DIR, "common.lin.dec")
    map_path = os.path.join(DEC_DIR, f"{map_name}.lin.dec")
    evt_path = os.path.join(DUMP_DIR, map_name, "dump_events.bin")
    out_dir = os.path.join(OUT_ROOT, map_name)
    map_pkgs_dir = os.path.join(out_dir, "map_pkgs")
    com_pkgs_dir = os.path.join(out_dir, "common_pkgs")
    os.makedirs(map_pkgs_dir, exist_ok=True)
    os.makedirs(com_pkgs_dir, exist_ok=True)

    print(f"[*] Loading {com_path}, {map_path}, and {evt_path}...")
    with open(com_path, "rb") as f:
        com_buf = f.read()
    with open(map_path, "rb") as f:
        map_buf = f.read()

    com_ctor_len = get_lin_ctor_size(com_buf)
    map_ctor_len = get_lin_ctor_size(map_buf)
    hk_payload, hk_ver = extract_havok_block(map_buf, map_ctor_len)

    hk_out_path = os.path.join(out_dir, f"{map_name}_havok.bin")
    with open(hk_out_path, "wb") as f:
        f.write(hk_payload)
    print(f"[OK] Saved Havok hkWorld block ({len(hk_payload):,} bytes, ver={hk_ver}) -> {hk_out_path}")

    com_pkgs = scan_lin_packages(com_buf)
    map_pkgs = scan_lin_packages(map_buf)
    enters, linkers, root_names, captured, stats = parse_dump_events(evt_path)
    print(f"[*] Scanned headers: common={len(com_pkgs)}, {map_name}={len(map_pkgs)}; dump linkers={len(linkers)}")

    # Match every linker_va via enter-cursor
    matched_entries = []  # (lva, src, pk)
    unmatched = []
    for lva, (fn, nc, ec, ic, lic) in linkers.items():
        if lva not in enters:
            unmatched.append((lva, fn, "missing_E"))
            continue
        ce, te = enters[lva]
        if te == 0:
            off = com_ctor_len + ce
            pk = com_pkgs.get(off)
            src = "common"
        else:
            off = map_ctor_len + te
            pk = map_pkgs.get(off)
            src = "map"
        if not pk or (pk["ncnt"], pk["ecnt"], pk["icnt"]) != (nc, ec, ic):
            unmatched.append((lva, fn, f"mismatch src={src} off={off:#x}"))
            continue
        matched_entries.append((lva, src, pk))

    if unmatched:
        raise RuntimeError(f"Failed to match {len(unmatched)} linkers: {unmatched[:5]}")
    print(f"[OK] Matched all {len(matched_entries)} linkers 1-to-1 via enter-cursor!")

    pkg_names = resolve_package_names([(src, pk) for _, src, pk in matched_entries], map_name)
    ct_known = get_ct_known_packages()

    min_map_off = min(pk["offset"] for _, src, pk in matched_entries if src == "map")
    manifest = {
        "map_name": map_name,
        "havok_file": os.path.basename(hk_out_path),
        "havok_bytes": len(hk_payload),
        "havok_version": hk_ver,
        "packages": [],
    }

    total_cap = 0
    total_exact = 0
    for idx, (lva, src, pk) in enumerate(matched_entries):
        name = pkg_names[idx]
        is_root = src == "map" and pk["offset"] == min_map_off
        ext = guess_extension(src, is_root, pk)
        out_bytes, cap_cnt, exact_cnt = build_standalone_package(pk, lva, captured)
        total_cap += cap_cnt
        total_exact += exact_cnt

        sub_dir = map_pkgs_dir if src == "map" else com_pkgs_dir
        out_file = os.path.join(sub_dir, f"{name}{ext}")
        with open(out_file, "wb") as f:
            f.write(out_bytes)

        manifest["packages"].append({
            "source": src,
            "name": name,
            "ext": ext,
            "filename": f"{name}{ext}",
            "rel_path": os.path.relpath(out_file, out_dir).replace("\\", "/"),
            "lic_ver": pk["lic"],
            "names": pk["ncnt"],
            "exports": pk["ecnt"],
            "imports": pk["icnt"],
            "captured_exports": cap_cnt,
            "exact_size_matches": exact_cnt,
            "size_bytes": len(out_bytes),
            "in_scct_pc": name.lower() in ct_known,
            "linker_va": f"0x{lva:08X}",
            "lin_offset": f"0x{pk['offset']:06X}",
        })

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(
        f"[OK] Reconstructed {len(matched_entries)} packages -> {out_dir}\n"
        f"     Captured exports: {total_cap} (exact size matches: {total_exact}, diffs: {total_cap - total_exact})\n"
        f"     Manifest: {manifest_path}"
    )


if __name__ == "__main__":
    m = sys.argv[1] if len(sys.argv) > 1 else "x01_coop_ice_a"
    reconstruct_map(m)
