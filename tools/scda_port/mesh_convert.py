"""XB (SCDA Xbox, LicVer 127) -> PC (SCCT, LicVer 124) conversion of UMeshAnimation and USkeletalMesh.

All layouts were derived from disassembly of default.xbe and splintercell3.exe and verified
against every MeshAnimation / SkeletalMesh export of retail CT (byte exact PC round trip) and of
the reconstructed SCDA x01_coop_ice_a packages (consumed == len(data)).
See docs/scda_coop_port/01_lin_format_and_engine_diff.md section 5.6.
"""

import struct
import zlib

import reconstruct_packages as rp


# ----------------------------------------------------------------------------------------------
# LZO1X decompressor (used by XB MotionChunk when g_lin2 != 0)
# ----------------------------------------------------------------------------------------------
def lzo1x_decompress(src):
    if not src:
        return b""
    out = bytearray()
    ip = 0
    n = len(src)
    t = src[ip]
    if t > 17:
        ip += 1
        cnt = t - 17
        out.extend(src[ip : ip + cnt])
        ip += cnt
        if cnt < 4:
            t = src[ip]
            ip += 1
            state = "match"
        else:
            state = "first_lit"
    else:
        state = "top"

    while ip < n:
        if state == "top":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            cnt = t + 3
            out.extend(src[ip : ip + cnt])
            ip += cnt
            state = "first_lit"
        elif state == "first_lit":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            m_pos = len(out) - (1 + 0x0800 + (t >> 2) + (src[ip] << 2))
            ip += 1
            for k in range(3):
                out.append(out[m_pos + k])
            state = "match_done"
        elif state == "match":
            if t >= 64:
                m_pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                ip += 1
                ln = (t >> 5) - 1 + 2
            elif t >= 32:
                t &= 31
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 31 + src[ip]
                    ip += 1
                m_pos = len(out) - 1 - ((src[ip] >> 2) + (src[ip + 1] << 6))
                ip += 2
                ln = t + 2
            elif t >= 16:
                m_pos = len(out) - ((t & 8) << 11)
                t &= 7
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 7 + src[ip]
                    ip += 1
                m_pos -= (src[ip] >> 2) + (src[ip + 1] << 6)
                ip += 2
                if m_pos == len(out):
                    break
                m_pos -= 0x4000
                ln = t + 2
            else:
                m_pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                ln = 2
            for _ in range(ln):
                out.append(out[m_pos])
                m_pos += 1
            state = "match_done"
        else:  # match_done
            t = src[ip - 2] & 3
            if t == 0:
                state = "top"
                continue
            out.extend(src[ip : ip + t])
            ip += t
            t = src[ip]
            ip += 1
            state = "match"
    return bytes(out)


# ----------------------------------------------------------------------------------------------
# UMeshAnimation
# ----------------------------------------------------------------------------------------------
def parse_mesh_anim(d, prop_end, ver, lic, is_xb):
    p = prop_end
    iver, = struct.unpack_from("<I", d, p)
    p += 4
    nb, p = rp.read_ci(d, p)
    ref_bones = []
    for _ in range(nb):
        bnm, p = rp.read_ci(d, p)
        flags, p_idx = struct.unpack_from("<II", d, p)
        p += 8
        ref_bones.append((bnm, flags, p_idx))

    nm, p = rp.read_ci(d, p)
    moves = []
    for _ in range(nm):
        hdr12 = d[p : p + 12]
        p += 12
        n1_tracks = []
        if not is_xb:
            n1, p = rp.read_ci(d, p)
            for _ in range(n1):
                w0, w1, w2 = struct.unpack_from("<HHH", d, p)
                p += 6
                n1_tracks.append((w0, w1, w2, d[p : p + w0 + w1 + w2]))
                p += w0 + w1 + w2
        rw0, rw1, rw2 = struct.unpack_from("<HHH", d, p)
        p += 6
        rpay = d[p : p + rw0 + rw1 + rw2]
        p += rw0 + rw1 + rw2
        n2, p = rp.read_ci(d, p)
        n2_tracks = []
        for _ in range(n2):
            w0, w1, w2 = struct.unpack_from("<HHH", d, p)
            p += 6
            n2_tracks.append((w0, w1, w2, d[p : p + w0 + w1 + w2]))
            p += w0 + w1 + w2
        if is_xb:
            psz, = struct.unpack_from("<I", d, p)
            p += 4
            pbuf = d[p : p + psz]
            p += psz
            if psz > 0:
                raw = lzo1x_decompress(pbuf)
                cnt, = struct.unpack_from("<I", raw, 0)
                q = 4
                meta = []
                for _ in range(cnt):
                    w0, w1, w2, wsum, _dummy = struct.unpack_from("<HHHHI", raw, q)
                    q += 12
                    if w0 + w1 + w2 != wsum:
                        raise ValueError("MotionChunk LZO track size mismatch")
                    meta.append((w0, w1, w2, wsum))
                for w0, w1, w2, wsum in meta:
                    n1_tracks.append((w0, w1, w2, raw[q : q + wsum]))
                    q += wsum
                if q != len(raw):
                    raise ValueError("MotionChunk LZO leftover bytes")
        moves.append((hdr12, n1_tracks, (rw0, rw1, rw2, rpay), n2_tracks))

    na, p = rp.read_ci(d, p)
    anims = []
    for _ in range(na):
        nm1, p = rp.read_ci(d, p)
        if is_xb and lic >= 0x10:
            _nm2, p = rp.read_ci(d, p)  # XB-only second FName
        ngrp, p = rp.read_ci(d, p)
        grps = []
        for _ in range(ngrp):
            g, p = rp.read_ci(d, p)
            grps.append(g)
        f1, f2 = d[p : p + 4], d[p + 4 : p + 8]
        p += 8
        nnot, p = rp.read_ci(d, p)
        notifs = []
        for _ in range(nnot):
            ftime = d[p : p + 4]
            p += 4
            a, p = rp.read_ci(d, p)
            b, p = rp.read_ci(d, p)
            c, p = rp.read_ci(d, p)
            notifs.append((ftime, a, b, c))
        tail = d[p : p + 10]
        p += 10
        arr1, arr2 = [], []
        c1, p = rp.read_ci(d, p)
        for _ in range(c1):
            arr1.append(d[p : p + 13])
            p += 13
        c2, p = rp.read_ci(d, p)
        for _ in range(c2):
            arr2.append(d[p : p + 13])
            p += 13
        anims.append((nm1, grps, f1, f2, notifs, tail, arr1, arr2))
    if p != len(d):
        raise ValueError(f"MeshAnimation consumed {p} != {len(d)}")
    return iver, ref_bones, moves, anims


def emit_mesh_anim_pc(iver, ref_bones, moves, anims):
    out = bytearray(struct.pack("<I", iver))
    out += rp.write_ci(len(ref_bones))
    for bnm, flags, p_idx in ref_bones:
        out += rp.write_ci(bnm) + struct.pack("<II", flags, p_idx)
    out += rp.write_ci(len(moves))
    for hdr12, n1_tracks, (rw0, rw1, rw2, rpay), n2_tracks in moves:
        out += hdr12 + rp.write_ci(len(n1_tracks))
        for w0, w1, w2, pay in n1_tracks:
            out += struct.pack("<HHH", w0, w1, w2) + pay
        out += struct.pack("<HHH", rw0, rw1, rw2) + rpay
        out += rp.write_ci(len(n2_tracks))
        for w0, w1, w2, pay in n2_tracks:
            out += struct.pack("<HHH", w0, w1, w2) + pay
    out += rp.write_ci(len(anims))
    for nm1, grps, f1, f2, notifs, tail, arr1, arr2 in anims:
        out += rp.write_ci(nm1) + rp.write_ci(len(grps))
        for g in grps:
            out += rp.write_ci(g)
        out += f1 + f2 + rp.write_ci(len(notifs))
        for ftime, a, b, c in notifs:
            out += ftime + rp.write_ci(a) + rp.write_ci(b) + rp.write_ci(c)
        out += tail
        out += rp.write_ci(len(arr1)) + b"".join(arr1)
        out += rp.write_ci(len(arr2)) + b"".join(arr2)
    return bytes(out)


# ----------------------------------------------------------------------------------------------
# USkeletalMesh
# ----------------------------------------------------------------------------------------------
def _aspan(d, p, esz):
    """TArray<fixed esz>: returns (bytes incl. count, end)."""
    n, q = rp.read_ci(d, p)
    q += n * esz
    return d[p:q], q


def _ci_span(d, p):
    n, q = rp.read_ci(d, p)
    for _ in range(n):
        _, q = rp.read_ci(d, q)
    return d[p:q], q


def _ci_one(d, p):
    _, q = rp.read_ci(d, p)
    return d[p:q], q


def _fstring(d, p):
    sl, q = rp.read_ci(d, p)
    if sl > 0:
        q += sl
    elif sl < 0:
        q += -sl * 2
    return q


# element sizes of the nine fixed-record arrays (identical on XB and PC)
_A9 = {"A": 16, "B": 20, "C": 16, "D": 28, "E": 120, "F": 136, "G": 8, "H": 8, "I": 76}
_XB_ORDER = "ABCDEFGHI"
_PC_ORDER = "DEFGHICAB"


def _lod_models(d, p):
    s = p
    n, p = rp.read_ci(d, p)
    for _ in range(n):
        p += 18
        m, p = rp.read_ci(d, p)
        p += 2 * m + 2 + 4
    return d[s:p], p


def _blk358(d, p):
    """u32 n1, u32 n0, TArray<FString>, FString, n0*n1*28B."""
    s = p
    n1, n0 = struct.unpack_from("<II", d, p)
    p += 8
    cnt, p = rp.read_ci(d, p)
    for _ in range(cnt):
        p = _fstring(d, p)
    p = _fstring(d, p)
    p += n0 * n1 * 28
    return d[s:p], p


def _bhas(d, p):
    s = p
    has, = struct.unpack_from("<I", d, p)
    p += 4
    if has:
        n, p = rp.read_ci(d, p)
        for _ in range(n):
            p += 12
            m, p = rp.read_ci(d, p)
            p += 16 * m
    return d[s:p], p


def _nested16(d, p):
    s = p
    n, p = rp.read_ci(d, p)
    for _ in range(n):
        m, p = rp.read_ci(d, p)
        for _ in range(m):  # element: FName ci, u32, u32, u8 (Ver >= 0x5d)
            _, p = rp.read_ci(d, p)
            p += 9
    return d[s:p], p


def _ref_skeleton(d, p):
    s = p
    n, p = rp.read_ci(d, p)
    for _ in range(n):
        _, p = rp.read_ci(d, p)
        p += 56
    return d[s:p], p


def _a1ec(d, p):
    s = p
    n, p = rp.read_ci(d, p)
    for _ in range(n):
        c, p = rp.read_ci(d, p)
        p += 2 * c + 4
    return d[s:p], p


def parse_skeletal_mesh(d, lod_end, is_xb, lic=None):
    """Split a SkeletalMesh export body into byte segments. lod_end = offset after ULodMesh body.

    lic is the package LicenseeVersion (archive+0x0C). SCDA is 127, SCCT is 124.
    The Xbox-only dword at object+0x208 is serialized only when lic >= 0x7F (XB 0x223018).
    IndexBuffer does not include the g_lin1 8-byte CacheId (XB 0x28F49D / 0x5EFC6): the
    reconstructed exports of LockPick, coop_player_a and npc_coop_iceland start at the u16
    TArray, and PC's lin global (0x112327E8) is zero so CT files omit it too.
    """
    if lic is None:
        lic = 127 if is_xb else 124
    S = {}
    p = lod_end
    S["head"] = d[:p]
    S["ver"] = d[p : p + 4]
    p += 4
    s = p
    _, p = _aspan(d, p, 12)
    _, p = _aspan(d, p, 12)
    S["points_tmp"] = d[s:p]

    if is_xb:
        S["refskel"], p = _ref_skeleton(d, p)
        S["defanim"], p = _ci_one(d, p)
        S["sdep"] = d[p : p + 4]
        p += 4
        S["a1ec"], p = _a1ec(d, p)
        S["a1f8"], p = _aspan(d, p, 4)
        S["alias"], p = _ci_span(d, p)
        S["bone"], p = _ci_span(d, p)
        S["coord"], p = _aspan(d, p, 48)
        for k in _XB_ORDER:
            S[k], p = _aspan(d, p, _A9[k])
    else:
        S["u3d4"] = d[p : p + 4]
        p += 4
        S["alias"], p = _ci_span(d, p)
        S["bone"], p = _ci_span(d, p)
        S["coord"], p = _aspan(d, p, 48)
        S["u3d8"] = d[p : p + 4]
        p += 4
        S["sdep"] = d[p : p + 4]
        p += 4
        S["defanim"], p = _ci_one(d, p)
        S["a1f8"], p = _aspan(d, p, 4)
        S["a1ec"], p = _a1ec(d, p)
        S["refskel"], p = _ref_skeleton(d, p)
        for k in _PC_ORDER:
            S[k], p = _aspan(d, p, _A9[k])
    S["lods"], p = _lod_models(d, p)
    S["blk358"], p = _blk358(d, p)
    S["bhas"], p = _bhas(d, p)
    if not is_xb:
        S["u3dc"] = d[p : p + 4]
        p += 4
    S["vtx"], p = _aspan(d, p, 36)
    S["vtxrev"] = d[p : p + 4]
    p += 4
    # g_lin1 == 0 in these packages: no 8-byte CacheId before the u16 index TArray.
    S["ib"], p = _aspan(d, p, 2)
    S["ibrev"] = d[p : p + 4]
    p += 4
    S["u1e8"] = d[p : p + 4]
    p += 4
    if is_xb and lic >= 0x7F:
        S["xb208"] = d[p : p + 4]
        p += 4
    S["nested"], p = _nested16(d, p)
    S["_end"] = p
    return S


def emit_skeletal_pc(S, seed, default_anim_ref=None):
    r1 = zlib.crc32(seed + b"a") & 0x7FFF
    r2 = zlib.crc32(seed + b"b") & 0x7FFF
    defanim = S["defanim"]
    if default_anim_ref is not None and defanim == b"\x00":
        defanim = rp.write_ci(default_anim_ref)
    out = bytearray(S["head"] + S["ver"] + S["points_tmp"])
    out += struct.pack("<f", float(r1))
    out += S["alias"] + S["bone"] + S["coord"]
    out += struct.pack("<I", r2)
    out += S["sdep"] + defanim + S["a1f8"] + S["a1ec"] + S["refskel"]
    for k in _PC_ORDER:
        out += S[k]
    out += S["lods"] + S["blk358"] + S["bhas"] + struct.pack("<I", 1)
    out += S["vtx"] + S["vtxrev"] + S["ib"] + S["ibrev"] + S["u1e8"] + S["nested"]
    return bytes(out)


def ulod_end(names_prop_end, d):
    """Offset after the ULodMesh body (identical on XB and PC), starting at property end."""
    p = names_prop_end + 41  # UPrimitive: FBox 25 + FSphere 16
    v, _fl = struct.unpack_from("<II", d, p)
    p += 8
    for _ in range(2):
        c, p = rp.read_ci(d, p)
        p += 4 * c
    for _ in range(2 if v >= 3 else 1):
        c, p = rp.read_ci(d, p)
        for _ in range(c):
            _, p = rp.read_ci(d, p)
    p += 36
    for esz in (2, 2, 8, 2, 10, 8):
        c, p = rp.read_ci(d, p)
        p += esz * c
    p += 24
    return p
