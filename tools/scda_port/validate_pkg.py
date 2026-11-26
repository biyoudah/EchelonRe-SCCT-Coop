"""Validate converted SCCT PC packages (LicVer=124) for structural, offset, and import integrity."""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import convert_package as cp
import mesh_convert as mc
import reconstruct_packages as rp


def validate_texture_pc(pk, exp, errs):
    d = exp["data"]
    if not d:
        return
    nm = pk.name_str(exp["nm"])
    try:
        _, p, _ = cp.parse_prop_spans(pk.names, d)
        if p == len(d):
            return
        mc, p = rp.read_ci(d, p)
        if mc < 0 or mc > 32:
            errs.append(f"Tex {nm}: bad mip count {mc}")
            return
        for mi in range(mc):
            skip_off = struct.unpack_from("<I", d, p)[0]
            p += 4
            dsz, p = rp.read_ci(d, p)
            p += dsz
            expected_skip = exp["soff"] + p
            if skip_off != expected_skip:
                errs.append(f"Tex {nm} mip {mi}: SkipOffset {skip_off:#x} != expected {expected_skip:#x}")
            w, h, ub, vb = struct.unpack_from("<IIBB", d, p)
            p += 10
        if p != len(d):
            errs.append(f"Tex {nm}: end {p} != len {len(d)}")
    except Exception as ex:
        errs.append(f"Tex {nm}: exception {ex}")


def validate_static_mesh_pc(pk, exp, errs):
    d = exp["data"]
    if not d:
        return
    nm = pk.name_str(exp["nm"])
    try:
        spans, p, _ = cp.parse_prop_spans(pk.names, d)
        for pnm, _, _ in spans:
            if pnm.lower() == "isoptimized":
                errs.append(f"SM {nm}: leftover isOptimized property")
        p += 41
        vc, p = rp.read_ci(d, p)
        p += vc * 28 + 4
        ic, p = rp.read_ci(d, p)
        if ic > 0:
            idxs = struct.unpack_from(f"<{ic}H", d, p)
            bad_v = [idx for idx in idxs if idx >= vc]
            if bad_v:
                errs.append(f"SM {nm}: vertex index {bad_v[0]} >= vc {vc}")
        p += ic * 2 + 4
        wc, p = rp.read_ci(d, p)
        p += wc * 2 + 4
        _, has_cs = struct.unpack_from("<II", d, p)
        p += 8
        if has_cs:
            cc, p = rp.read_ci(d, p)
            p += cc * 4 + 4
        sc, p = rp.read_ci(d, p)
        for si in range(sc):
            f4, unk0, fi, mn, mx, np1, np2, unk = struct.unpack_from("<IIHHHHHI", d, p)
            p += 22
            mat, p = rp.read_ci(d, p)
            if f4 != 0:
                errs.append(f"SM {nm} sec {si}: PC Section f4={f4} != 0")
            if np1 > 0 and fi + np1 * 3 > ic:
                errs.append(f"SM {nm} sec {si}: fi+3*np={fi+np1*3} > ic={ic}")
            if mat < -len(pk.imps) or mat > len(pk.exps):
                errs.append(f"SM {nm} sec {si}: bad material ref {mat}")
        p += 4
        ct, p = rp.read_ci(d, p)
        if ct != 0:
            errs.append(f"SM {nm}: unexpected ct={ct}")
            return
        cv, p = rp.read_ci(d, p)
        p += cv * 12
        cm, p = rp.read_ci(d, p)
        for _ in range(cm):
            mref, p = rp.read_ci(d, p)
            if mref < -len(pk.imps) or mref > len(pk.exps):
                errs.append(f"SM {nm}: bad CollisionMat ref {mref}")
        cb, p = rp.read_ci(d, p)
        p += cb + 4
        cn, p = rp.read_ci(d, p)
        for _ in range(cn):
            nw, p = rp.read_ci(d, p)
            p += nw * 2 + 25
        p += 4
        skip_off = struct.unpack_from("<I", d, p)[0]
        p += 4
        rc, p = rp.read_ci(d, p)
        expected_skip = exp["soff"] + p
        if skip_off != expected_skip:
            errs.append(f"SM {nm}: RawTriangles SkipOffset {skip_off:#x} != {expected_skip:#x}")
        iv, u0, u1b4, u1b8 = struct.unpack_from("<4I", d, p)
        p += 16
        c0, p = rp.read_ci(d, p)
        c1, p = rp.read_ci(d, p)
        p += 4
        c2, p = rp.read_ci(d, p)
        p += c2 * 4
        c3, p = rp.read_ci(d, p)
        if p != len(d):
            errs.append(f"SM {nm}: tail parse end {p} != len {len(d)}")
    except Exception as ex:
        errs.append(f"SM {nm}: exception {ex}")


def _read_fstring(b, p):
    sl, p = rp.read_ci(b, p)
    if sl == 0:
        return "", p
    if sl < 0:
        n = (-sl) * 2
        return b[p : p + n].decode("utf-16-le", "replace").rstrip("\x00"), p + n
    return b[p : p + sl].decode("latin1", "replace").rstrip("\x00"), p + sl


def validate_level_pc(pk, exp, errs):
    d = exp["data"]
    nm = pk.name_str(exp["nm"])
    try:
        _, p, _ = cp.parse_prop_spans(pk.names, d)
        dbnum, _ = struct.unpack_from("<ii", d, p)
        p += 8
        for _ in range(dbnum):
            _, p = rp.read_ci(d, p)
        for _ in range(4):
            _, p = _read_fstring(d, p)
        op_cnt, p = rp.read_ci(d, p)
        for _ in range(op_cnt):
            _, p = _read_fstring(d, p)
        p += 8
        _, p = rp.read_ci(d, p)
        p += 4
        _, p = rp.read_ci(d, p)
        for _ in range(16):
            _, p = rp.read_ci(d, p)
        _, p = _read_fstring(d, p)
        for _tree_i in range(2):
            p += 24
            cnt, p = rp.read_ci(d, p)
            for _ in range(cnt):
                tp = d[p]
                p += 1
                if tp == 3:
                    _, p = rp.read_ci(d, p)
                elif tp in (1, 2, 4, 5, 6, 7, 8, 16, 32):
                    p += 4
                    _, p = rp.read_ci(d, p)
                    p += 24
                    if tp in (5, 16, 32):
                        p += 12
                else:
                    errs.append(f"Level {nm}: invalid PC FAABTObject type {tp}")
                    return
            def _skip_node(pos):
                nt = d[pos]
                pos += 1
                if nt == 1:
                    return pos
                if nt in (2, 4, 8):
                    return _skip_node(_skip_node(pos + 4))
                if nt == 16:
                    lc, pos = rp.read_ci(d, pos)
                    return pos + 4 * lc
                raise ValueError(f"Bad FAABTNode type {nt}")
            p = _skip_node(p)
        nt, ns = struct.unpack_from("<2I", d, p)
        p += 8
        if nt != 0 or ns != 0:
            errs.append(f"Level {nm}: unexpected PC preload nt={nt} ns={ns}")
        _, p = rp.read_ci(d, p)
        ac, p = rp.read_ci(d, p)
        for _ in range(ac):
            _, p = rp.read_ci(d, p)
        if p != len(d):
            errs.append(f"Level {nm}: tail end {p} != len {len(d)}")
    except Exception as ex:
        errs.append(f"Level {nm}: exception {ex}")


def validate_mesh_anim_pc(pk, exp, errs):
    d = exp["data"]
    if not d:
        return
    nm = pk.name_str(exp["nm"])
    try:
        _, pe, _ = cp.parse_prop_spans(pk.names, d)
        mc.parse_mesh_anim(d, pe, pk.fv, pk.lv, False)
    except Exception as ex:
        errs.append(f"MeshAnimation {nm}: {ex}")


def validate_skeletal_mesh_pc(pk, exp, errs):
    d = exp["data"]
    if not d:
        return
    nm = pk.name_str(exp["nm"])
    try:
        _, pe, _ = cp.parse_prop_spans(pk.names, d)
        S = mc.parse_skeletal_mesh(d, mc.ulod_end(pe, d), False, pk.lv)
        if S["_end"] != len(d):
            errs.append(f"SkeletalMesh {nm}: end {S['_end']} != len {len(d)}")
        if "xb208" in S:
            errs.append(f"SkeletalMesh {nm}: Xbox Lic>=127 dword left in PC body")
    except Exception as ex:
        errs.append(f"SkeletalMesh {nm}: {ex}")


def validate_dir(conv_dir):
    retail = cp.RetailIndex()
    ct_exe_bytes = open(os.path.join(REPO, "System", "splintercell3.exe"), "rb").read()
    conv_pkgs = {}  # stem_lower -> (path, ext, PackageData, raw)
    for root, _, files in os.walk(conv_dir):
        for fn in files:
            ext = os.path.splitext(fn)[1].lower()
            if ext in (".scl", ".u", ".utx", ".usx", ".ukx", ".uax"):
                fp = os.path.join(root, fn)
                raw = open(fp, "rb").read()
                pk = cp.PackageData(raw=raw)
                stem = os.path.splitext(fn)[0].lower()
                conv_pkgs[stem] = (fp, ext, pk, raw)

    conv_export_sets = {}
    for stem, (_, _, pk, _) in conv_pkgs.items():
        conv_export_sets[stem] = {pk.ref_path(e["idx"]).lower() for e in pk.exps} | {pk.name_str(e["nm"]).lower() for e in pk.exps}

    total_errs = []
    tex_ok = sm_ok = mod_ok = anim_ok = skel_ok = pkg_ok = 0
    for stem, (fp, ext, pk, raw) in sorted(conv_pkgs.items()):
        errs = []
        if pk.fv != 100 or pk.lv != 124 or pk.pf != 0:
            errs.append(f"Bad version/flags: fv={pk.fv} lv={pk.lv} pf={pk.pf:#x}")

        # Verify table / body contiguity & EOF / Havok trailer
        for e in pk.exps:
            if e["ssz"] > 0 and (e["soff"] < 0 or e["soff"] + e["ssz"] > len(raw)):
                errs.append(f"Export {pk.name_str(e['nm'])} OOB: {e['soff']}+{e['ssz']} > {len(raw)}")
            if len(pk.name_str(e["nm"])) > 63:
                errs.append(f"Name too long: {pk.name_str(e['nm'])}")
            cn = pk.exp_cls_name(e)
            if e["ssz"] > 0:
                if cn in ("Texture", "VolumeTexture"):
                    pre = len(errs)
                    validate_texture_pc(pk, e, errs)
                    if len(errs) == pre:
                        tex_ok += 1
                elif cn == "StaticMesh":
                    pre = len(errs)
                    validate_static_mesh_pc(pk, e, errs)
                    if len(errs) == pre:
                        sm_ok += 1
                elif cn == "Model":
                    try:
                        cp.walk_model_data(pk.names, e["data"], is_xb=False)
                        mod_ok += 1
                    except Exception as ex:
                        errs.append(f"UModel {pk.name_str(e['nm'])}: {ex}")
                elif cn == "Level":
                    validate_level_pc(pk, e, errs)
                elif cn == "MeshAnimation":
                    pre = len(errs)
                    validate_mesh_anim_pc(pk, e, errs)
                    if len(errs) == pre:
                        anim_ok += 1
                elif cn == "SkeletalMesh":
                    pre = len(errs)
                    validate_skeletal_mesh_pc(pk, e, errs)
                    if len(errs) == pre:
                        skel_ok += 1
                elif cn in ("Class", "State", "Function", "Struct"):
                    try:
                        cp.walk_ustruct_pc(pk.names, pk.exp_cls_name, e, e["data"])
                    except Exception as ex:
                        errs.append(f"{cn} {pk.name_str(e['nm'])}: {ex}")
                elif cn in ("IntProperty", "StrProperty", "BoolProperty", "FloatProperty", "NameProperty",
                            "ObjectProperty", "StructProperty", "ByteProperty", "ArrayProperty",
                            "ClassProperty", "FixedArrayProperty", "MapProperty"):
                    try:
                        d = e["data"]
                        _, p, _ = cp.parse_prop_spans(pk.names, d)
                        _, p = rp.read_ci(d, p)  # SuperField
                        _, p = rp.read_ci(d, p)  # Next
                        p += 6  # ArrayDim + PropertyFlags
                        _, p = rp.read_ci(d, p)  # Category
                        _, p = rp.read_ci(d, p)  # ExtraUnk
                        if cn in ("ObjectProperty", "StructProperty", "ByteProperty", "ArrayProperty"):
                            _, p = rp.read_ci(d, p)
                        elif cn in ("ClassProperty", "FixedArrayProperty", "MapProperty"):
                            _, p = rp.read_ci(d, p)
                            _, p = rp.read_ci(d, p)
                        if p != len(d):
                            errs.append(f"{cn} {pk.name_str(e['nm'])}: end {p} != {len(d)}")
                    except Exception as ex:
                        errs.append(f"{cn} {pk.name_str(e['nm'])}: {ex}")

        if ext == ".scl":
            hk_sz, hk_ver, hk_magic = struct.unpack_from("<III", raw, len(raw) - 12)
            if hk_ver != 1 or hk_magic != 0xDEADF00F:
                errs.append(f".scl bad Havok trailer: sz={hk_sz} ver={hk_ver} magic={hk_magic:#x}")
            hk_start = len(raw) - 12 - hk_sz
            if hk_start < 0 or raw[hk_start : hk_start + 4] != b"\x33\x5a\x0d\xf0":
                errs.append(f".scl missing 0xF00D5A33 Havok start signature at {hk_start:#x}")

        # Validate EVERY Import resolves cleanly to either conv_pkgs or retail!
        for i, im in enumerate(pk.imps):
            rpkg = pk.imp_root_pkg(i).lower()
            if len(rpkg) > 63:
                errs.append(f"Root pkg name > 63 chars: {rpkg}")
            if im["out"] == 0 and pk.name_str(im["cc"]).lower() == "package":
                if rpkg not in conv_pkgs and retail.has_retail_pkg(rpkg) is None:
                    errs.append(f"Missing external package: {rpkg}")
                continue
            full_p = pk.ref_path(-(i + 1)).lower()
            sub = ".".join(full_p.split(".")[1:])
            obj = pk.name_str(im["nm"]).lower()
            if rpkg in conv_export_sets:
                es = conv_export_sets[rpkg]
                if sub not in es and obj not in es:
                    errs.append(f"Unresolved import in conv {rpkg}: {full_p}")
            else:
                src, es = retail.get_exports(rpkg)
                if src is None:
                    errs.append(f"Import pkg not found anywhere: {rpkg} ({full_p})")
                elif sub not in es and obj not in es:
                    # Check if Core/Engine C++ native class/property registered in splintercell3.exe
                    if rpkg in ("core", "engine"):
                        u16 = (pk.name_str(im["nm"]).encode("utf-16-le") + b"\x00\x00")
                        if u16 in ct_exe_bytes:
                            continue
                    errs.append(f"Unresolved import in retail {src}: {full_p}")

        if errs:
            total_errs.extend([(os.path.basename(fp), er) for er in errs])
        else:
            pkg_ok += 1

    print(f"Validated {conv_dir}:")
    print(f"  Packages OK: {pkg_ok}/{len(conv_pkgs)} | Textures OK: {tex_ok} | StaticMeshes OK: {sm_ok} | Models OK: {mod_ok} | MeshAnimations OK: {anim_ok} | SkeletalMeshes OK: {skel_ok}")
    print(f"  Errors: {len(total_errs)}")
    for fn, er in total_errs[:25]:
        print(f"    [{fn}] {er}")
    return len(total_errs) == 0


def main():
    ap = argparse.ArgumentParser(description="Validate converted SCCT PC map directory")
    ap.add_argument("conv_dir", help="Path to converted/<out_name> directory")
    args = ap.parse_args()
    ok = validate_dir(args.conv_dir)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
