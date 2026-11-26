"""
Convert the SCDA Xbox Iceland co-op DareAudio bank into Chaos Theory PC format.

Retail MAPS.SM0 / STREAM.SS0 / MAPS.LM0 are copied, never modified.
The copies are SCDA_MAPS.SM0, SCDA_STREAM.SS0 and SCDA_MAPS.LM0.
ct_probe redirects those three filenames when the game opens them.

PC bank (DareAudio v18):
  directory entry 0x34 bytes, name at +0x10
  bank header 0x24, Sec1 event 104 bytes, Sec2 wave 96 bytes
  type-1 waves are 16-bit PCM in STREAM.SS0 (codec 1)

Xbox bank (DareAudio v22):
  same 0x34 directory entries
  bank header 0x2C, Sec1 event 72 bytes, Sec2 wave 88 bytes
  type-1 waves are Xbox IMA ADPCM in STREAM.SS2 (36 bytes -> 64 samples mono)
"""

import argparse
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso_extract as ie

ISO = ie.ISO_PATH
BANK_NAME = "x01_coop_ice_a"
OUT_NAME = "scda_ice_a"

STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def read_at(f, off, n):
    f.seek(off)
    data = f.read(n)
    if len(data) != n:
        raise IOError(f"short read at {off:#x}: {len(data)}/{n}")
    return data


def decode_nibble(sample, index, nibble):
    step = STEP[index]
    delta = step >> 3
    if nibble & 1:
        delta += step >> 2
    if nibble & 2:
        delta += step >> 1
    if nibble & 4:
        delta += step
    sample += -delta if (nibble & 8) else delta
    if sample > 32767:
        sample = 32767
    elif sample < -32768:
        sample = -32768
    index += INDEX[nibble & 15]
    if index < 0:
        index = 0
    elif index > 88:
        index = 88
    return sample, index


def decode_mono_block(block):
    sample = struct.unpack_from("<h", block, 0)[0]
    index = block[2]
    if index > 88:
        index = 88
    out = [sample]
    for byte in block[4:36]:
        sample, index = decode_nibble(sample, index, byte & 0xF)
        out.append(sample)
        if len(out) == 64:
            return out
        sample, index = decode_nibble(sample, index, byte >> 4)
        out.append(sample)
        if len(out) == 64:
            return out
    return out[:64]


def decode_stereo_block(block):
    sl = struct.unpack_from("<h", block, 0)[0]
    il = block[2] if block[2] <= 88 else 88
    sr = struct.unpack_from("<h", block, 4)[0]
    ir = block[6] if block[6] <= 88 else 88
    left = [sl]
    right = [sr]
    pos = 8
    while pos + 8 <= 72 and len(left) < 64:
        for byte in block[pos:pos + 4]:
            sl, il = decode_nibble(sl, il, byte & 0xF)
            left.append(sl)
            if len(left) == 64:
                break
            sl, il = decode_nibble(sl, il, byte >> 4)
            left.append(sl)
            if len(left) == 64:
                break
        pos += 4
        if len(left) == 64:
            break
        for byte in block[pos:pos + 4]:
            sr, ir = decode_nibble(sr, ir, byte & 0xF)
            right.append(sr)
            if len(right) == 64:
                break
            sr, ir = decode_nibble(sr, ir, byte >> 4)
            right.append(sr)
            if len(right) == 64:
                break
        pos += 4
    frames = []
    for i in range(64):
        frames.append(left[i] if i < len(left) else 0)
        frames.append(right[i] if i < len(right) else 0)
    return frames


def decode_adpcm(blob, channels):
    block = 36 if channels == 1 else 72
    if channels not in (1, 2) or len(blob) < block:
        return b""
    nblocks = len(blob) // block
    pcm = []
    decode = decode_mono_block if channels == 1 else decode_stereo_block
    for i in range(nblocks):
        pcm.extend(decode(blob[i * block:(i + 1) * block]))
    out = bytearray(len(pcm) * 2)
    for i, s in enumerate(pcm):
        struct.pack_into("<h", out, i * 2, int(s))
    return bytes(out)


def parse_dir(data, dir_off, count):
    ents = []
    for i in range(count):
        o = dir_off + i * 0x34
        flag, zero, off, size = struct.unpack_from("<IIII", data, o)
        name = data[o + 0x10:o + 0x30].split(b"\x00", 1)[0].decode("latin1")
        tail = struct.unpack_from("<I", data, o + 0x30)[0]
        ents.append({"name": name, "off": off, "size": size, "flag": flag, "zero": zero, "tail": tail})
    return ents


def find_bank(iso_f, container_off, name):
    hdr = read_at(iso_f, container_off, 16)
    ver_a, ver_b, dir_off, count = struct.unpack_from("<HHII", hdr, 0)
    directory = read_at(iso_f, container_off + dir_off, count * 0x34)
    # directory buffer is only the entries; parse_dir expects dir_off relative to `data`
    fake = bytearray(dir_off) + directory
    ents = parse_dir(fake, dir_off, count)
    for ent in ents:
        if ent["name"].lower() == name.lower():
            blob = read_at(iso_f, container_off + ent["off"], ent["size"])
            return ver_b, ent, blob, ents
    raise KeyError(name)


def expand_sec1(rec72):
    words = list(struct.unpack("<18I", rec72))[:7] + [
        0x60000, 0xFFFFFFFF, 0x1D0000, 0x1E0000, 0x0313B308,
        0x64, 1, 1, 0, 1, 1,
        0x047B4258, 1, 0, 1, 0, 0, 0, 0,
    ]
    return struct.pack("<26I", *words)


def expand_sec2(rec88, pcm_off, pcm_size, samples, rate, channels, stream_str_off=None, looping=False):
    src = list(struct.unpack("<22I", rec88))
    # Insert 2 dwords before xb[10] (samples/size) so xb[10..21] align to CT PC slots +0x30..+0x5c.
    words = src[:10] + [0, 0] + src[10:]
    if (src[1] == 1 or looping) and pcm_size:
        words[1] = 1
        words[2] = pcm_size
        words[4] = pcm_off
        words[6] = 1
        words[7] = 0 if looping else 1
        words[8] = 1
        words[9] = 1
        words[10] = 0
        words[11] = 1
        words[12] = samples
        words[13] = pcm_size
        words[14] = 0
        words[15] = 0
        words[16] = rate * channels * 2
        words[17] = rate
        words[18] = 16
        words[19] = channels
        words[20] = 1
        if stream_str_off is not None:
            words[21] = stream_str_off
        words[22] = 1
        words[23] = 0xFFFFFFFF
    elif src[1] == 3:
        # Switch: Xbox extra dword at src[6] shifted count to src[7] and mode/param to src[9].
        words[6] = src[7]
        words[7] = 0
        words[8] = src[9]
        words[9] = 0
    elif src[1] == 7:
        # Layer: Xbox extra dword at src[6] shifted count to src[7] and flag to src[8].
        words[6] = src[7]
        words[7] = 0
        words[8] = src[8]
        words[9] = 0
    return struct.pack("<24I", *words)


EMPTY_SEC4 = b"\xff\xff\xff\xff\x14\x00\x00\x00\x00\x00\x00\x00\x14\x00\x00\x00\x00\x00\x00\x00"


def _read_cstr(buf, off):
    if off < 0 or off >= len(buf):
        return ""
    end = buf.find(b"\x00", off)
    if end == -1:
        end = len(buf)
    return buf[off:end].decode("latin1", "replace")


def convert_bank(
    iso_f,
    bank,
    stream_base,
    iso_entries,
    is_lang,
    stream_filename,
    resident_blob=None,
    music_file="mus_01_iceland.ss2",
    shared_decoded=None,
    shared_pcm=None,
):
    hdr = struct.unpack_from("<11I", bank, 0)
    n1 = hdr[2]
    sec2_at = hdr[3]
    n2 = hdr[4]
    xb_sec3_off = hdr[5]
    xb_sec4_off = hdr[7]
    xb_sec3_size = hdr[10]
    if xb_sec3_off + xb_sec3_size > xb_sec4_off or xb_sec4_off > len(bank):
        raise ValueError(f"Unexpected SCDA bank section layout: {hdr}")

    sec3 = bytearray(bank[xb_sec3_off : xb_sec3_off + xb_sec3_size])
    while len(sec3) % 4 != 0:
        sec3.append(0)
    stream_str_off = len(sec3)
    sec3.extend(stream_filename.encode("latin1") + b"\x00")
    while len(sec3) % 4 != 0:
        sec3.append(0)

    recs = [bank[sec2_at + i * 88:sec2_at + (i + 1) * 88] for i in range(n2)]
    decoded = shared_decoded if shared_decoded is not None else {}
    pcm = shared_pcm if shared_pcm is not None else bytearray()
    cursor = stream_base + len(pcm) if shared_pcm is not None else stream_base
    energy = 0
    skipped = 0
    energy = 0
    skipped = 0

    def channels_of(src):
        return src[17] if src[17] in (1, 2) else 1

    def wave_source(src):
        w19 = src[19]
        if w19 == 0xFFFFFFFF:
            return "RESIDENT", None
        if w19 < xb_sec3_size:
            fname = _read_cstr(sec3, w19)
            if fname:
                sub = "int/" if is_lang else ""
                iso_key = f"default/dataxb/sounds/{sub}{fname.lower()}"
                ent = iso_entries.get(iso_key)
                if ent:
                    fsize, foff, _ = ent
                    return fname.lower(), (foff, fsize)
        return None, None

    for rec in recs:
        src = struct.unpack("<22I", rec)
        if src[1] != 1:
            continue
        channels = channels_of(src)
        rate = src[15] if 1000 <= src[15] <= 192000 else 0
        adpcm_size = src[2]
        adpcm_off = src[4]
        block = 36 if channels == 1 else 72
        src_tag, src_loc = wave_source(src)
        key = (src_tag, adpcm_off, adpcm_size, channels)
        if key in decoded or not src_tag or not rate or not adpcm_size or adpcm_size % block:
            if key not in decoded:
                skipped += 1
            continue
        if src_tag == "RESIDENT":
            if resident_blob is None or adpcm_off + adpcm_size > len(resident_blob):
                skipped += 1
                continue
            blob = resident_blob[adpcm_off : adpcm_off + adpcm_size]
        else:
            foff, fsize = src_loc
            if adpcm_off + adpcm_size > fsize:
                skipped += 1
                continue
            blob = read_at(iso_f, foff + adpcm_off, adpcm_size)
        pcm_bytes = decode_adpcm(blob, channels)
        if not pcm_bytes:
            skipped += 1
            continue
        decoded[key] = (cursor, len(pcm_bytes), len(pcm_bytes) // (2 * channels), rate, channels)
        if energy == 0:
            acc = 0
            step = max(2, (len(pcm_bytes) // 2000) * 2)
            for p in range(0, len(pcm_bytes) - 1, step):
                s = struct.unpack_from("<h", pcm_bytes, p)[0]
                acc += s * s
            energy = acc
        pcm.extend(pcm_bytes)
        cursor += len(pcm_bytes)

    sec1 = b"".join(expand_sec1(bank[0x2C + i * 72:0x2C + (i + 1) * 72]) for i in range(n1))

    mus_decoded = {}
    if not is_lang and music_file:
        mus_key = f"default/dataxb/sounds/{music_file.lower()}"
        mus_ent = iso_entries.get(mus_key) or iso_entries.get("default/dataxb/sounds/mus_01_iceland.ss2")
        if mus_ent:
            msz, moff, _ = mus_ent
            shared_mus_key = ("__MUSIC__", mus_key)
            for idx_r, rec in enumerate(recs):
                src = struct.unpack("<22I", rec)
                if src[1] == 6:
                    if shared_mus_key in decoded:
                        mus_decoded[idx_r] = decoded[shared_mus_key]
                    else:
                        # Decode first 60s of stereo 48kHz Xbox IMA ADPCM (48000 // 64 * 72 * 60 = 3,240,000 bytes).
                        read_sz = min(3240000, (msz // 72) * 72)
                        blob = read_at(iso_f, moff, read_sz)
                        pcm_bytes = decode_adpcm(blob, 2)
                        if pcm_bytes:
                            entry_m = (cursor, len(pcm_bytes), len(pcm_bytes) // 4, 48000, 2)
                            decoded[shared_mus_key] = entry_m
                            mus_decoded[idx_r] = entry_m
                            pcm.extend(pcm_bytes)
                            cursor += len(pcm_bytes)

    sec2 = bytearray()
    linked = 0
    for idx_r, rec in enumerate(recs):
        src = struct.unpack("<22I", rec)
        if src[1] == 4:
            cnt = src[7]
            old_off = src[3]
            if cnt <= 0 or old_off + cnt * 12 > xb_sec3_size:
                raise ValueError(f"Bad type=4 playlist sec3 range @ rec[{idx_r}]: cnt={cnt}, old_off={old_off:#x}, s3_sz={xb_sec3_size:#x}")
            while len(sec3) % 4 != 0:
                sec3.append(0)
            new_off = len(sec3)
            for j in range(cnt):
                child_idx, weight, _unk = struct.unpack_from("<3I", bank, xb_sec3_off + old_off + j * 12)
                if (child_idx & 0x3FFFFFFF) >= n2:
                    raise ValueError(f"Bad type=4 child_idx {child_idx:#x} >= n2={n2} in rec[{idx_r}]")
                sec3.extend(struct.pack("<4I", child_idx, weight, 0, 0))
            words = src[:10] + (0, 0) + src[10:]
            words = list(words)
            words[3] = new_off
            words[6] = cnt
            words[7] = 0
            words[8] = 0
            words[9] = 1
            sec2.extend(struct.pack("<24I", *words))
            continue
        if src[1] == 6 and idx_r in mus_decoded:
            pcm_off, pcm_size, samples, rate, ch = mus_decoded[idx_r]
            sec2.extend(expand_sec2(rec, pcm_off, pcm_size, samples, rate, ch, stream_str_off, looping=True))
            linked += 1
            continue
        channels = channels_of(src)
        src_tag, _ = wave_source(src) if src[1] == 1 else (None, None)
        hit = decoded.get((src_tag, src[4], src[2], channels)) if src[1] == 1 else None
        if hit:
            pcm_off, pcm_size, samples, rate, ch = hit
            sec2.extend(expand_sec2(rec, pcm_off, pcm_size, samples, rate, ch, stream_str_off))
            linked += 1
        else:
            sec2.extend(expand_sec2(rec, 0, 0, 0, 22050, 1))
    sec3_bytes = bytes(sec3)
    sec4 = EMPTY_SEC4
    sec2_off = 0x24 + len(sec1)
    pc_sec3_off = sec2_off + len(sec2)
    pc_sec4_off = pc_sec3_off + len(sec3_bytes)
    header = struct.pack("<9I", 0, 0x24, n1, sec2_off, n2, pc_sec4_off, 1, pc_sec3_off, len(sec3_bytes))
    out_pcm = b"" if shared_pcm is not None else bytes(pcm)
    return header + sec1 + bytes(sec2) + sec3_bytes + sec4, out_pcm, len(decoded), skipped, energy, linked


def relocate_bank_blob(blob, stream_base):
    """Shift all type-1 wave offsets in a converted PC bank blob by stream_base."""
    if not stream_base:
        return blob
    out = bytearray(blob)
    _, _, _, sec2_off, n2, _, _, _, _ = struct.unpack_from("<9I", out, 0)
    for i in range(n2):
        rec_off = sec2_off + i * 96
        w1, w2 = struct.unpack_from("<II", out, rec_off + 4)
        if w1 == 1 and w2 > 0:
            w4 = struct.unpack_from("<I", out, rec_off + 16)[0]
            struct.pack_into("<I", out, rec_off + 16, w4 + stream_base)
    return bytes(out)


def append_banks_batch(src_path, dst_path, bank_entries):
    """Append multiple (bank_name, bank_blob) entries to a DareAudio MAPS.SM0/LM0 in one pass."""
    src_size = os.path.getsize(src_path)
    with open(src_path, "rb") as f:
        prefix = f.read(0x0C)
    ver_a, ver_b, dir_off, count = struct.unpack_from("<HHII", prefix, 0)
    if dir_off != 0x0C:
        raise ValueError(f"{src_path} directory offset {dir_off:#x}, expected 0x0c")
    with open(src_path, "rb") as f:
        f.seek(dir_off)
        directory = f.read(count * 0x34)
    head = prefix + directory
    old_dir_end = dir_off + count * 0x34
    ents = parse_dir(head, dir_off, count)
    existing_names = {e["name"].lower() for e in ents}
    to_add = [(nm, bl) for nm, bl in bank_entries if nm.lower() not in existing_names]
    shift = 0x34 * len(to_add)
    out_dir = bytearray()
    for ent in ents:
        out_dir += struct.pack("<IIII", ent["flag"], ent["zero"], ent["off"] + shift, ent["size"])
        out_dir += ent["name"].encode("latin1")[:32].ljust(32, b"\x00")
        out_dir += struct.pack("<I", ent["tail"])
    cursor = src_size + shift
    for nm, bl in to_add:
        out_dir += struct.pack("<IIII", 0, 0, cursor, len(bl))
        out_dir += nm.encode("latin1")[:32].ljust(32, b"\x00")
        out_dir += struct.pack("<I", 0x04D71B00)
        cursor += len(bl)
    header = struct.pack("<HHII", ver_a, ver_b, 0x0C, count + len(to_add))
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with open(dst_path, "wb") as out, open(src_path, "rb") as src:
        out.write(header)
        out.write(out_dir)
        src.seek(old_dir_end)
        shutil.copyfileobj(src, out, length=1024 * 1024)
        for _, bl in to_add:
            out.write(bl)


def export_audio_pack(maps_list, out_dir):
    """Export deduplicated SFX/dialogue PCM streams and relocatable .bank files for all SCDA maps."""
    import json

    os.makedirs(out_dir, exist_ok=True)
    entries = ie.parse_iso_list()
    _, sm2_off, _ = entries["default/dataxb/sounds/maps.sm2"]
    _, lm2_off, _ = entries["default/dataxb/sounds/int/maps.lm2"]

    sfx_decoded = {}
    sfx_pcm = bytearray()
    dlg_decoded = {}
    dlg_pcm = bytearray()
    manifest_maps = []

    with open(ISO, "rb") as iso:
        for item in maps_list:
            bank_name = item["map_name"]
            out_name = item["out_name"]
            music_file = item.get("music", "mus_01_iceland.ss2")

            _ver, _ent, sm_bank, _ents = find_bank(iso, sm2_off, bank_name)
            ent_idx = [e["name"].lower() for e in _ents].index(bank_name.lower())
            res_sz = (_ents[ent_idx + 1]["off"] - (_ent["off"] + _ent["size"])) if ent_idx + 1 < len(_ents) else 0
            res_blob = read_at(iso, sm2_off + _ent["off"] + _ent["size"], res_sz) if res_sz > 0 else None

            sm_blob, _, _, _, _, _ = convert_bank(
                iso,
                sm_bank,
                0,
                entries,
                False,
                "SCDA_STREAM.SS0",
                res_blob,
                music_file=music_file,
                shared_decoded=sfx_decoded,
                shared_pcm=sfx_pcm,
            )
            sfx_fn = f"sfx_{out_name}.bank"
            with open(os.path.join(out_dir, sfx_fn), "wb") as bf:
                bf.write(sm_blob)

            _lver, _lent, lm_bank, _lents = find_bank(iso, lm2_off, bank_name)
            lm_blob, _, _, _, _, _ = convert_bank(
                iso,
                lm_bank,
                0,
                entries,
                True,
                "SCDA_STREAM.LS0",
                None,
                music_file=None,
                shared_decoded=dlg_decoded,
                shared_pcm=dlg_pcm,
            )
            dlg_fn = f"dlg_{out_name}.bank"
            with open(os.path.join(out_dir, dlg_fn), "wb") as bf:
                bf.write(lm_blob)

            manifest_maps.append({"out_name": out_name, "sfx_bank": sfx_fn, "dlg_bank": dlg_fn})
            print(f"  [audio-pack] {bank_name} -> {out_name} (sfx_pcm={len(sfx_pcm)/1048576:.1f} MB, dlg_pcm={len(dlg_pcm)/1048576:.1f} MB)")

    with open(os.path.join(out_dir, "sfx.pcm"), "wb") as sf:
        sf.write(sfx_pcm)
    with open(os.path.join(out_dir, "dlg.pcm"), "wb") as df:
        df.write(dlg_pcm)
    manifest = {"maps": manifest_maps, "sfx_pcm": "sfx.pcm", "dlg_pcm": "dlg.pcm"}
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)
    return manifest


def append_bank(src_path, dst_path, bank_name, bank_blob):
    src_size = os.path.getsize(src_path)
    with open(src_path, "rb") as f:
        prefix = f.read(0x0C)
    ver_a, ver_b, dir_off, count = struct.unpack_from("<HHII", prefix, 0)
    if dir_off != 0x0C:
        raise ValueError(f"{src_path} directory offset {dir_off:#x}, expected 0x0c")
    with open(src_path, "rb") as f:
        f.seek(dir_off)
        directory = f.read(count * 0x34)
    head = prefix + directory
    old_dir_end = dir_off + count * 0x34
    ents = parse_dir(head, dir_off, count)
    if any(e["name"].lower() == bank_name.lower() for e in ents):
        raise ValueError(f"{bank_name} already in {src_path}")
    shift = 0x34
    out_dir = bytearray()
    for ent in ents:
        out_dir += struct.pack("<IIII", ent["flag"], ent["zero"], ent["off"] + shift, ent["size"])
        out_dir += ent["name"].encode("latin1")[:32].ljust(32, b"\x00")
        out_dir += struct.pack("<I", ent["tail"])
    new_off = src_size + shift
    out_dir += struct.pack("<IIII", 0, 0, new_off, len(bank_blob))
    out_dir += bank_name.encode("latin1")[:32].ljust(32, b"\x00")
    out_dir += struct.pack("<I", 0x04D71B00)
    header = struct.pack("<HHII", ver_a, ver_b, 0x0C, count + 1)
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with open(dst_path, "wb") as out, open(src_path, "rb") as src:
        out.write(header)
        out.write(out_dir)
        src.seek(old_dir_end)
        shutil.copyfileobj(src, out, length=1024 * 1024)
        out.write(bank_blob)
    return new_off


def append_stream(src_path, dst_path, pcm):
    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    base = os.path.getsize(src_path)
    if os.path.exists(dst_path):
        os.remove(dst_path)
    shutil.copyfile(src_path, dst_path)
    with open(dst_path, "ab") as f:
        f.write(pcm)
    return base


def build(ct_sounds, bank_name=BANK_NAME, out_name=OUT_NAME):
    entries = ie.parse_iso_list()
    _, sm2_off, _ = entries["default/dataxb/sounds/maps.sm2"]
    _, lm2_off, _ = entries["default/dataxb/sounds/int/maps.lm2"]

    sm0 = os.path.join(ct_sounds, "MAPS.SM0")
    ss0 = os.path.join(ct_sounds, "STREAM.SS0")
    out_sm = os.path.join(ct_sounds, "SCDA_MAPS.SM0")
    out_ss = os.path.join(ct_sounds, "SCDA_STREAM.SS0")

    with open(ISO, "rb") as iso:
        _ver, _ent, sm_bank, _ents = find_bank(iso, sm2_off, bank_name)
        ent_idx = [e["name"].lower() for e in _ents].index(bank_name.lower())
        res_sz = (_ents[ent_idx + 1]["off"] - (_ent["off"] + _ent["size"])) if ent_idx + 1 < len(_ents) else 0
        res_blob = read_at(iso, sm2_off + _ent["off"] + _ent["size"], res_sz) if res_sz > 0 else None

        sfx_base = os.path.getsize(ss0)
        sm_blob, sm_pcm, sm_conv, sm_skip, sm_energy, sm_link = convert_bank(
            iso, sm_bank, sfx_base, entries, False, "SCDA_STREAM.SS0", res_blob
        )
        print(f"{bank_name} sfx bank events={struct.unpack_from('<I', sm_blob, 8)[0]} waves={struct.unpack_from('<I', sm_blob, 16)[0]}")
        print(f"  pcm bytes={len(sm_pcm):#x} unique={sm_conv} linked={sm_link} skipped={sm_skip} energy={sm_energy}")
        if sm_conv == 0 or sm_energy == 0:
            raise RuntimeError("ADPCM SFX decode produced no audible samples")
        append_stream(ss0, out_ss, sm_pcm)
        off_sm = append_bank(sm0, out_sm, out_name, sm_blob)
        print(f"STREAM.SS0 -> {out_ss} append base={sfx_base:#x}")
        print(f"MAPS.SM0 -> {out_sm} bank off={off_sm:#x} size={len(sm_blob):#x}")

        _lver, _lent, lm_bank, _lents = find_bank(iso, lm2_off, bank_name)
        for lang in ("int", "org"):
            lm0 = os.path.join(ct_sounds, lang, "MAPS.LM0")
            ls0 = os.path.join(ct_sounds, lang, "STREAM.LS0")
            if not (os.path.isfile(lm0) and os.path.isfile(ls0)):
                continue
            out_lm = os.path.join(ct_sounds, lang, "SCDA_MAPS.LM0")
            out_ls = os.path.join(ct_sounds, lang, "SCDA_STREAM.LS0")
            dlg_base = os.path.getsize(ls0)
            lm_blob, lm_pcm, lm_conv, lm_skip, lm_energy, lm_link = convert_bank(
                iso, lm_bank, dlg_base, entries, True, "SCDA_STREAM.LS0", None
            )
            print(f"{bank_name} dlg[{lang}] events={struct.unpack_from('<I', lm_blob, 8)[0]} waves={struct.unpack_from('<I', lm_blob, 16)[0]}")
            print(f"  pcm bytes={len(lm_pcm):#x} unique={lm_conv} linked={lm_link} skipped={lm_skip} energy={lm_energy}")
            if lm_conv == 0 or lm_energy == 0:
                raise RuntimeError(f"ADPCM dialogue decode for {lang} produced no audible samples")
            append_stream(ls0, out_ls, lm_pcm)
            off_lm = append_bank(lm0, out_lm, out_name, lm_blob)
            print(f"  {lang}/STREAM.LS0 -> {out_ls} append base={dlg_base:#x}")
            print(f"  {lang}/MAPS.LM0 -> {out_lm} bank off={off_lm:#x} size={len(lm_blob):#x}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", default=BANK_NAME, help="SCDA Xbox bank/map name (e.g. x01_coop_ice_a)")
    parser.add_argument("--out-name", default=OUT_NAME, help="Target SCCT bank/map name (e.g. scda_ice_a)")
    parser.add_argument("--sounds", default=r"D:\Jeux\Tom Clancys Splinter Cell Chaos Theory\Data\Sounds")
    parser.add_argument("--export-pack", default="", help="Export deduplicated audio pack to directory")
    args = parser.parse_args()
    if args.export_pack:
        import json
        meta_path = os.path.join(HERE, "map_metadata.json")
        with open(meta_path, "r", encoding="utf-8") as f:
            meta = json.load(f).get("maps", {})
        maps_list = [{"map_name": k, **v} for k, v in meta.items()]
        export_audio_pack(maps_list, args.export_pack)
        return
    build(args.sounds, bank_name=args.map, out_name=args.out_name)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        sys.exit(1)
