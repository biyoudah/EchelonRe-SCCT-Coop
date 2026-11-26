"""Convert reconstructed SCDA Xbox packages (LicVer=127) to SCCT PC (LicVer=124)."""

import argparse
import json
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import reconstruct_packages as rp
import mesh_convert as mc
import convert_loading

# SCDA-only classes mapped to closest retail SCCT PC equivalents
SCDA_CLASS_MAP = {
    ("echelongameobject", "esnowball"): ("echelongameobject", "ebottlewine"),
    ("echeloningredient", "ecardreader"): ("echeloningredient", "eswitchobject"),
    ("echeloningredient", "ekeycard"): ("echeloningredient", "egameplayobjectpickup"),
    ("echelon", "esavecheckpointvolume"): ("echelon", "evolume"),
    ("engine", "emitterinstancer"): ("engine", "emitter"),
    ("engine", "scx_fingerprint_material"): ("engine", "scx_basic_material"),
    ("engine", "scx_heatdistortion_material"): ("engine", "scx_eev_material"),
    ("engine", "scx_refractiveglass_material"): ("engine", "scx_glass_material"),
    ("echelon", "eleggrabinteraction"): ("echelon", "ewindowgrabinteraction"),
    ("echelon", "eloopmover"): ("echelon", "etrapmover"),
    ("echelonai", "soldierfriendly"): ("echelonai", "basesoldier"),
    ("echeloningredient", "eberetta92fsw"): ("echeloningredient", "epya"),
    ("echeloningredient", "ecutwireinteraction"): ("echeloningredient", "ewirehackinteraction"),
    ("echeloningredient", "eradio"): ("echeloningredient", "eswitchobject"),
    ("echeloningredient", "esbcustom"): ("echeloningredient", "esbcustomactor"),
    ("echeloningredient", "esbrope"): ("echeloningredient", "esbropeactor"),
    ("echeloningredient", "esearchlight"): ("echeloningredient", "ealarmlight"),
    ("echeloningredient", "euavhelico"): ("echeloningredient", "ehelicopter"),
    ("engine", "ellipsoidactor"): ("engine", "actor"),
    ("engine", "esoftbody"): ("engine", "esoftbodyactor"),
    ("engine", "particlespawner"): ("engine", "emitter"),
    ("engine", "scx_ice_material"): ("engine", "scx_basic_material"),
    ("echeloneffect", "brk_explopropane"): ("echeloneffect", "xexplogazbottle"),
    ("echeloneffect", "xsnowball"): ("echeloneffect", "xem_bottlewatersmall"),
}

EXT_DEST_DIR = {
    ".scl": os.path.join("Data", "Maps"),
    ".utx": os.path.join("Data", "Textures"),
    ".usx": os.path.join("Data", "StaticMeshes"),
    ".ukx": os.path.join("Data", "Animations"),
    ".uax": os.path.join("Data", "Sounds"),
    ".u": "System",
}


class PackageData:
    def __init__(self, path=None, raw=None):
        if raw is None:
            with open(path, "rb") as f:
                raw = f.read()
        self.raw = raw
        tag, fv, lv, pf, unk = struct.unpack_from("<IHHII", raw, 0)
        if tag != 0x9E2A83C1:
            raise ValueError(f"Bad package tag {tag:#x} in {path}")
        self.fv = fv
        self.lv = lv
        self.pf = pf
        self.unk = unk
        has_ech_hdr = len(raw) >= 0x30 and struct.unpack_from("<I", raw, 0x28)[0] == 0x0FF0ADDE
        off_tbl = 16 if has_ech_hdr else 12
        nc, no, ec, eo, ic, io = struct.unpack_from("<6I", raw, off_tbl)

        self.names = []
        pos = no
        for _ in range(nc):
            sl, pos = rp.read_ci(raw, pos)
            if sl < 0:
                s = raw[pos : pos + (-sl) * 2].decode("utf-16-le", "replace").rstrip("\x00")
                pos += (-sl) * 2 + (4 if not has_ech_hdr else 6)
            elif not has_ech_hdr:
                s = raw[pos : pos + sl].decode("latin1", "replace").rstrip("\x00")
                pos += sl + 4
            else:
                s = raw[pos : pos + sl].decode("latin1", "replace")
                pos += sl + 5
            fl = struct.unpack_from("<I", raw, pos - 4)[0]
            self.names.append([s, fl])

        if has_ech_hdr:
            p_hdr = 0x2C
            csl, p_hdr = rp.read_ci(raw, p_hdr)
            if csl > 0:
                p_hdr += csl
            elif csl < 0:
                p_hdr += (-csl) * 2
            self.guid = raw[p_hdr : p_hdr + 16]
            p_hdr += 16
            gcnt, = struct.unpack_from("<I", raw, p_hdr)
            p_hdr += 4
            self.generations = list(struct.unpack_from(f"<{gcnt * 2}I", raw, p_hdr)) if gcnt <= 16 else []
        else:
            self.guid = raw[0x24:0x34]
            gcnt, = struct.unpack_from("<I", raw, 0x34)
            self.generations = list(struct.unpack_from(f"<{gcnt * 2}I", raw, 0x38)) if gcnt <= 16 else []

        self.imps = []
        pos = io
        for _ in range(ic):
            cp, pos = rp.read_ci(raw, pos)
            cc, pos = rp.read_ci(raw, pos)
            out, pos = struct.unpack_from("<i", raw, pos)[0], pos + 4
            nm, pos = rp.read_ci(raw, pos)
            self.imps.append({
                "cp": cp,
                "cc": cc,
                "out": out,
                "nm": nm,
            })

        self.exps = []
        pos = eo
        for idx in range(ec):
            cls, pos = rp.read_ci(raw, pos)
            sup, pos = rp.read_ci(raw, pos)
            out, pos = struct.unpack_from("<i", raw, pos)[0], pos + 4
            nm, pos = rp.read_ci(raw, pos)
            fl, pos = struct.unpack_from("<I", raw, pos)[0], pos + 4
            ssz, pos = rp.read_ci(raw, pos)
            soff = 0
            if ssz > 0:
                soff, pos = rp.read_ci(raw, pos)
            self.exps.append({
                "idx": idx + 1,
                "cls": cls,
                "sup": sup,
                "out": out,
                "nm": nm,
                "fl": fl,
                "ssz": ssz,
                "soff": soff,
                "data": raw[soff : soff + ssz] if ssz > 0 else b"",
            })

    def name_str(self, idx):
        return self.names[idx][0]

    def ensure_name(self, s, flags=0x00070010):
        low = s.lower()
        for i, (ns, _) in enumerate(self.names):
            if ns.lower() == low:
                return i
        self.names.append([s, flags])
        return len(self.names) - 1

    def exp_cls_name(self, e):
        c = e["cls"]
        if c == 0:
            return "Class"
        if c < 0:
            return self.name_str(self.imps[-c - 1]["nm"])
        return self.name_str(self.exps[c - 1]["nm"])

    def ref_path(self, r):
        parts = []
        seen = set()
        while r != 0 and r not in seen:
            seen.add(r)
            if r < 0:
                im = self.imps[-r - 1]
                parts.append(self.name_str(im["nm"]))
                r = im["out"]
            else:
                ex = self.exps[r - 1]
                parts.append(self.name_str(ex["nm"]))
                r = ex["out"]
        return ".".join(reversed(parts))

    def imp_root_pkg(self, i):
        r = -(i + 1)
        seen = set()
        last = None
        while r < 0 and r not in seen:
            seen.add(r)
            last = self.imps[-r - 1]
            r = last["out"]
        if last and last["out"] == 0 and self.name_str(last["cc"]).lower() == "package":
            return self.name_str(last["nm"])
        if last:
            return self.name_str(last["cp"])
        return ""


class RetailIndex:
    """Lazy index of retail SCCT PC exports across System/*.u, Data/, and dynamic-pc.umd."""

    def __init__(self):
        self.umd_path = os.path.join(REPO, "System", "dynamic-pc.umd")
        self.umd_entries = {}
        with open(self.umd_path, "rb") as f:
            f.seek(-20, 2)
            magic, doff, usz, _, _ = struct.unpack("<5I", f.read(20))
            if doff >= usz:
                raise ValueError(f"Bad UMD tail magic={magic:#x} doff={doff:#x} usz={usz:#x}")
            f.seek(doff)
            rd = f.read(usz - doff)
        pos = 0
        cnt, pos = rp.read_ci(rd, pos)
        for _ in range(cnt):
            sl, pos = rp.read_ci(rd, pos)
            nm = rd[pos : pos + sl - 1].decode("latin1", "replace")
            pos += sl
            off, sz, fl = struct.unpack_from("<3I", rd, pos)
            pos += 12
            self.umd_entries[os.path.basename(nm).lower()] = (nm, off, sz, fl)

        self.disk_pkgs = {}
        for root, _, files in os.walk(REPO):
            rel = os.path.relpath(root, REPO).lower()
            if rel.startswith("scda_coop_maps") or ".git" in rel or rel.startswith("versus"):
                continue
            for fn in files:
                low = fn.lower()
                if low.startswith("scda_"):
                    continue
                if low.endswith((".u", ".utx", ".usx", ".ukx", ".uax", ".scl")):
                    self.disk_pkgs[low] = os.path.join(root, fn)

        self._cache = {}

    def has_retail_pkg(self, root_lower):
        for ext in (".u", ".utx", ".usx", ".ukx", ".uax", ".scl"):
            k = root_lower + ext
            if k in self.disk_pkgs or k in self.umd_entries:
                return ext
        return None

    def _load_retail_pkg(self, root_lower):
        for ext in (".u", ".utx", ".usx", ".ukx", ".uax", ".scl"):
            k = root_lower + ext
            if k in self.disk_pkgs:
                return k, PackageData(path=self.disk_pkgs[k])
            if k in self.umd_entries:
                _, off, sz, _ = self.umd_entries[k]
                with open(self.umd_path, "rb") as f:
                    f.seek(off)
                    buf = f.read(sz)
                return "UMD:" + k, PackageData(raw=buf)
        return None, None

    def get_exports(self, root_lower):
        if root_lower in self._cache:
            return self._cache[root_lower]
        label, pk = self._load_retail_pkg(root_lower)
        if pk is None:
            self._cache[root_lower] = (None, set())
        else:
            s = {pk.ref_path(e["idx"]).lower() for e in pk.exps} | {pk.name_str(e["nm"]).lower() for e in pk.exps}
            self._cache[root_lower] = (label, s)
        return self._cache[root_lower]

    def get_export_classes(self, root_lower):
        if not hasattr(self, "_cls_cache"):
            self._cls_cache = {}
        if root_lower in self._cls_cache:
            return self._cls_cache[root_lower]
        _, pk = self._load_retail_pkg(root_lower)
        if pk is None:
            self._cls_cache[root_lower] = {}
        else:
            self._cls_cache[root_lower] = {
                pk.ref_path(e["idx"]).lower(): pk.exp_cls_name(e) for e in pk.exps
            }
        return self._cls_cache[root_lower]


def parse_prop_spans(names, data):
    """Parse UObject tagged properties returning (list_of_spans, prop_end_offset)."""
    p = 0
    spans = []
    while p < len(data):
        start = p
        ni, p = rp.read_ci(data, p)
        nm = names[ni][0]
        if nm == "None":
            return spans, p, start
        info = data[p]
        p += 1
        pt = info & 0x0F
        st = (info >> 4) & 7
        arr = (info & 0x80) != 0
        if pt == 10:
            _, p = rp.read_ci(data, p)
        if st < 5:
            sz = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}[st]
        elif st == 5:
            sz = data[p]
            p += 1
        elif st == 6:
            sz = struct.unpack_from("<H", data, p)[0]
            p += 2
        elif st == 7:
            sz = struct.unpack_from("<I", data, p)[0]
            p += 4
        if pt == 3:
            sz = 0
        elif arr:
            b0 = data[p]
            if (b0 & 0x80) == 0:
                p += 1
            elif (b0 & 0x40) == 0:
                p += 2
            else:
                p += 4
        p += sz
        spans.append((nm, start, p))
    raise ValueError("Missing None property terminator")


def _detect_mip_stride(d, p_after_mc, mc):
    """Detect Xbox/legacy FMipmap footer size: 7B (HHBBB), 6B (HHBB), or 10B (IIBB)."""
    for st in (7, 6, 10):
        q = p_after_mc
        ok = True
        for _ in range(mc):
            if q + 4 >= len(d):
                ok = False
                break
            dsz, q2 = rp.read_ci(d, q + 4)
            if dsz < 0 or q2 + dsz + st > len(d):
                ok = False
                break
            q = q2 + dsz + st
        if ok and q == len(d):
            return st
    return None


def convert_texture(pkg, exp):
    """Convert UTexture/UVolumeTexture FMipmap headers (uint16 -> uint32 USize/VSize)."""
    d = exp["data"]
    if not d:
        return b"", []
    _, prop_end, _ = parse_prop_spans(pkg.names, d)
    if prop_end == len(d):
        return d, []

    mc, p = rp.read_ci(d, prop_end)
    if mc < 0 or mc > 32:
        raise ValueError(f"Unexpected mip count {mc} in texture {pkg.name_str(exp['nm'])}")
    if mc == 0 or p == len(d):
        # Some shader/unloaded stubs (e.g. WrapLighting) end right after mc without mip bodies
        return bytes(d[:prop_end]) + rp.write_ci(0), []

    stride = _detect_mip_stride(d, p, mc)
    if stride is None:
        raise ValueError(f"Texture {pkg.name_str(exp['nm'])}: could not match FMipmap stride (mc={mc}, p={p}, len={len(d)})")

    out = bytearray(d[:prop_end])
    out += rp.write_ci(mc)
    skip_patches = []  # (offset_in_out, rel_end_after_data_array)
    for _ in range(mc):
        p += 4
        dsz, p = rp.read_ci(d, p)
        raw_mip = d[p : p + dsz]
        p += dsz
        if stride == 7:
            w, h, _unk_b, ub, vb = struct.unpack_from("<HHBBB", d, p)
        elif stride == 6:
            w, h, ub, vb = struct.unpack_from("<HHBB", d, p)
        else:
            w, h, ub, vb = struct.unpack_from("<IIBB", d, p)
        p += stride

        skip_pos = len(out)
        out += b"\x00\x00\x00\x00"
        out += rp.write_ci(dsz)
        out += raw_mip
        rel_after_data = len(out)
        skip_patches.append((skip_pos, rel_after_data))
        out += struct.pack("<IIBB", w, h, ub, vb)

    return bytes(out), skip_patches


def convert_static_mesh(pkg, exp):
    """Convert UStaticMesh from SCDA Lic=127 Xbox to SCCT Lic=124 PC."""
    d = exp["data"]
    if not d:
        return b"", []

    spans, prop_end, none_start = parse_prop_spans(pkg.names, d)
    out = bytearray()
    for nm, s, e in spans:
        if nm.lower() == "isoptimized":
            continue
        out += d[s:e]
    out += d[none_start:prop_end]

    p = prop_end
    # UPrimitive (FBox 25B + FSphere 16B = 41B)
    out += d[p : p + 41]
    p += 41

    # Strip XB 8B pre-stream header (packed_bools + [esi+0x16c])
    p += 8

    # VertexStream: ci vc + vc*28B + u32 rev
    vc, p2 = rp.read_ci(d, p)
    vs_end = p2 + vc * 28 + 4
    out += d[p:vs_end]
    p = vs_end

    # IndexBuffer (XB)
    ic, p = rp.read_ci(d, p)
    idxs = struct.unpack_from(f"<{ic}H", d, p) if ic > 0 else ()
    p += ic * 2
    irev, = struct.unpack_from("<I", d, p)
    p += 4

    # ColorStream flag & optional ColorStream (same position in PC after Wireframe+unk1d4)
    has_cs, = struct.unpack_from("<I", d, p)
    p += 4
    cs_bytes = b""
    if has_cs != 0:
        cs_start = p
        cc, p2 = rp.read_ci(d, p)
        cs_end = p2 + cc * 4 + 4
        cs_bytes = d[cs_start:cs_end]
        p = cs_end

    # XB Sections -> expand tristrip (f4==1) to trilist (f4==0)
    sc, p = rp.read_ci(d, p)
    pc_idxs = []
    pc_secs = bytearray()
    for _ in range(sc):
        f4, fi, mn, mx, np, ni, unk = struct.unpack_from("<IHHHHHI", d, p)
        p += 18
        mat, p = rp.read_ci(d, p)
        new_fi = len(pc_idxs)
        if f4 == 1:
            strip = idxs[fi : fi + ni + 2]
            tris = 0
            for k in range(len(strip) - 2):
                a, b, c = strip[k], strip[k + 1], strip[k + 2]
                if a == b or b == c or a == c:
                    continue
                if k & 1:
                    pc_idxs.extend((a, c, b))
                else:
                    pc_idxs.extend((a, b, c))
                tris += 1
            if tris != np:
                raise ValueError(f"StaticMesh {pkg.name_str(exp['nm'])} strip tri mismatch: {tris} != {np}")
        elif f4 == 0:
            sl = idxs[fi : fi + np * 3]
            tris = len(sl) // 3
            pc_idxs.extend(sl[: tris * 3])
        else:
            raise ValueError(f"Unknown Section f4={f4} in {pkg.name_str(exp['nm'])}")
        pc_secs += struct.pack("<IIHHHHHI", 0, 0, new_fi, mn, mx, tris, tris, unk)
        pc_secs += rp.write_ci(mat)

    # Emit PC VertexBuffer -> IndexBuffer -> WireframeIndexBuffer -> unk1d4 -> ColorStream -> Sections -> unk1d8
    out += rp.write_ci(len(pc_idxs))
    if pc_idxs:
        out += struct.pack(f"<{len(pc_idxs)}H", *pc_idxs)
    out += struct.pack("<I", irev)
    out += b"\x00\x00\x00\x00\x00"  # empty WireframeIndexBuffer (ci 0 + rev 0)
    out += struct.pack("<II", 0, has_cs)
    out += cs_bytes
    out += rp.write_ci(sc)
    out += pc_secs
    out += struct.pack("<I", 0)  # PC [ebp+0x1d8]

    # Shared post-Sections collision / kDOP body up to RawTriangles TLazyArray
    col_start = p
    ct, p = rp.read_ci(d, p)
    if ct != 0:
        raise ValueError(f"Unexpected non-zero CollisionTris ct={ct} in {pkg.name_str(exp['nm'])}")
    cv, p = rp.read_ci(d, p)
    p += cv * 12
    cm, p = rp.read_ci(d, p)
    for _ in range(cm):
        _, p = rp.read_ci(d, p)
    cb, p = rp.read_ci(d, p)
    p += cb + 4
    cn, p = rp.read_ci(d, p)
    for _ in range(cn):
        nw, p = rp.read_ci(d, p)
        p += nw * 2 + 25
    out += d[col_start:p]

    # PC [ebp+0x1dc] = 1
    out += struct.pack("<I", 1)

    # XB RawTriangles TLazyArray (4B SkipOffset only) -> PC TLazyArray (4B SkipOffset + 0x00)
    p += 4
    skip_pos = len(out)
    out += b"\x00\x00\x00\x00\x00"
    skip_patches = [(skip_pos, len(out))]

    # XB trailer: InternalVer(4B), unk(4B), u1b4(4B), XB Lic>=127 u150(4B)
    if p + 16 != len(d):
        raise ValueError(f"StaticMesh {pkg.name_str(exp['nm'])} tail size mismatch: {p+16} != {len(d)}")
    iv, unk0, u1b4, _u150 = struct.unpack_from("<4I", d, p)
    out += struct.pack("<4I", iv, unk0, u1b4, 1)  # u1b8 = 1

    # PC Lic>=121 shadow tail (00, 00+rev0, sc*(0..sc-1), 00)
    out += b"\x00\x00\x00\x00\x00\x00"
    out += rp.write_ci(sc)
    for i in range(sc):
        out += struct.pack("<I", i)
    out += b"\x00"

    return bytes(out), skip_patches


_PC_SURF_TAIL = struct.pack("<5iI", -1, -1, -1, -1, -1, 0)


def walk_model_data(names, d, is_xb=True):
    """Parse UModel body (XB Lic=127 or PC Lic=124); convert XB->PC when is_xb=True."""
    read_ci = rp.read_ci
    _, p, _ = parse_prop_spans(names, d)
    p += 41  # UPrimitive: FBox (25B) + FSphere (16B)

    n_vec, p = read_ci(d, p)
    p += n_vec * 12
    n_pts, p = read_ci(d, p)
    p += n_pts * 12

    n_nod, p = read_ci(d, p)
    for _ in range(n_nod):
        p += 37  # FPlane(16) + ZoneMask(20) + NodeFlags(1)
        for _ in range(7):
            _, p = read_ci(d, p)
        p += 51  # 32 + 3 + 16

    out = bytearray()
    mark = 0

    n_surfs, p = read_ci(d, p)
    for _ in range(n_surfs):
        _, p = read_ci(d, p)  # Material
        p += 4  # PolyFlags
        for _ in range(7):
            _, p = read_ci(d, p)
        p += 24  # FPlane(16) + Nodes(4) + ZoneMask(4)
        for _ in range(4):
            _, p = read_ci(d, p)
        if is_xb:
            out.extend(d[mark:p])
            for _ in range(2):  # Strip XB Lic>=1 extra UObject* refs (0x0023FEAF)
                _, p = read_ci(d, p)
            mark = p
        p += 16  # 4 floats
        if is_xb:
            out.extend(d[mark:p])
            out.extend(_PC_SURF_TAIL)  # Insert PC Lic>=121 6 DWORDs (0x1097F702)
            mark = p
        else:
            p += 24

    n_verts, p = read_ci(d, p)
    for _ in range(n_verts):
        _, p = read_ci(d, p)
        _, p = read_ci(d, p)

    _, num_zones = struct.unpack_from("<II", d, p)
    p += 8
    for _ in range(num_zones):
        _, p = read_ci(d, p)
        p += 44  # ZoneMask*2(40) + LastRenderTime(4)
        n_z, p = read_ci(d, p)
        p += n_z * 4 + 80  # Connectivity + 4*ZoneMask(80)

    _, p = read_ci(d, p)  # Polys

    n_bounds, p = read_ci(d, p)
    for _ in range(n_bounds):
        p += 174  # 2*u32(8) + 2*FMatrix(128) + 2*u8(2) + 3*FVector(36)
        for _ in range(4):
            _, p = read_ci(d, p)
        if is_xb:
            out.extend(d[mark:p])
            for _ in range(2):  # Strip 2x XB 0x23FAD4 texture blocks
                u0 = struct.unpack_from("<I", d, p)[0]
                p += 4
                if u0 != 0:
                    u1, u2, u3, u4 = struct.unpack_from("<4I", d, p)
                    p += 16
                    a5 = u4 + 4
                    if a5 & 2:
                        sz = u0 * u1 + 0x800
                        if a5 & 8:
                            sz += 0x10
                    else:
                        sz = (u2 * u3) << 3
                    if (a5 & 4) == 0:
                        sz += 0x38
                    if u4 & 3:
                        p += sz
            mark = p

    n_lh, p = read_ci(d, p)
    for _ in range(n_lh):
        _, p = read_ci(d, p)
        n_b, p = read_ci(d, p)
        p += n_b + 12

    n_lf, p = read_ci(d, p)
    p += n_lf * 25
    n_lgt, p = read_ci(d, p)
    p += n_lgt * 4

    n_prt, p = read_ci(d, p)
    for _ in range(n_prt):
        for _ in range(3):
            _, p = read_ci(d, p)
        p += 8

    n_u6, p = read_ci(d, p)
    for _ in range(n_u6):
        _, p = read_ci(d, p)

    p += 8  # RootOutside + Linked

    n_lmt, p = read_ci(d, p)
    for _ in range(n_lmt):
        n_sub, p = read_ci(d, p)
        p += n_sub * 28 + 4

    if not is_xb:
        n_tail, p = read_ci(d, p)
        if n_tail != 0:
            raise ValueError(f"Unexpected non-zero trailing UModel PC array count {n_tail}")

    if p != len(d):
        raise ValueError(f"UModel length mismatch: parsed {p} != {len(d)}")

    if not is_xb:
        return None
    out.extend(d[mark:p])
    out.append(0)  # PC Lic>=121 trailing TArray count = 0 (0x109853A7)
    return bytes(out)


def convert_model(pkg, exp):
    """Convert XB UModel to PC Lic>=121 layout (Surfs, Bounds, trailing TArray)."""
    if not exp["data"]:
        return b"", []
    return walk_model_data(pkg.names, exp["data"], is_xb=True), []


def convert_mesh_animation(pkg, exp):
    """UMeshAnimation XB -> PC (LZO MotionChunk tracks expanded, extra MeshAnimSeq FName dropped)."""
    d = exp["data"]
    _, prop_end, _ = parse_prop_spans(pkg.names, d)
    parts = mc.parse_mesh_anim(d, prop_end, pkg.fv, pkg.lv, True)
    return d[:prop_end] + mc.emit_mesh_anim_pc(*parts), []


def convert_skeletal_mesh(pkg, exp, default_anim_ref=None):
    """USkeletalMesh XB -> PC (field reordering, PC-only fields added, XB-only u32 dropped)."""
    d = exp["data"]
    _, prop_end, _ = parse_prop_spans(pkg.names, d)
    S = mc.parse_skeletal_mesh(d, mc.ulod_end(prop_end, d), True, pkg.lv)
    if S["_end"] != len(d):
        raise ValueError(f"SkeletalMesh {pkg.name_str(exp['nm'])}: consumed {S['_end']} != {len(d)}")
    return mc.emit_skeletal_pc(S, pkg.name_str(exp["nm"]).encode(), default_anim_ref=default_anim_ref), []


def _read_fstring_span(b, p):
    sl, p2 = rp.read_ci(b, p)
    if sl == 0:
        return b"", p2
    if sl < 0:
        n = (-sl) * 2
        return b[p2 : p2 + n].decode("utf-16-le", "replace").rstrip("\x00"), p2 + n
    return b[p2 : p2 + sl].decode("latin1", "replace").rstrip("\x00"), p2 + sl


def _convert_aabbt_xb_to_pc(d, p):
    """Convert one XB Ver=129 FAABBT tree to PC Ver=100 FAABBT."""
    out = bytearray()
    _blk_sz, = struct.unpack_from("<I", d, p)  # XB Ver>=0x7c (124) preallocated node bytes
    p += 4
    out += d[p : p + 24]  # Root FBox (6 floats)
    p += 24

    cnt, p_next = rp.read_ci(d, p)
    out += d[p:p_next]
    p = p_next

    for i in range(cnt):
        tp = d[p]
        p += 1
        if tp == 3:
            out.append(3)
            _, p_next = rp.read_ci(d, p)
            out += d[p:p_next]
            p = p_next
            continue

        pc_tp = 8 if tp in (9, 64) else tp
        if pc_tp not in (1, 2, 4, 5, 6, 7, 8, 16, 32):
            raise ValueError(f"Unsupported FAABTObject type {tp} at obj {i}/{cnt}")
        out.append(pc_tp)

        out += d[p : p + 4]  # u32 [esi+0xc]
        p += 4
        _, p_next = rp.read_ci(d, p)  # UObject* [esi+0x10]
        out += d[p:p_next]
        p = p_next

        # Strip XB Ver>=0x79 (121) extra fields: UObject* + u8 + u32
        _, p = rp.read_ci(d, p)
        p += 1 + 4

        out += d[p : p + 24]  # FBox (6 floats)
        p += 24
        if tp in (5, 16, 32):
            out += d[p : p + 12]  # Extra FVector (3 floats)
            p += 12

    def _copy_node(pos):
        nt = d[pos]
        out.append(nt)
        pos += 1
        if nt == 1:
            return pos
        elif nt in (2, 4, 8):
            out.extend(d[pos : pos + 4])
            pos += 4
            pos = _copy_node(pos)
            pos = _copy_node(pos)
            return pos
        elif nt == 16:
            lc, pos_n = rp.read_ci(d, pos)
            out.extend(d[pos:pos_n])
            pos = pos_n
            out.extend(d[pos : pos + 4 * lc])
            return pos + 4 * lc
        raise ValueError(f"Unsupported FAABTNode type {nt} at {pos - 1}")

    p = _copy_node(p)
    return bytes(out), p


def convert_level(pkg, exp):
    """Convert SCDA XB ULevel (MyLevel) to SCCT PC layout."""
    d = exp["data"]
    if not d:
        return b"", []
    _, p, _ = parse_prop_spans(pkg.names, d)

    # ULevelBase: TTransArray<AActor*> + FURL
    dbnum, _dbmax = struct.unpack_from("<ii", d, p)
    p += 8
    for _ in range(dbnum):
        _, p = rp.read_ci(d, p)
    for _ in range(4):
        _, p = _read_fstring_span(d, p)
    op_cnt, p = rp.read_ci(d, p)
    for _ in range(op_cnt):
        _, p = _read_fstring_span(d, p)
    p += 8  # Port, Valid
    out = bytearray(d[:p])

    # Strip XB Lic>=13 preload block before Model (12B when NumTex=0 & NumSM=0)
    flg, nt, ns = struct.unpack_from("<3I", d, p)
    if nt != 0 or ns != 0:
        raise ValueError(f"ULevel XB preload block non-zero: flg={flg} nt={nt} ns={ns}")
    p += 12

    # ULevel middle: Model + ApproxTime + NavRef + Refs[16] + FString
    p0 = p
    _, p = rp.read_ci(d, p)  # Model
    p += 4  # ApproxTime
    _, p = rp.read_ci(d, p)  # UObject*
    for _ in range(16):
        _, p = rp.read_ci(d, p)
    _, p = _read_fstring_span(d, p)
    out += d[p0:p]

    # Two FAABBT collision/render trees
    ab1, p = _convert_aabbt_xb_to_pc(d, p)
    ab2, p = _convert_aabbt_xb_to_pc(d, p)
    out += ab1 + ab2

    # PC Ver>=0x3d post-AABBT2 preload counts (NumTextures=0, NumStaticMeshes=0)
    out += struct.pack("<2I", 0, 0)

    # Remaining Ver>=0x41 ref + Ver>=0x4b TArray<UObject*>
    p_tail = p
    _, p = rp.read_ci(d, p)
    arr_cnt, p = rp.read_ci(d, p)
    for _ in range(arr_cnt):
        _, p = rp.read_ci(d, p)
    out += d[p_tail:p]

    if p != len(d):
        raise ValueError(f"ULevel parse mismatch: consumed {p}/{len(d)} bytes")
    return bytes(out), []


def _read_xb_prop_ref(d, p):
    c, p = rp.read_ci(d, p)
    if c == 0:
        return None, p
    nm, p = rp.read_ci(d, p)
    out, p = rp.read_ci(d, p)
    return (c, nm, out), p


# SCDA extended native opcodes whose parameter count grew vs CT PC (extra params are
# appended at the end) or that do not exist in CT PC (None = NOP entire call).
# Dropped bytes are padded with EX_Nothing (0x0B -> 0x10CED980 ret 8) AFTER 0x16 so
# in-memory byte offsets (Jump/JumpIfNot/Switch/LabelTable) remain 100% invariant.
SCDA_NATIVE_ARG_LIMIT = {
    1407: 7,     # EPattern.Speech: 10 -> 7 args
    3333: 10,    # EPattern.AddObjectiveToMission: 12-13 -> 10 args
    3472: 5,     # EPattern.PopUpMessage: 8-9 -> 5 args
    4025: None,  # EPattern.Communications: SCDA-only -> EX_Nothing padding
}


def _walk_script_expr(d, p, mem, is_xb, on_prop_ref=None):
    """Walk one UE2 script expression (XB 3-CI or PC 1-CI prop refs) and return (op, p, mem, bytes)."""
    read_ci = rp.read_ci
    out = bytearray() if on_prop_ref is not None else None
    p0 = p
    op = d[p]
    p += 1
    mem += 1
    mem_start = mem - 1
    if out is not None:
        out.append(op)

    def _emit_raw(end_p):
        if out is not None:
            out.extend(d[p0:end_p])

    if op >= 0x70:
        while True:
            so, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
            if out is not None:
                out.extend(sub)
            if so == 0x16:
                break
        return op, p, mem, bytes(out) if out is not None else None

    if op >= 0x60:
        ext_b = d[p]
        nat = ((op - 0x60) << 8) | ext_b
        p += 1
        mem += 1
        args = []
        end_sub = b""
        while True:
            m0 = mem
            so, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
            if so == 0x16:
                end_sub = sub
                break
            args.append((sub, mem - m0))
        if out is not None:
            if is_xb and nat in SCDA_NATIVE_ARG_LIMIT:
                limit = SCDA_NATIVE_ARG_LIMIT[nat]
                if limit is None:
                    total_mem = mem - mem_start
                    return op, p, mem, b"\x0b" * total_mem
                if len(args) > limit:
                    out.append(ext_b)
                    for sub, _ in args[:limit]:
                        out.extend(sub)
                    out.extend(end_sub)
                    dropped_mem = sum(m for _, m in args[limit:])
                    out.extend(b"\x0b" * dropped_mem)
                    return op, p, mem, bytes(out)
            out.append(ext_b)
            for sub, _ in args:
                out.extend(sub)
            out.extend(end_sub)
        return op, p, mem, bytes(out) if out is not None else None

    if op in (0x00, 0x01, 0x02, 0x29):
        mem += 4
        if is_xb:
            ref, p = _read_xb_prop_ref(d, p)
            if out is not None:
                out.extend(on_prop_ref(ref))
        else:
            ref, p = read_ci(d, p)
        return op, p, mem, bytes(out) if out is not None else None

    if op in (0x1C, 0x1B, 0x38):
        _, p = read_ci(d, p)
        mem += 4
        if out is not None:
            out.extend(d[p0 + 1 : p])
        while True:
            so, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
            if out is not None:
                out.extend(sub)
            if so == 0x16:
                break
        return op, p, mem, bytes(out) if out is not None else None

    if op in (0x12, 0x19):
        _, p, mem, sub1 = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        p_mid = p
        p += 3
        mem += 3
        _, p, mem, sub2 = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub1)
            out.extend(d[p_mid : p_mid + 3])
            out.extend(sub2)
        return op, p, mem, bytes(out) if out is not None else None

    n_sub = {0x11: 4, 0x40: 3, 0x41: 3, 0x0F: 2, 0x10: 2, 0x14: 2, 0x1A: 2, 0x04: 1, 0x0D: 1, 0x0E: 1, 0x37: 1, 0x43: 1}.get(op, 0)
    if n_sub:
        for _ in range(n_sub):
            _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
            if out is not None:
                out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None

    if op in (0x1D, 0x1E):
        p += 4
        mem += 4
    elif op == 0x1F:
        while d[p] != 0:
            p += 1
            mem += 1
        p += 1
        mem += 1
    elif op == 0x34:
        while d[p : p + 2] != b"\x00\x00":
            p += 2
            mem += 2
        p += 2
        mem += 2
    elif op in (0x20, 0x21):
        _, p = read_ci(d, p)
        mem += 4
    elif op in (0x22, 0x23):
        p += 12
        mem += 12
    elif op in (0x24, 0x2C):
        p += 1
        mem += 1
    elif op == 0x06:
        p += 2
        mem += 2
    elif op == 0x0C:
        while True:
            nm, p = read_ci(d, p)
            p += 4
            mem += 8
            if nm == 0:
                break
    elif op in (0x13, 0x2E):
        _, p = read_ci(d, p)
        mem += 4
        if out is not None:
            out.extend(d[p0 + 1 : p])
        _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None
    elif op == 0x2F:
        _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub)
            out.extend(d[p : p + 2])
        p += 2
        mem += 2
        return op, p, mem, bytes(out) if out is not None else None
    elif op in (0x05, 0x39):
        if out is not None:
            out.append(d[p])
        p += 1
        mem += 1
        _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None
    elif op == 0x0A:
        nxt = struct.unpack_from("<H", d, p)[0]
        if out is not None:
            out.extend(d[p : p + 2])
        p += 2
        mem += 2
        if nxt != 0xFFFF:
            _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
            if out is not None:
                out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None
    elif op in (0x07, 0x09, 0x18):
        if out is not None:
            out.extend(d[p : p + 2])
        p += 2
        mem += 2
        _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None
    elif op in (0x32, 0x33):
        _, p = read_ci(d, p)
        mem += 4
        if out is not None:
            out.extend(d[p0 + 1 : p])
        _, p, mem, s1 = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        _, p, mem, s2 = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(s1)
            out.extend(s2)
        return op, p, mem, bytes(out) if out is not None else None
    elif op == 0x36:
        mem += 4
        if is_xb:
            ref, p = _read_xb_prop_ref(d, p)
            if out is not None:
                out.extend(on_prop_ref(ref))
        else:
            ref, p = read_ci(d, p)
        _, p, mem, sub = _walk_script_expr(d, p, mem, is_xb, on_prop_ref)
        if out is not None:
            out.extend(sub)
        return op, p, mem, bytes(out) if out is not None else None

    if out is not None:
        out.extend(d[p0 + 1 : p])
    return op, p, mem, bytes(out) if out is not None else None


def walk_ustruct_pc(names, exp_cls_fn, exp, d):
    """Validate a PC UStruct-derived export body (Class/State/Function/Struct)."""
    read_ci = rp.read_ci
    cn = exp_cls_fn(exp)
    p = 0
    if cn != "Class":
        _, p, _ = parse_prop_spans(names, d)
    for _ in range(4):
        _, p = read_ci(d, p)
    _, p = read_ci(d, p)  # FriendlyName
    p += 8  # Line + TextPos
    _, p = read_ci(d, p)  # CppText
    script_sz = struct.unpack_from("<I", d, p)[0]
    p += 4
    mem = 0
    while mem < script_sz:
        _, p, mem, _ = _walk_script_expr(d, p, mem, is_xb=False)
    if mem != script_sz:
        raise ValueError(f"PC UStruct script mem {mem} != {script_sz}")
    if cn == "Function":
        p += 7
    elif cn == "State":
        p += 22
    elif cn == "Class":
        p += 22
        cflags = struct.unpack_from("<I", d, p)[0]
        p += 20  # ClassFlags + Guid
        n_dep, p = read_ci(d, p)
        for _ in range(n_dep):
            _, p = read_ci(d, p)
            p += 8
        n_imp, p = read_ci(d, p)
        for _ in range(n_imp):
            _, p = read_ci(d, p)
        _, p = read_ci(d, p)
        _, p = read_ci(d, p)
        n_hc, p = read_ci(d, p)
        for _ in range(n_hc):
            _, p = read_ci(d, p)
        if not (cflags & 0x4000):
            _, pend, _ = parse_prop_spans(names, d[p:])
            p += pend
    if p != len(d):
        raise ValueError(f"PC UStruct length mismatch: parsed {p} != {len(d)}")


def convert_ustruct_package(pkg, retail=None):
    """Expand XB inlined UProperty chains into standalone PC UProperty exports and rewrite Script opcodes."""
    read_ci = rp.read_ci
    write_ci = rp.write_ci
    ustruct_exps = [
        e for e in pkg.exps
        if e["data"] and pkg.exp_cls_name(e) in ("Class", "State", "Function", "Struct")
    ]
    if not ustruct_exps:
        return pkg

    # Pass 1: Extract inline UProperty chains and append standalone PC UProperty exports.
    prop_map = {}
    rewrite_meta = {}
    for e in ustruct_exps:
        cn = pkg.exp_cls_name(e)
        d = e["data"]
        p = 0
        if cn != "Class":
            _, p, _ = parse_prop_spans(pkg.names, d)
        pre_end = p
        sup, p = read_ci(d, p)
        nxt, p = read_ci(d, p)
        st, p = read_ci(d, p)
        ch, p = read_ci(d, p)

        inline_props = []
        cur, p = _read_xb_prop_ref(d, p)
        while cur is not None:
            pc_name = pkg.name_str(pkg.imps[-cur[0] - 1]["nm"]) if cur[0] < 0 else pkg.name_str(pkg.exps[cur[0] - 1]["nm"])
            arr_dim, pflags = struct.unpack_from("<HI", d, p)
            p += 6
            cat, p = read_ci(d, p)
            extra_unk, p = read_ci(d, p)
            nxt_prop, p = _read_xb_prop_ref(d, p)
            t0 = p
            if pc_name in ("ObjectProperty", "StructProperty", "ByteProperty", "ArrayProperty"):
                _, p = read_ci(d, p)
            elif pc_name in ("ClassProperty", "FixedArrayProperty", "MapProperty"):
                _, p = read_ci(d, p)
                _, p = read_ci(d, p)
            elif pc_name not in ("IntProperty", "StrProperty", "BoolProperty", "FloatProperty", "NameProperty"):
                raise ValueError(f"Unsupported XB UProperty subclass {pc_name}")
            inline_props.append((cur, arr_dim, pflags, cat, extra_unk, d[t0:p]))
            cur = nxt_prop

        body_rest_off = p
        base_idx = len(pkg.exps) + 1
        for k, (cur_ref, arr_dim, pflags, cat, extra_unk, tail_b) in enumerate(inline_props):
            c_idx, nm_idx, out_idx = cur_ref
            exp_idx = base_idx + k
            nxt_idx = base_idx + k + 1 if k + 1 < len(inline_props) else ch
            prop_body = (
                b"\x00\x00"  # UObject None + SuperField=0
                + write_ci(nxt_idx)
                + struct.pack("<HI", arr_dim, pflags)
                + write_ci(cat)
                + write_ci(extra_unk)
                + tail_b
            )
            pkg.exps.append({
                "idx": exp_idx,
                "cls": c_idx,
                "sup": 0,
                "out": out_idx,
                "nm": nm_idx,
                "fl": 0x00070004,
                "ssz": len(prop_body),
                "soff": 0,
                "data": prop_body,
            })
            prop_map[(out_idx, nm_idx)] = exp_idx

        new_ch = base_idx if inline_props else ch
        rewrite_meta[e["idx"]] = (pre_end, sup, nxt, st, new_ch, body_rest_off)

    # Pass 2: Rewrite each UStruct export header + Script bytecode to PC 1-CI property refs.
    core_nm_idx = pkg.ensure_name("Core", 0x04070010)
    ext_prop_cache = {}
    imp_lookup = {(im["cc"], im["out"], im["nm"]): -(i + 1) for i, im in enumerate(pkg.imps)}
    eventtype_ci_set = set()

    def resolve_prop_ref(ref):
        c_idx, nm_idx, out_idx = ref
        if out_idx > 0:
            if (out_idx, nm_idx) not in prop_map:
                raise KeyError(f"Missing inlined UProperty ({out_idx}, {pkg.name_str(nm_idx)})")
            return write_ci(prop_map[(out_idx, nm_idx)])
        key = (c_idx, nm_idx, out_idx)
        if key in ext_prop_cache:
            return ext_prop_cache[key]

        cls_str = pkg.name_str(pkg.imps[-c_idx - 1]["nm"]) if c_idx < 0 else pkg.name_str(pkg.exps[c_idx - 1]["nm"])
        is_aievent_type = False
        if retail is not None and out_idx < 0:
            root_low = pkg.imp_root_pkg(-out_idx - 1).lower()
            full_path = f"{pkg.ref_path(out_idx)}.{pkg.name_str(nm_idx)}".lower()
            sub_path = full_path.split(".", 1)[1] if "." in full_path else full_path
            if sub_path == "actor.eaievent.eventtype":
                is_aievent_type = True
            cls_map = retail.get_export_classes(root_low)
            if sub_path in cls_map and cls_map[sub_path]:
                cls_str = cls_map[sub_path]

        cc_idx = pkg.ensure_name(cls_str, 0x04070010)
        imp_key = (cc_idx, out_idx, nm_idx)
        if imp_key in imp_lookup:
            ref_idx = imp_lookup[imp_key]
        else:
            pkg.imps.append({"cp": core_nm_idx, "cc": cc_idx, "out": out_idx, "nm": nm_idx})
            ref_idx = -len(pkg.imps)
            imp_lookup[imp_key] = ref_idx
        ci_b = write_ci(ref_idx)
        if is_aievent_type:
            eventtype_ci_set.add(ci_b)
        ext_prop_cache[key] = ci_b
        return ci_b

    for e in ustruct_exps:
        cn = pkg.exp_cls_name(e)
        d = e["data"]
        pre_end, sup, nxt, st, new_ch, p = rewrite_meta[e["idx"]]
        out_buf = bytearray(d[:pre_end])
        out_buf += write_ci(sup) + write_ci(nxt) + write_ci(st) + write_ci(new_ch)

        p_mid = p
        _, p = read_ci(d, p)  # FriendlyName
        p += 8  # Line, TextPos
        _, p = read_ci(d, p)  # CppText
        script_sz = struct.unpack_from("<I", d, p)[0]
        p += 4
        out_buf.extend(d[p_mid:p])

        mem = 0
        in_eventtype_switch = False
        while mem < script_sz:
            op, p, mem, expr_bytes = _walk_script_expr(d, p, mem, is_xb=True, on_prop_ref=resolve_prop_ref)
            eb = bytearray(expr_bytes)
            # SCDA widened Actor.EAIEvent.EventType to IntProperty (EX_Switch bSize=4 + EX_IntConst).
            # CT PC defines it as ByteProperty (AIEventType Enum: 0xBE=AI_DEAD, 0xBF=AI_UNCONSCIOUS).
            if (
                op == 0x05
                and len(eb) >= 4
                and eb[1] == 0x04
                and eb[2] == 0x36
                and any(eb[3 : 3 + len(c)] == c for c in eventtype_ci_set)
            ):
                eb[1] = 0x01
                in_eventtype_switch = True
            elif in_eventtype_switch and op == 0x0A and len(eb) >= 3:
                nxt_case = struct.unpack_from("<H", eb, 1)[0]
                if nxt_case == 0xFFFF:
                    in_eventtype_switch = False
                elif len(eb) == 8 and eb[3] == 0x1D:
                    scda_ev = struct.unpack_from("<I", eb, 4)[0]
                    ct_ev = {412: 0xBE, 413: 0xBF}.get(scda_ev, scda_ev & 0xFF)
                    # Replace 5B EX_IntConst with 2B EX_ByteConst + 3B EX_Nothing to keep mem offsets fixed.
                    eb[3:8] = bytes([0x24, ct_ev, 0x0B, 0x0B, 0x0B])
            out_buf.extend(eb)

        p_tail = p
        if cn == "Function":
            p += 7
        elif cn == "State":
            p += 22
        elif cn == "Class":
            p += 22
            cflags = struct.unpack_from("<I", d, p)[0]
            p += 20
            n_dep, p = read_ci(d, p)
            for _ in range(n_dep):
                _, p = read_ci(d, p)
                p += 8
            n_imp, p = read_ci(d, p)
            for _ in range(n_imp):
                _, p = read_ci(d, p)
            _, p = read_ci(d, p)
            _, p = read_ci(d, p)
            n_hc, p = read_ci(d, p)
            for _ in range(n_hc):
                _, p = read_ci(d, p)
            if not (cflags & 0x4000):
                _, pend, _ = parse_prop_spans(pkg.names, d[p:])
                p += pend
        if p != len(d):
            raise ValueError(f"XB UStruct {pkg.name_str(e['nm'])} mismatch: {p} != {len(d)}")
        out_buf.extend(d[p_tail:p])
        e["data"] = bytes(out_buf)
        e["ssz"] = len(e["data"])

    return pkg


def prune_echelonpattern(pkg, used_pkg_names_lower=None):
    """Build pruned pkg (Name/Import/Export tables) with only non-empty exports of echelonpattern.u.
    Wait: instead of renumbering every bytecode/property object ref inside UClass, we keep
    only non-empty exports by zeroing out unused Package imports? No: let's see which imports
    are actually referenced inside the 37 non-empty exports (header sup/cls/out + data bytes)!
    Actually keeping exact Import indices & turning unused unrelated Import entries? No: we can
    build a clean remapped ImportTable containing ONLY reachable Imports from those 37 exports
    ONLY if we scan FCompactIndex refs, OR keep empty-pkg imports untouched when stubbed cleanly!
    Wait - let's check whether keeping only non-empty exports + pruning unreachable root Packages
    works without renumbering Export indices if we zero-link? Can't have dangling Import indices.
    Even better: scan imports referenced in UField/UStruct/UState/UClass + DefaultProperties of
    the 37 non-empty exports OR emit the tiny 1KB .uax stubs that are SCDA-only."""
    return pkg


def serialize_pkg(pkg, pkg_renames, class_remap=None, havok_payload=None, force_keep_exports=None, retail=None, default_anim_ref=None):
    """Convert & serialize a PackageData to SCCT PC (FileVer=100, LicVer=124, PkgFlags=0)."""
    # 0. Expand any XB inlined UProperty chains (echelonpattern.u) into PC exports/imports first
    if any(e["data"] and pkg.exp_cls_name(e) in ("Class", "State", "Function", "Struct") for e in pkg.exps):
        convert_ustruct_package(pkg, retail=retail if retail is not None else RetailIndex())

    # 1. Apply package renames and class remaps directly on ImportTable rows via NameTable
    for i, im in enumerate(pkg.imps):
        out_idx = im["out"]
        cc_low = pkg.name_str(im["cc"]).lower()
        nm_low = pkg.name_str(im["nm"]).lower()
        if out_idx == 0 and cc_low == "package":
            if nm_low in pkg_renames:
                im["nm"] = pkg.ensure_name(pkg_renames[nm_low], pkg.names[im["nm"]][1])
        elif class_remap and out_idx < 0:
            root_low = pkg.imp_root_pkg(i).lower()
            # Map before package rename took place? Check original or renamed
            for (src_p, src_c), (dst_p, dst_c) in class_remap.items():
                if nm_low == src_c and (pkg.name_str(pkg.imps[-out_idx - 1]["nm"]).lower() in (src_p, pkg_renames.get(src_p, src_p).lower())):
                    im["nm"] = pkg.ensure_name(dst_c, pkg.names[im["nm"]][1])

    if havok_payload is not None:
        for e in pkg.exps:
            if e["data"] and pkg.exp_cls_name(e).lower() == "echelonlevelinfo":
                d = e["data"]
                sf_end = _get_actor_prop_offset(d)
                spans, _, _ = parse_prop_spans(pkg.names, d[sf_end:])
                if "sammesh" not in {nm.lower() for nm, _, _ in spans}:
                    coop_pkg_imp = _ensure_import(pkg, "Core", "Package", 0, "CoopAnims")
                    coop_mesh_ref = _ensure_import(pkg, "Engine", "SkeletalMesh", coop_pkg_imp, "COOP_Player")
                    prop_b = _encode_obj_prop(pkg.ensure_name("SamMesh", 0x00070010), coop_mesh_ref)
                    e["data"] = d[:sf_end] + prop_b + d[sf_end:]
                    e["ssz"] = len(e["data"])
                break

    # 2. If force_keep_exports (e.g. for echelonpattern.u -> empty out nothing, OR wait:
    # keeping ssz=0 rows in ExportTable costs only ~12B per row and preserves 100% of Class/Super/Function indices!)
    # Wait: if we keep echelonpattern's ExportTable slots untouched (with ssz=0 for unused maps),
    # NO index anywhere changes and 3256 * 12B is only ~39KB!

    # 3. Convert every export body & collect SkipOffset patch locations
    converted_bodies = []
    patch_lists = []
    for e in pkg.exps:
        cname = pkg.exp_cls_name(e)
        if force_keep_exports is not None and e["idx"] not in force_keep_exports:
            converted_bodies.append(b"")
            patch_lists.append([])
            continue
        if not e["data"]:
            converted_bodies.append(b"")
            patch_lists.append([])
        elif cname in ("Texture", "VolumeTexture"):
            body, patches = convert_texture(pkg, e)
            converted_bodies.append(body)
            patch_lists.append(patches)
        elif cname == "StaticMesh":
            body, patches = convert_static_mesh(pkg, e)
            converted_bodies.append(body)
            patch_lists.append(patches)
        elif cname == "Model":
            body, patches = convert_model(pkg, e)
            converted_bodies.append(body)
            patch_lists.append(patches)
        elif cname == "MeshAnimation":
            body, patches = convert_mesh_animation(pkg, e)
            converted_bodies.append(body)
            patch_lists.append(patches)
        elif cname == "SkeletalMesh":
            body, patches = convert_skeletal_mesh(pkg, e, default_anim_ref=default_anim_ref)
            converted_bodies.append(body)
            patch_lists.append(patches)
        elif cname == "Level":
            body, patches = convert_level(pkg, e)
            converted_bodies.append(body)
            patch_lists.append(patches)
        else:
            converted_bodies.append(e["data"])
            patch_lists.append([])

    # SCDA's player body is coop_player_a. CT resolves SamMesh=ESam.SamAMesh to the SamA export.
    sama_i = player_i = None
    for i, e in enumerate(pkg.exps):
        if pkg.exp_cls_name(e) != "SkeletalMesh":
            continue
        nm = pkg.name_str(e["nm"]).lower()
        if nm == "sama":
            sama_i = i
        elif nm == "coop_player_a":
            player_i = i
    if (
        sama_i is not None
        and player_i is not None
        and not converted_bodies[sama_i]
        and converted_bodies[player_i]
    ):
        converted_bodies[sama_i] = converted_bodies[player_i]

    # 4. Assemble NameTable (Echelon PC format: write_ci(len(s)) + latin1 + \0 + u32 flags)
    name_tbl = bytearray()
    for ns, fl in pkg.names:
        b = ns.encode("latin1", "replace")
        name_tbl += rp.write_ci(len(b)) + b + b"\x00" + struct.pack("<I", fl)

    # 5. Build Header prefix (64B + Guid + Generations match reconstruct_packages layout)
    compile_s = b"CompilationTime: Nov 23 2026 00:00:00\x00"
    compile_xor = bytes(c ^ 0xFF for c in compile_s)
    hdr_prefix = (
        struct.pack("<IHHII6I", 0x9E2A83C1, 100, 124, 0, 1, 0, 0, 0, 0, 0, 0)
        + struct.pack("<I", 0x0FF0ADDE)
        + rp.write_ci(len(compile_xor))
        + compile_xor
        + pkg.guid
        + struct.pack("<I", len(pkg.generations) // 2)
        + struct.pack(f"<{len(pkg.generations)}I", *pkg.generations)
    )

    name_off = len(hdr_prefix)
    body_base = name_off + len(name_tbl)

    # 6. Concatenate ExportBodies and patch SkipOffsets with exact file offsets
    bodies_buf = bytearray()
    new_meta = []
    for i, e in enumerate(pkg.exps):
        body = converted_bodies[i]
        ssz = len(body)
        if ssz > 0:
            soff = body_base + len(bodies_buf)
            b_arr = bytearray(body)
            for sp_pos, rel_end in patch_lists[i]:
                struct.pack_into("<I", b_arr, sp_pos, soff + rel_end)
            bodies_buf += b_arr
        else:
            soff = 0
        new_meta.append((e["cls"], e["sup"], e["out"], e["nm"], e["fl"], ssz, soff))

    # 7. ImportTable & ExportTable
    imp_off = body_base + len(bodies_buf)
    imp_tbl = bytearray()
    for im in pkg.imps:
        imp_tbl += rp.write_ci(im["cp"]) + rp.write_ci(im["cc"])
        imp_tbl += struct.pack("<i", im["out"])
        imp_tbl += rp.write_ci(im["nm"])

    exp_off = imp_off + len(imp_tbl)
    exp_tbl = bytearray()
    for cls, sup, out_v, nm, fl, ssz, soff in new_meta:
        exp_tbl += rp.write_ci(cls) + rp.write_ci(sup)
        exp_tbl += struct.pack("<i", out_v)
        exp_tbl += rp.write_ci(nm)
        exp_tbl += struct.pack("<I", fl)
        exp_tbl += rp.write_ci(ssz)
        if ssz > 0:
            exp_tbl += rp.write_ci(soff)

    hdr_final = (
        struct.pack(
            "<IHHII6I",
            0x9E2A83C1,
            100,
            124,
            0,
            1,
            len(pkg.names),
            name_off,
            len(pkg.exps),
            exp_off,
            len(pkg.imps),
            imp_off,
        )
        + hdr_prefix[40:]
    )

    out_bytes = bytearray(hdr_final)
    out_bytes += name_tbl
    out_bytes += bodies_buf
    out_bytes += imp_tbl
    out_bytes += exp_tbl
    if havok_payload is not None:
        out_bytes += havok_payload
        out_bytes += struct.pack("<III", len(havok_payload), 1, 0xDEADF00F)
    return bytes(out_bytes)


def find_reconstructed_pkg(recon_dir, root_lower):
    for sub in ("map_pkgs", "common_pkgs"):
        sd = os.path.join(recon_dir, sub)
        if not os.path.isdir(sd):
            continue
        for ext in (".scl", ".u", ".utx", ".usx", ".ukx", ".uax"):
            p = os.path.join(sd, root_lower + ext)
            if os.path.exists(p):
                return p, ext
    return None, None


def collect_active_export_refs(pkg, root_pkg_lower_set):
    """Return subset of Imports referenced by non-empty exports? Actually check package roots
    where CT retail already satisfies every referenced import vs needs SCDA copy."""
    pass


def rename_variable_class(pk, map_name, out_name):
    """CT looks up EchelonPattern.V<launched map name>. Rename vx01_coop_ice_a -> Vscda_ice_a."""
    old = "v" + map_name.lower()
    new = "V" + out_name
    found = False
    for row in pk.names:
        if row[0].lower() == old:
            row[0] = new
            found = True
    return new if found else None


def _get_actor_prop_offset(d):
    """Return byte offset where an Actor export's UObject property list starts (after StateFrame)."""
    p = 0
    node, p = rp.read_ci(d, p)
    _, p = rp.read_ci(d, p)
    p += 12  # ProbeMask (8B) + LatentAction (4B)
    if node != 0:
        _, p = rp.read_ci(d, p)
    return p


def _encode_name_prop(name_idx, val_idx):
    val_b = rp.write_ci(val_idx)
    n = len(val_b)
    if n == 1:
        st_hdr = b"\x06"
    elif n == 2:
        st_hdr = b"\x16"
    elif n == 4:
        st_hdr = b"\x26"
    else:
        st_hdr = bytes([0x56, n])
    return rp.write_ci(name_idx) + st_hdr + val_b


def _encode_obj_prop(name_idx, obj_ref):
    ref_b = rp.write_ci(obj_ref)
    n = len(ref_b)
    if n == 1:
        st_hdr = b"\x05"
    elif n == 2:
        st_hdr = b"\x15"
    elif n == 4:
        st_hdr = b"\x25"
    else:
        st_hdr = bytes([0x55, n])
    return rp.write_ci(name_idx) + st_hdr + ref_b


def _ensure_import(pk, cp_str, cc_str, out_idx, nm_str):
    cp_low, cc_low, nm_low = cp_str.lower(), cc_str.lower(), nm_str.lower()
    for i, im in enumerate(pk.imps):
        if (
            im["out"] == out_idx
            and pk.name_str(im["cp"]).lower() == cp_low
            and pk.name_str(im["cc"]).lower() == cc_low
            and pk.name_str(im["nm"]).lower() == nm_low
        ):
            return -(i + 1)
    cp_i = pk.ensure_name(cp_str, 0x04070010)
    cc_i = pk.ensure_name(cc_str, 0x04070010)
    nm_i = pk.ensure_name(nm_str, 0x00070010)
    pk.imps.append({"cp": cp_i, "cc": cc_i, "out": out_idx, "nm": nm_i})
    return -len(pk.imps)


def _clone_struct_prop(pk, prop_bytes, new_name_str):
    _, after_nm = rp.read_ci(prop_bytes, 0)
    return rp.write_ci(pk.ensure_name(new_name_str, 0x00070010)) + prop_bytes[after_nm:]


def _encode_bytes_prop_size(pt_and_arr_base, body):
    n = len(body)
    if n < 256:
        return bytes([0x50 | pt_and_arr_base, n]) + body
    if n < 65536:
        return bytes([0x60 | pt_and_arr_base]) + struct.pack("<H", n) + body
    return bytes([0x70 | pt_and_arr_base]) + struct.pack("<I", n) + body


def patch_scda_level_actors(pk, parsed_pkgs):
    """Apply clean UE2 property fixes to mapped SCDA actors before PC serialization."""
    keycard_ref = None
    if "ingredients_obj" in parsed_pkgs:
        ing_pk = parsed_pkgs["ingredients_obj"][2]
        if any(ing_pk.ref_path(e["idx"]).lower() == "cardreader.keycard" for e in ing_pk.exps):
            ing_imp = _ensure_import(pk, "Core", "Package", 0, "ingredients_obj")
            cr_imp = _ensure_import(pk, "Core", "Package", ing_imp, "cardreader")
            keycard_ref = _ensure_import(pk, "Engine", "StaticMesh", cr_imp, "keycard")

    coopnpc_imp = _ensure_import(pk, "Core", "Package", 0, "CoopNPC")
    ct_npc_mesh_ref = _ensure_import(pk, "Engine", "SkeletalMesh", coopnpc_imp, "NK_Soldier_COOP")
    ech_ing_imp = _ensure_import(pk, "Core", "Package", 0, "EchelonIngredient")
    ak_ref = _ensure_import(pk, "Core", "Class", ech_ing_imp, "EAK101W")
    pya_ref = _ensure_import(pk, "Core", "Class", ech_ing_imp, "EPYa")

    target_classes = (
        "ekeycard",
        "ecardreader",
        "ecoopinteractvolume",
        "ecoopsuccessvolume",
        "evolume",
        "egameplayobjectpickup",
        "ecustomobjectiveinteraction",
        "eaigenericnpc",
        "emusic",
        "echelonplayerstart",
        "espeechdispatcher",
    )

    keycard_fixed = 0
    cardreader_fixed = 0
    coopvol_fixed = 0
    npc_fixed = 0
    npc_class_fixed = 0
    music_fixed = 0
    pstart_fixed = 0
    speech_fixed = 0
    col_fixed = 0
    for e in pk.exps:
        if not e["data"]:
            continue
        cls_low = pk.exp_cls_name(e).lower()
        if cls_low not in target_classes:
            continue
        d = bytearray(e["data"])
        sf_end = _get_actor_prop_offset(d)
        spans, _, none_start = parse_prop_spans(pk.names, d[sf_end:])
        existing = {nm.lower() for nm, _, _ in spans}
        drop_props = set()
        replacements = {}
        prefix = bytearray()

        if cls_low == "ekeycard":
            # EKeyCard remaps to EGameplayObjectPickup, which lacks pickup/collision/mesh defaults.
            if "bpickable" not in existing:
                prefix += rp.write_ci(pk.ensure_name("bPickable", 0x00070010)) + b"\xd3\x00"
            if "bdamageable" not in existing:
                prefix += rp.write_ci(pk.ensure_name("bDamageable", 0x00070010)) + b"\x53\x00"
            for col_nm in ("ColFlagPlayer", "ColFlagHavok", "ColFlagCamera"):
                if col_nm.lower() not in existing:
                    prefix += rp.write_ci(pk.ensure_name(col_nm, 0x00070010)) + b"\x01\x03"
            if keycard_ref is not None and "staticmesh" not in existing:
                prefix += _encode_obj_prop(pk.ensure_name("StaticMesh", 0x00070010), keycard_ref)
            if "objectname" not in existing:
                prefix += rp.write_ci(pk.ensure_name("ObjectName", 0x00070010)) + b"\x5d\x09\x08KeyCard\x00"
            if prefix:
                keycard_fixed += 1
        elif cls_low == "ecardreader":
            # ECardReader remaps to ESwitchObject; ensure collision + interaction label so players can trigger it.
            for col_nm in ("ColFlagPlayer", "ColFlagHavok", "ColFlagCamera"):
                if col_nm.lower() not in existing:
                    prefix += rp.write_ci(pk.ensure_name(col_nm, 0x00070010)) + b"\x01\x03"
            if "objectname" not in existing:
                prefix += rp.write_ci(pk.ensure_name("ObjectName", 0x00070010)) + b"\x5d\x0c\x0bCardReader\x00"
            if prefix:
                cardreader_fixed += 1
        elif cls_low == "ecoopinteractvolume":
            # Classify SCDA CoopInteraction (omitted = 0 DualRappellingUpStart):
            # - 4: Back-to-Back Climb -> remap to 0, Limit=404.25 sentinel, Tag=SCDA_B2B_Climb
            # - 0: Real Dual Rappelling -> ensure Limit >= 350.0, drop bAlignAtCenter
            # - 2: HangoverWithRope -> drop bAlignAtCenter (SCDA pivot is mid-height, not ledge)
            coop_val = 0
            old_limit = None
            for nm, _, p_end in spans:
                nl = nm.lower()
                if nl == "coopinteraction" and p_end >= 2:
                    coop_val = d[sf_end + p_end - 1]
                elif nl == "limit" and p_end >= 4:
                    old_limit = struct.unpack("<f", d[sf_end + p_end - 4 : sf_end + p_end])[0]

            if coop_val == 4:
                for idx_s, (nm, p_start, p_end) in enumerate(spans):
                    if nm.lower() == "coopinteraction" and p_end >= 2:
                        seg = bytearray(d[sf_end + p_start : sf_end + p_end])
                        seg[-1] = 0
                        replacements[idx_s] = bytes(seg)
                drop_props.update(("limit", "tag"))
                prefix += rp.write_ci(pk.ensure_name("Limit", 0x00070010)) + b"\x24" + struct.pack("<f", 404.25)
                prefix += _encode_name_prop(
                    pk.ensure_name("Tag", 0x00070010),
                    pk.ensure_name("SCDA_B2B_Climb", 0x00070010),
                )
                coopvol_fixed += 1
            elif coop_val == 0:
                if "balignatcenter" in existing:
                    drop_props.add("balignatcenter")
                    coopvol_fixed += 1
                if old_limit is None or old_limit < 320.0:
                    drop_props.add("limit")
                    prefix += rp.write_ci(pk.ensure_name("Limit", 0x00070010)) + b"\x24" + struct.pack("<f", 350.0)
                    if "balignatcenter" not in existing:
                        coopvol_fixed += 1
            elif coop_val == 2:
                if "balignatcenter" in existing:
                    drop_props.add("balignatcenter")
                    coopvol_fixed += 1
        elif cls_low == "eaigenericnpc":
            # Remap EAIGenericNPC -> EchelonCharacter.ENorthKoreanSoldier with native
            # CT Co-op soldier mesh + weapons so EAIPawn.InitAnims binds without T-posing.
            if e["cls"] < 0:
                old_cls = pk.imps[-e["cls"] - 1]
                e["cls"] = _ensure_import(
                    pk,
                    pk.name_str(old_cls["cp"]),
                    pk.name_str(old_cls["cc"]),
                    old_cls["out"],
                    "ENorthKoreanSoldier",
                )
                npc_class_fixed += 1

            drop_props.update(("mesh", "backupmesh", "anim", "dyninitialinventory"))
            prefix += _encode_obj_prop(pk.ensure_name("Mesh", 0x00070010), ct_npc_mesh_ref)
            prefix += _encode_obj_prop(pk.ensure_name("BackupMesh", 0x00070010), ct_npc_mesh_ref)
            inv_body = rp.write_ci(2) + rp.write_ci(ak_ref) + rp.write_ci(pya_ref)
            prefix += rp.write_ci(pk.ensure_name("DynInitialInventory", 0x00070010)) + _encode_bytes_prop_size(9, inv_body)
            if "colflaghavok" not in existing:
                prefix += rp.write_ci(pk.ensure_name("ColFlagHavok", 0x00070010)) + b"\x01\x03"
            npc_fixed += 1
        elif cls_low == "emusic":
            def _read_obj_ref(p_start, p_end):
                q = sf_end + p_start
                _, q = rp.read_ci(d, q)
                info = d[q]
                q += 1
                if (info & 0x0F) != 5:
                    return None
                st = (info >> 4) & 7
                if st == 5:
                    q += 1
                elif st == 6:
                    q += 2
                elif st == 7:
                    q += 4
                if info & 0x80:
                    _, q = rp.read_ci(d, q)
                ref_idx, _ = rp.read_ci(d, q)
                return ref_idx

            mt_ref = None
            smt_ref = None
            for nm, p_start, p_end in spans:
                nl = nm.lower()
                if nl == "multitrackmusic":
                    mt_ref = _read_obj_ref(p_start, p_end)
                elif nl == "stopmultitrackmusic":
                    smt_ref = _read_obj_ref(p_start, p_end)
            if "start_explo" not in existing and mt_ref is not None:
                prefix += _encode_obj_prop(pk.ensure_name("Start_Explo", 0x00070010), mt_ref)
            if "stop_explo" not in existing and smt_ref is not None:
                prefix += _encode_obj_prop(pk.ensure_name("Stop_Explo", 0x00070010), smt_ref)
            if prefix:
                music_fixed += 1
        elif cls_low == "echelonplayerstart":
            if "startmesh" in existing or "skins" in existing:
                drop_props.update(("startmesh", "skins"))
                pstart_fixed += 1
        elif cls_low == "espeechdispatcher":
            # Expand SCDA eSPEECH_TeamInterrogation (SpeechType=3 + SpeechClassTeamInterrogation)
            # into two CT-compatible stSpeechInfo entries (SpeechType=0).
            for idx_s, (nm, p_start, p_end) in enumerate(spans):
                if nm.lower() != "speechinfos":
                    continue
                raw = d[sf_end + p_start : sf_end + p_end]
                q = 0
                prop_nm_idx, q = rp.read_ci(raw, q)
                q += 1  # ArrayProperty info (0x59 / 0x69)
                _, q = rp.read_ci(raw, q)  # array byte size
                cnt, q = rp.read_ci(raw, q)
                entries = []
                changed_speech = False
                for _ in range(cnt):
                    sc_ref = None
                    st_ref = None
                    stype = 0
                    while q < len(raw):
                        ni, q = rp.read_ci(raw, q)
                        fnm = pk.name_str(ni).lower()
                        if fnm == "none":
                            break
                        info = raw[q]
                        q += 1
                        tp = info & 0x0F
                        if tp == 3:
                            if ((info >> 4) & 7) == 5 and q < len(raw):
                                q += 1
                        elif tp == 1:
                            val = raw[q]
                            q += 1
                            if fnm == "speechtype":
                                stype = val
                        elif tp == 5:
                            st = (info >> 4) & 7
                            if st == 5:
                                q += 1
                            elif st == 6:
                                q += 2
                            elif st == 7:
                                q += 4
                            ref, q = rp.read_ci(raw, q)
                            if fnm == "speechclass":
                                sc_ref = ref
                            elif fnm == "speechclassteaminterrogation":
                                st_ref = ref
                    if sc_ref is not None:
                        entries.append((sc_ref, 0 if stype == 3 else stype))
                    if stype == 3 and st_ref is not None:
                        entries.append((st_ref, 0))
                        changed_speech = True
                if changed_speech and entries:
                    sc_nm = pk.ensure_name("SpeechClass", 0x00070010)
                    st_nm = pk.ensure_name("SpeechType", 0x00070010)
                    bd_nm = pk.ensure_name("bDispatched", 0x00070010)
                    so_nm = pk.ensure_name("SpeechObj", 0x00070010)
                    body = bytearray(rp.write_ci(len(entries)))
                    for ref, st_val in entries:
                        body += _encode_obj_prop(sc_nm, ref)
                        body += rp.write_ci(st_nm) + bytes([0x01, st_val])
                        body += rp.write_ci(bd_nm) + b"\x53\x00"
                        body += _encode_obj_prop(so_nm, 0)
                        body += b"\x00"
                    replacements[idx_s] = rp.write_ci(prop_nm_idx) + _encode_bytes_prop_size(9, bytes(body))
                    speech_fixed += 1

        # Add CT-PC ColLocation / ColRotation from existing Location / Rotation if missing
        if cls_low in ("ekeycard", "ecardreader", "ecoopinteractvolume", "ecoopsuccessvolume", "evolume",
                       "egameplayobjectpickup", "ecustomobjectiveinteraction", "eaigenericnpc"):
            for nm, p_start, p_end in spans:
                nl = nm.lower()
                if nl == "location" and "collocation" not in existing:
                    prefix += _clone_struct_prop(pk, d[sf_end + p_start : sf_end + p_end], "ColLocation")
                    col_fixed += 1
                elif nl == "rotation" and "colrotation" not in existing:
                    prefix += _clone_struct_prop(pk, d[sf_end + p_start : sf_end + p_end], "ColRotation")
                    col_fixed += 1

        if prefix or drop_props or replacements:
            rebuilt = bytearray()
            for idx_s, (nm, p_start, p_end) in enumerate(spans):
                if nm.lower() in drop_props:
                    continue
                if idx_s in replacements:
                    rebuilt += replacements[idx_s]
                else:
                    rebuilt += d[sf_end + p_start : sf_end + p_end]
            rebuilt += d[sf_end + none_start :]
            e["data"] = bytes(d[:sf_end]) + bytes(prefix) + bytes(rebuilt)
            e["ssz"] = len(e["data"])

    if any((keycard_fixed, cardreader_fixed, coopvol_fixed, npc_fixed, npc_class_fixed, music_fixed, pstart_fixed, speech_fixed, col_fixed)):
        print(
            f"  patched level actors: keycards={keycard_fixed}, cardreaders={cardreader_fixed}, "
            f"coopvols={coopvol_fixed}, npcs={npc_fixed} (class={npc_class_fixed}), "
            f"music={music_fixed}, pstarts={pstart_fixed}, speech={speech_fixed}, col={col_fixed}"
        )


def strip_material_normals(pk, target_names):
    """Remove Normal and NormalTransform property spans on specified SCX_basic_material exports."""
    target_low = {t.lower() for t in target_names}
    stripped = 0
    for e in pk.exps:
        if not e["data"] or pk.exp_cls_name(e).lower() != "scx_basic_material":
            continue
        if pk.name_str(e["nm"]).lower() not in target_low:
            continue
        d = e["data"]
        spans, _, none_start = parse_prop_spans(pk.names, d)
        out = bytearray()
        changed = False
        for nm, ps, pe in spans:
            if nm.lower() in ("normal", "normaltransform"):
                changed = True
                continue
            out += d[ps:pe]
        if changed:
            out += d[none_start:]
            e["data"] = bytes(out)
            e["ssz"] = len(e["data"])
            stripped += 1
    if stripped:
        print(f"  stripped Normal/NormalTransform from {stripped} material(s)")


def convert_map(map_name, out_name, stage=False, stage_dir=None):
    recon_dir = os.path.join(REPO, "SCDA_Coop_Maps", "reconstructed", map_name)
    out_root = os.path.join(REPO, "SCDA_Coop_Maps", "converted", out_name)
    if not os.path.isdir(recon_dir):
        raise FileNotFoundError(f"Reconstructed dir not found: {recon_dir}")
    if os.path.exists(out_root):
        shutil.rmtree(out_root)

    retail = RetailIndex()
    scl_path, _ = find_reconstructed_pkg(recon_dir, map_name.lower())
    if not scl_path:
        raise FileNotFoundError(f"Cannot find {map_name}.scl in {recon_dir}")

    havok_path = os.path.join(recon_dir, f"{map_name}_havok.bin")
    havok_payload = open(havok_path, "rb").read() if os.path.exists(havok_path) else None

    # 1. Discover transitive SCDA packages starting from <map>.scl
    parsed_pkgs = {}  # root_lower -> (src_path, ext, PackageData)
    queue = [map_name.lower()]
    while queue:
        r = queue.pop(0)
        if r in parsed_pkgs:
            continue
        pth, ext = find_reconstructed_pkg(recon_dir, r)
        if not pth:
            continue
        pk = PackageData(path=pth)
        parsed_pkgs[r] = (pth, ext, pk)
        for i in range(len(pk.imps)):
            dep = pk.imp_root_pkg(i).lower()
            if dep and dep not in parsed_pkgs:
                if dep in ("core", "engine", "echelon", "echelonai", "echeloncharacter", "echeloneffect", "echelongameobject", "echeloningredient"):
                    continue
                queue.append(dep)

    # 2. Decide which packages can stay pointing to CT retail vs must be converted from SCDA
    # Note: echelonpattern -> <out_name>_pat.u; colliding packages -> <out_name>_<orig>
    # First check per-package missing imports across ALL non-empty parsed packages (or their non-empty exports).
    missing_counts = {r: 0 for r in parsed_pkgs}
    for r, (_, _, pk) in parsed_pkgs.items():
        if r in ("core", "engine", "echelon", "echelonai", "echeloncharacter", "echeloneffect", "echelongameobject", "echeloningredient"):
            continue
        # Only inspect imports actually bound to SCDA content: check all imports of non-stub pkgs
        # (Wait: check if package has any non-empty export!)
        if not any(e["ssz"] > 0 for e in pk.exps) and r != map_name.lower():
            continue
        for i, im in enumerate(pk.imps):
            if im["out"] == 0 and pk.name_str(im["cc"]).lower() == "package":
                continue
            dep = pk.imp_root_pkg(i).lower()
            if dep not in missing_counts or dep == r:
                continue
            _, ct_exps = retail.get_exports(dep)
            full_path = pk.ref_path(-(i + 1)).lower()
            subpath = ".".join(full_path.split(".")[1:])
            objname = pk.name_str(im["nm"]).lower()
            if subpath not in ct_exps and objname not in ct_exps:
                missing_counts[dep] = missing_counts.get(dep, 0) + 1

    # Also force conversion of the map .scl itself, echelonpattern, and any non-CT package that
    # is imported at all by a converted package (even as empty stub if not in CT) so ULinkerLoad finds the file!
    needs_scda = {map_name.lower(), "echelonpattern"}
    changed = True
    while changed:
        changed = False
        for r in list(needs_scda):
            if r not in parsed_pkgs:
                continue
            _, _, pk = parsed_pkgs[r]
            for i in range(len(pk.imps)):
                dep = pk.imp_root_pkg(i).lower()
                if not dep or dep in needs_scda:
                    continue
                if dep in ("core", "engine", "echelon", "echelonai", "echeloncharacter", "echeloneffect", "echelongameobject", "echeloningredient"):
                    continue
                if dep not in parsed_pkgs:
                    continue
                ct_ext = retail.has_retail_pkg(dep)
                has_nonempty = any(e["ssz"] > 0 for e in parsed_pkgs[dep][2].exps)
                if ct_ext is None or missing_counts.get(dep, 0) > 0 or (dep in missing_counts and has_nonempty and dep in ("esam", "npc", "coopanims", "ingredients_obj", "gameobjects_obj", "sfx_tex", "scx_mus_common")):
                    needs_scda.add(dep)
                    changed = True

    # Build pkg_renames dict (orig_lower -> new_pkg_stem)
    # Prefix ALL SCDA converted packages so neither retail CT nor other SCDA maps ever collide!
    # E.g. scda_ice_a -> prefix scda_ice_a_ (or shorter s_ice_a_ if needed to stay <= 58 chars for UE2 64B FName)
    short_tag = out_name
    pkg_renames = {map_name.lower(): out_name, "echelonpattern": f"{out_name}_pat"}
    for r in sorted(needs_scda):
        if r in pkg_renames:
            continue
        candidate = f"{short_tag}_{r}"
        if len(candidate) > 58:
            # Deterministic short hash-free truncation or compact tag
            compact = short_tag.replace("scda_", "s_")
            candidate = f"{compact}_{r}"
            if len(candidate) > 58:
                candidate = candidate[:58]
        pkg_renames[r] = candidate

    # 3. Convert and write each needed package
    written = []
    for r in sorted(needs_scda):
        _, ext, pk = parsed_pkgs[r]
        out_stem = pkg_renames[r]
        dest_sub = EXT_DEST_DIR[ext]
        dest_dir = os.path.join(out_root, dest_sub)
        os.makedirs(dest_dir, exist_ok=True)
        out_file = os.path.join(dest_dir, out_stem + ext)

        is_scl = r == map_name.lower()
        if is_scl:
            patch_scda_level_actors(pk, parsed_pkgs)
        if r == "echelonpattern":
            vname = rename_variable_class(pk, map_name, out_name)
            print(f"  pattern variable class -> {vname}")
        if r == "x00_coop_actors_tex":
            strip_material_normals(pk, ("coop_player_a_all", "coop_player_a_flip"))
        default_anim_ref = None
        if ext == ".ukx" and "npc" in r and "skel" in r and "npc" in parsed_pkgs and "npc" in pkg_renames:
            npc_pk = parsed_pkgs["npc"][2]
            has_2h = any(
                npc_pk.exp_cls_name(e) == "MeshAnimation"
                and npc_pk.name_str(e["nm"]).lower() == "npc_base2h_anims"
                and e["ssz"] > 0
                for e in npc_pk.exps
            )
            anim_nm = "npc_base2h_anims" if has_2h else "npc_base_anims"
            npc_pkg_imp = _ensure_import(pk, "Core", "Package", 0, "npc")
            default_anim_ref = _ensure_import(pk, "Engine", "MeshAnimation", npc_pkg_imp, anim_nm)
        raw_out = serialize_pkg(
            pk,
            pkg_renames=pkg_renames,
            class_remap=SCDA_CLASS_MAP,
            havok_payload=havok_payload if is_scl else None,
            retail=retail,
            default_anim_ref=default_anim_ref,
        )
        with open(out_file, "wb") as f:
            f.write(raw_out)
        written.append((ext, out_stem + ext, len(raw_out), sum(1 for e in pk.exps if e["ssz"] > 0)))

    # 4. Companion files (.ini, _sound.occ, .int)
    meta_dir = os.path.join(REPO, "SCDA_Coop_Maps", "extracted", "meta")
    maps_out = os.path.join(out_root, "Data", "Maps")
    loc_out = os.path.join(out_root, "Data", "System", "Localization")
    os.makedirs(maps_out, exist_ok=True)
    os.makedirs(loc_out, exist_ok=True)

    ini_src = os.path.join(meta_dir, f"{map_name}.ini")
    if os.path.exists(ini_src):
        scda_map_identity = {"x01_coop_ice_a": 210}
        meta_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "map_metadata.json")
        if os.path.isfile(meta_file):
            try:
                with open(meta_file, "r", encoding="utf-8") as mf_f:
                    for mk, mv in (json.load(mf_f).get("maps") or {}).items():
                        if "identity" in mv:
                            scda_map_identity[mk.lower()] = int(mv["identity"])
            except Exception:
                pass
        txt = open(ini_src, "r", encoding="latin1").read()
        txt = re.sub(r"(?im)^MapName=.*$", f"MapName={out_name}", txt)
        txt = re.sub(r"(?im)^StartMap=.*$", f"StartMap={out_name}", txt)
        txt = re.sub(r"(?im)^LocalizationFile=.*$", f"LocalizationFile=Localization\\\\{out_name}", txt)
        if map_name.lower() in scda_map_identity:
            txt = re.sub(r"(?im)^Identity=\d+\s*$", f"Identity={scda_map_identity[map_name.lower()]}", txt)
        txt = re.sub(r"(?im)^Flag=Training\r?\n?", "", txt)
        txt = re.sub(r"(?im)(\|Training\b|\bTraining\|)", "", txt)
        if "esam" in pkg_renames:
            txt = re.sub(r"(?im)^SamMesh=ESam\.", f"SamMesh={pkg_renames['esam']}.", txt)
        with open(os.path.join(maps_out, f"{out_name}.ini"), "w", encoding="latin1", newline="\r\n") as f:
            f.write(txt)

    occ_src = os.path.join(meta_dir, f"{map_name}_sound.occ")
    if os.path.exists(occ_src):
        shutil.copy2(occ_src, os.path.join(maps_out, f"{out_name}_sound.occ"))

    int_src = os.path.join(meta_dir, f"{map_name}.int")
    pint_src = os.path.join(meta_dir, f"p_{map_name}.int")
    map_int = os.path.join(loc_out, f"{out_name}.int")
    if os.path.exists(int_src):
        base = open(int_src, "r", encoding="latin1").read().rstrip() + "\n"
    else:
        base = "[GENERAL]\nMapName={}\n".format(out_name)

    extra_interaction = {
        "KeyCard": "Take Keycard",
        "CardReader": "Use Card Reader",
        "TutorialTomoeNage": "Tomoe Nage",
        "TutorialHangOver": "Hang Over",
        "TutorialShortScale": "Short Scale",
        "TrainingShortScale": "Short Scale",
        "TomoeTraining": "Tomoe Nage",
        "Hang_over_hints": "Hang Over",
        "Hangover_move": "Hang Over",
    }
    base_low = base.lower()
    missing_ik = [f"{k}={v}" for k, v in extra_interaction.items() if f"\n{k.lower()}=" not in base_low]
    if missing_ik:
        m_inter = re.search(r"(?im)^\[Interaction\]\r?\n", base)
        if m_inter:
            nxt_sec = re.search(r"(?m)^\[", base[m_inter.end():])
            ins_at = m_inter.end() + nxt_sec.start() if nxt_sec else len(base)
            chunk = "\n".join(missing_ik) + "\n"
            while ins_at > m_inter.end() and base[ins_at - 1] == "\n":
                ins_at -= 1
            base = base[:ins_at] + ("\n" if ins_at > 0 and base[ins_at - 1] != "\n" else "") + chunk + base[ins_at:]
        else:
            base += "\n[Interaction]\n" + "\n".join(missing_ik) + "\n"

    if os.path.exists(pint_src):
        speech = open(pint_src, "r", encoding="latin1").read()
        shutil.copy2(pint_src, os.path.join(loc_out, f"P_{map_name}.int"))
        alias = speech.replace(f"[p_{map_name}_", f"[{out_name}_")
        body = base + "\n" + speech + "\n" + alias
    else:
        body = base
    with open(map_int, "w", encoding="latin1", newline="\n") as f:
        f.write(body)

    try:
        convert_loading.convert_loading_assets(map_name, out_name, out_root)
    except Exception as ex:
        print(f"  [WARN] Loading assets skipped for {map_name}: {ex}")

    manifest = {
        "map_name": map_name,
        "out_name": out_name,
        "pkg_renames": pkg_renames,
        "packages_written": [{"ext": e, "file": fn, "bytes": b, "non_empty_exports": ne} for e, fn, b, ne in written],
    }
    with open(os.path.join(out_root, "convert_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    if stage:
        dst_root = os.path.abspath(stage_dir) if stage_dir else REPO
        for root, _, files in os.walk(out_root):
            rel = os.path.relpath(root, out_root)
            if rel == ".":
                continue
            dst_dir = os.path.join(dst_root, rel)
            os.makedirs(dst_dir, exist_ok=True)
            for fn in files:
                dst_p = os.path.join(dst_dir, fn)
                low = fn.lower()
                short_prefix = out_name.replace("scda_", "s_").lower() + "_"
                # Safety guard: only overwrite files belonging to this converted SCDA map!
                if os.path.exists(dst_p) and not (
                    low.startswith(out_name.lower())
                    or low.startswith(short_prefix)
                    or low.startswith("load_map_coop_")
                    or low == f"p_{map_name.lower()}.int"
                ):
                    raise RuntimeError(f"Refusing to overwrite non-target file: {dst_p}")
                shutil.copy2(os.path.join(root, fn), dst_p)
        sys_loc = os.path.join(dst_root, "System", "Localization")
        os.makedirs(sys_loc, exist_ok=True)
        for fn in (f"{out_name}.int", f"P_{map_name}.int"):
            src = os.path.join(loc_out, fn)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(sys_loc, fn))

    return out_root, manifest


def main():
    ap = argparse.ArgumentParser(description="Convert reconstructed SCDA coop map to SCCT PC")
    ap.add_argument("map_name", help="Source SCDA map (e.g. x01_coop_ice_a)")
    ap.add_argument("--out-name", default=None, help="Target SCCT map stem (e.g. scda_ice_a)")
    ap.add_argument("--stage", action="store_true", help="Copy converted non-retail files into Chaos Theory Data/ and System/")
    ap.add_argument("--ct-dir", default=None, help="Target Chaos Theory install folder for --stage (defaults to workspace)")
    args = ap.parse_args()

    out_name = args.out_name
    if not out_name:
        m = re.match(r"^x\d+_coop_(.+)$", args.map_name.lower())
        out_name = f"scda_{m.group(1)}" if m else f"scda_{args.map_name.lower()}"

    out_root, mf = convert_map(args.map_name, out_name, stage=args.stage, stage_dir=args.ct_dir)
    print(f"Converted {args.map_name} -> {out_name} in {out_root}")
    print(f"Total packages written: {len(mf['packages_written'])}")
    for p in mf["packages_written"]:
        print(f"  {p['ext']:5s} {p['file']:48s} {p['bytes']:10d}B  nonEmpty={p['non_empty_exports']}")


if __name__ == "__main__":
    main()
