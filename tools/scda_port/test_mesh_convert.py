"""Verify mesh_convert on retail CT (PC round trip) and reconstructed SCDA (XB parse + PC re-parse)."""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mesh_convert as mc
from convert_package import PackageData, RetailIndex, parse_prop_spans


def main():
    ri = RetailIndex()
    ct = []
    for nm in ("npc", "esam", "coopanims", "ingredients_skel"):
        ct.append((nm, ri._load_retail_pkg(nm)[1], False))
    xb = []
    root = os.path.join(HERE, "..", "..", "SCDA_Coop_Maps", "reconstructed")
    for path in sorted(glob.glob(os.path.join(root, "*", "*", "*.ukx"))):
        xb.append((os.path.basename(path), PackageData(path), True))

    errs = 0
    n_ma = n_sk = 0
    for label, pk, is_xb in ct + xb:
        for e in pk.exps:
            cn = pk.exp_cls_name(e)
            d = e["data"]
            if not d or cn not in ("MeshAnimation", "SkeletalMesh"):
                continue
            nm = pk.name_str(e["nm"])
            try:
                _, pe, _ = parse_prop_spans(pk.names, d)
                if cn == "MeshAnimation":
                    parts = mc.parse_mesh_anim(d, pe, pk.fv, pk.lv, is_xb)
                    pc = d[:pe] + mc.emit_mesh_anim_pc(*parts)
                    if not is_xb:
                        assert pc == d, "PC round trip differs"
                    mc.parse_mesh_anim(pc, pe, pk.fv, pk.lv, False)
                    n_ma += 1
                else:
                    lod_end = mc.ulod_end(pe, d)
                    S = mc.parse_skeletal_mesh(d, lod_end, is_xb, pk.lv)
                    assert S["_end"] == len(d), f"consumed {S['_end']} != {len(d)}"
                    pc = mc.emit_skeletal_pc(S, nm.encode())
                    S2 = mc.parse_skeletal_mesh(pc, lod_end, False, 124)
                    assert S2["_end"] == len(pc), "re-parse of PC output failed"
                    if not is_xb:
                        # everything except the two random PC-only fields must round trip
                        assert len(pc) == len(d), "PC length differs"
                        for k in S:
                            if k not in ("u3d4", "u3d8"):
                                assert S[k] == S2[k], f"segment {k} differs"
                    n_sk += 1
            except Exception as ex:
                errs += 1
                print(f"FAIL {label}:{nm} ({cn}) -> {ex!r}")
    print(f"MeshAnimation OK={n_ma} SkeletalMesh OK={n_sk} errors={errs}")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
