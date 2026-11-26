"""
audit_maps.py

Audits all 14 decompressed SCDA Xbox co-op .lin maps + common.lin against
Chaos Theory PC (System/*.u and System/dynamic-pc.umd) and writes
docs/scda_coop_port/02_coop_maps_inventory.md.
"""

import glob
import os
import struct
from collections import Counter, defaultdict

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEC_DIR = os.path.join(REPO_ROOT, "SCDA_Coop_Maps", "extracted", "lin_dec")
OUT_MD = os.path.join(REPO_ROOT, "docs", "scda_coop_port", "02_coop_maps_inventory.md")

NATIVE_CLASSES = {
    "class", "package", "level", "model", "polys", "staticmesh",
    "staticmeshinstance", "emitterinstancer", "hkleveldata", "enavmesh",
    "textbuffer", "struct", "function", "state", "const", "enum",
    "intproperty", "boolproperty", "floatproperty", "nameproperty",
    "strproperty", "objectproperty", "classproperty", "structproperty",
    "arrayproperty", "byteproperty", "palette", "texture", "cubemap",
    "shader", "combiner", "finalblend", "texenvmap", "texoscillator",
    "texpanner", "texrotator", "texscaler", "colorModifier", "colormodifier",
    "fadeColor", "fadecolor", "material", "sound", "skeletalmesh", "meshanimation",
    "vertexmesh", "lodmesh",
}


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


def get_ct_classes():
    classes = set()
    pkg_names = set()
    for u_path in glob.glob(os.path.join(REPO_ROOT, "System", "*.u")):
        pkg_names.add(os.path.splitext(os.path.basename(u_path))[0].lower())
        with open(u_path, "rb") as f:
            d = f.read()
        tag, ver, lic, flags, unk, ncnt, noff, ecnt, eoff, icnt, ioff = struct.unpack_from("<IHHIIIIIIII", d, 0)
        p = noff
        names = []
        for _ in range(ncnt):
            slen, p = read_ci(d, p)
            if slen > 0:
                s = d[p:p + slen - 1].decode("latin1", errors="replace")
                p += slen
                if d[p - 1] != 0 and d[p] == 0:
                    s = d[p - slen:p].decode("latin1", errors="replace")
                    p += 1
            else:
                s = ""
            p += 4
            names.append(s)
        p = eoff
        for _ in range(ecnt):
            cls, p = read_ci(d, p)
            sup, p = read_ci(d, p)
            p += 4
            oname, p = read_ci(d, p)
            p += 4
            ssize, p = read_ci(d, p)
            if ssize > 0:
                _, p = read_ci(d, p)
            if cls == 0:
                classes.add(names[oname].lower())
    return classes, pkg_names


def get_ct_umd_packages():
    umd_path = os.path.join(REPO_ROOT, "System", "dynamic-pc.umd")
    pkgs = set()
    with open(umd_path, "rb") as f:
        f.seek(-20, 2)
        magic, dir_off, size, flags = struct.unpack("<IIII", f.read(16))
        f.seek(dir_off)
        b0 = f.read(1)[0]
        cnt = b0 & 0x3F
        if b0 & 0x40:
            shift = 6
            while True:
                b = f.read(1)[0]
                cnt |= (b & 0x7F) << shift
                shift += 7
                if not (b & 0x80):
                    break
        for _ in range(cnt):
            b0 = f.read(1)[0]
            slen = b0 & 0x3F
            if b0 & 0x40:
                b = f.read(1)[0]
                slen |= (b & 0x7F) << 6
            name = f.read(slen)[:-1].decode("latin1", errors="replace")
            f.read(12)
            base = os.path.basename(name)
            stem, ext = os.path.splitext(base)
            if ext.lower() in (".utx", ".usx", ".ukx", ".uax", ".umx", ".u"):
                pkgs.add(stem.lower())
    for folder in ("Textures", "StaticMeshes", "Animations", "Sounds", "Music"):
        for fp in glob.glob(os.path.join(REPO_ROOT, "Data", folder, "*")):
            pkgs.add(os.path.splitext(os.path.basename(fp))[0].lower())
    return pkgs


def parse_lin_header_and_packages(d):
    pools = struct.unpack_from("<5I", d, 0)
    checksum = struct.unpack_from("<I", d, 20)[0]
    p = 24
    # First package start
    first_tag = d.find(b"\xc1\x83\x2a\x9e")
    # Walk contiguous package headers starting at first_tag
    pos = first_tag
    pkgs = []
    while pos + 44 <= len(d) and d[pos:pos + 4] == b"\xc1\x83\x2a\x9e":
        ver, lic, flags, unk, ncnt, noff, ecnt, eoff, icnt, ioff = struct.unpack_from("<HHIIIIIIII", d, pos + 4)
        cur = pos + noff
        names = []
        for _ in range(ncnt):
            slen, cur = read_ci(d, cur)
            if slen > 0:
                s = d[cur:cur + slen - 1].decode("latin1", errors="replace")
                cur += slen
                if d[cur - 1] != 0 and d[cur] == 0:
                    s = d[cur - slen:cur].decode("latin1", errors="replace")
                    cur += 1
            else:
                s = ""
            cur += 4
            names.append(s)
        imports = []
        for _ in range(icnt):
            cpkg, cur = read_ci(d, cur)
            cname, cur = read_ci(d, cur)
            outer = struct.unpack_from("<i", d, cur)[0]
            cur += 4
            oname, cur = read_ci(d, cur)
            imports.append((
                names[cpkg] if 0 <= cpkg < ncnt else "",
                names[cname] if 0 <= cname < ncnt else "",
                outer,
                names[oname] if 0 <= oname < ncnt else "",
            ))
        exports = []
        for _ in range(ecnt):
            cls, cur = read_ci(d, cur)
            sup, cur = read_ci(d, cur)
            outer = struct.unpack_from("<i", d, cur)[0]
            cur += 4
            oname, cur = read_ci(d, cur)
            oflags = struct.unpack_from("<I", d, cur)[0]
            cur += 4
            ssize, cur = read_ci(d, cur)
            soff = 0
            if ssize > 0:
                soff, cur = read_ci(d, cur)
            exports.append((cls, sup, outer, names[oname] if 0 <= oname < ncnt else "", oflags, ssize, soff))
        pkgs.append({
            "offset": pos,
            "ver": ver,
            "lic": lic,
            "flags": flags,
            "names": names,
            "imports": imports,
            "exports": exports,
        })
        pos = cur
    return {
        "pools": pools,
        "checksum": checksum,
        "first_pkg_off": first_tag,
        "stream_off": pos,
        "stream_size": len(d) - pos,
        "total_size": len(d),
        "packages": pkgs,
    }


def main():
    ct_classes, ct_u_pkgs = get_ct_classes()
    ct_asset_pkgs = get_ct_umd_packages() | ct_u_pkgs

    lin_files = sorted(glob.glob(os.path.join(DEC_DIR, "*.lin.dec")))
    results = []
    all_missing_classes = defaultdict(set)
    all_npcs_by_map = {}

    for path in lin_files:
        name = os.path.basename(path).replace(".lin.dec", "")
        with open(path, "rb") as f:
            d = f.read()
        info = parse_lin_header_and_packages(d)
        pkgs = info["packages"]

        lics = Counter(p["lic"] for p in pkgs)
        total_exports = sum(len(p["exports"]) for p in pkgs)
        total_serial = sum(e[5] for p in pkgs for e in p["exports"])

        # Collect imported packages & classes across pkg[0] (and all packages)
        p0 = pkgs[0]
        p0_imp_pkgs = sorted({imp[3] for imp in p0["imports"] if imp[1].lower() == "package"})
        p0_imp_classes = sorted({imp[3] for imp in p0["imports"] if imp[1].lower() == "class"})

        missing_scda = []
        pattern_scripts = []
        for c in p0_imp_classes:
            clo = c.lower()
            if clo in ct_classes or clo in NATIVE_CLASSES:
                continue
            if clo.startswith("p_"):
                pattern_scripts.append(c)
            else:
                missing_scda.append(c)
                all_missing_classes[c].add(name)

        # Also check classes imported across ALL packages in this .lin
        all_imp_classes = sorted({imp[3] for pkg in pkgs for imp in pkg["imports"] if imp[1].lower() == "class"})
        for c in all_imp_classes:
            clo = c.lower()
            if clo not in ct_classes and clo not in NATIVE_CLASSES and not clo.startswith("p_"):
                all_missing_classes[c].add(name)

        # Count NPCs, lights, interactive objects in pkg[0]
        def resolve_cls(cls_idx):
            if cls_idx < 0:
                return p0["imports"][-cls_idx - 1][3]
            if cls_idx > 0:
                return p0["exports"][cls_idx - 1][3]
            return "Class"

        cls_counts = Counter(resolve_cls(e[0]) for e in p0["exports"])
        npc_count = sum(cnt for cname, cnt in cls_counts.items()
                        if any(k in cname.lower() for k in ("soldier", "guard", "civilian", "npc", "terrorist", "pawn", "dog", "sam")))
        light_count = sum(cnt for cname, cnt in cls_counts.items() if "light" in cname.lower())
        all_npcs_by_map[name] = {k: v for k, v in cls_counts.items()
                                 if any(w in k.lower() for w in ("soldier", "guard", "civilian", "npc", "terrorist", "pawn", "patrol", "navmesh", "light", "camera", "turret", "door", "card"))}

        in_ct_pkgs = [p for p in p0_imp_pkgs if p.lower() in ct_asset_pkgs]
        scda_only_pkgs = [p for p in p0_imp_pkgs if p.lower() not in ct_asset_pkgs]

        results.append({
            "name": name,
            "total_size": info["total_size"],
            "stream_off": info["stream_off"],
            "stream_size": info["stream_size"],
            "num_pkgs": len(pkgs),
            "lics": dict(lics),
            "total_exports": total_exports,
            "total_serial": total_serial,
            "p0_exports": len(p0["exports"]),
            "p0_imp_pkgs": p0_imp_pkgs,
            "in_ct_pkgs": in_ct_pkgs,
            "scda_only_pkgs": scda_only_pkgs,
            "missing_scda_classes": missing_scda,
            "pattern_scripts": pattern_scripts,
            "npc_count": npc_count,
            "light_count": light_count,
        })
        print(f"Audited {name:18s}: pkgs={len(pkgs):3d}, stream={info['stream_size']/1e6:5.2f}MB, NPCs={npc_count:3d}, Lights={light_count:3d}, missing={missing_scda}")

    # Write Markdown report
    lines = [
        "# SCDA Xbox Co-op Maps Inventory & Compatibility Audit",
        "",
        f"Generated against Chaos Theory PC (`{len(ct_classes)}` UClasses in `System/*.u`, `{len(ct_asset_pkgs)}` known asset/code packages).",
        "",
        "## 1. Per-Map Summary",
        "",
        "| Map | Decomp Size | Stream Size | Pkgs | Lic Versions | `pkg[0]` Exports | NPCs/Pawns | Lights | Patterns | Missing SCDA Classes |",
        "| --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for r in results:
        lic_str = ", ".join(f"{k}:{v}" for k, v in sorted(r["lics"].items()))
        miss_str = ", ".join(f"`{c}`" for c in r["missing_scda_classes"]) or "None"
        lines.append(
            f"| `{r['name']}` | {r['total_size']:,} | {r['stream_size']:,} | {r['num_pkgs']} | {lic_str} | "
            f"{r['p0_exports']} | {r['npc_count']} | {r['light_count']} | {len(r['pattern_scripts'])} | {miss_str} |"
        )

    lines.extend([
        "",
        "## 2. All SCDA-Only Classes Across All 14 Co-op Maps",
        "",
        "These classes are imported inside the SCDA `.lin` files and do not exist in Chaos Theory's `System/*.u` (excluding map-specific `p_*` `EchelonPattern` classes, which are embedded inside `EchelonPattern` in each `.lin`):",
        "",
        "| Class Name | Maps Referencing It |",
        "| --- | --- |",
    ])
    for cname, mset in sorted(all_missing_classes.items()):
        lines.append(f"| `{cname}` | {', '.join(f'`{m}`' for m in sorted(mset))} |")

    lines.extend([
        "",
        "## 3. Imported Packages Breakdown (`pkg[0]`)",
        "",
    ])
    for r in results:
        if r["name"] == "common":
            continue
        lines.append(f"### `{r['name']}`")
        lines.append(f"- **Pattern scripts ({len(r['pattern_scripts'])})**: {', '.join(f'`{p}`' for p in r['pattern_scripts']) or 'None'}")
        lines.append(f"- **Packages shared by name with SCCT ({len(r['in_ct_pkgs'])})**: {', '.join(f'`{p}`' for p in r['in_ct_pkgs'])}")
        lines.append(f"- **SCDA-only / custom packages ({len(r['scda_only_pkgs'])})**: {', '.join(f'`{p}`' for p in r['scda_only_pkgs'])}")
        npcs = all_npcs_by_map[r["name"]]
        if npcs:
            top_items = ", ".join(f"`{k}` x{v}" for k, v in sorted(npcs.items()))
            lines.append(f"- **Key gameplay/AI/light exports**: {top_items}")
        lines.append("")

    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\n[OK] Wrote {OUT_MD}")


if __name__ == "__main__":
    main()
