"""Convert SCDA Xbox loading screens (.tga) and map icons (.dds) to Chaos Theory PC format."""

import argparse
import io
import os
import struct
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    np = None
    Image = None

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import iso_extract as ie


def _rgb888_to_565(rgb):
    r = (rgb[..., 0] >> 3).astype(np.uint16)
    g = (rgb[..., 1] >> 2).astype(np.uint16)
    b = (rgb[..., 2] >> 3).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def _rgb565_to_888(c):
    r = ((c >> 11) & 0x1F).astype(np.int32)
    g = ((c >> 5) & 0x3F).astype(np.int32)
    b = (c & 0x1F).astype(np.int32)
    r8 = (r << 3) | (r >> 2)
    g8 = (g << 2) | (g >> 4)
    b8 = (b << 3) | (b >> 2)
    return np.stack([r8, g8, b8], axis=-1)


def encode_dxt5_dds(rgba_img):
    """Encode an RGBA PIL Image (dims multiple of 4) into a CT PC DXT5 DDS file (no mipmaps)."""
    img = rgba_img.convert("RGBA")
    w, h = img.size
    if (w % 4) or (h % 4):
        raise ValueError(f"DXT5 requires dimensions multiple of 4, got {w}x{h}")

    arr = np.asarray(img, dtype=np.uint8)
    bh, bw = h // 4, w // 4
    blocks = arr.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4)
    n = blocks.shape[0]

    # --- BC3 Alpha block (8 bytes/block) ---
    alpha = blocks[:, :, 3]
    a0 = alpha.max(axis=1)
    a1 = alpha.min(axis=1)

    pal_a = np.empty((n, 8), dtype=np.int32)
    a0_i = a0.astype(np.int32)
    a1_i = a1.astype(np.int32)
    pal_a[:, 0] = a0_i
    pal_a[:, 1] = a1_i
    for k in range(6):
        pal_a[:, 2 + k] = ((6 - k) * a0_i + (1 + k) * a1_i + 3) // 7

    da = (alpha[:, :, None].astype(np.int32) - pal_a[:, None, :]) ** 2
    a_idx = da.argmin(axis=2).astype(np.uint64)
    a_idx[a0 == a1] = 0
    a_shifts = np.arange(16, dtype=np.uint64) * 3
    a_bits = np.bitwise_or.reduce(a_idx << a_shifts[None, :], axis=1)

    # --- BC1 Color block (8 bytes/block, 4-color mode c0 >= c1) ---
    rgb = blocks[:, :, :3].astype(np.int32)
    c_min = rgb.min(axis=1)
    c_max = rgb.max(axis=1)
    c0 = _rgb888_to_565(c_max)
    c1 = _rgb888_to_565(c_min)

    swap = c0 < c1
    c0_fixed = np.where(swap, c1, c0)
    c1_fixed = np.where(swap, c0, c1)

    p0 = _rgb565_to_888(c0_fixed)
    p1 = _rgb565_to_888(c1_fixed)
    p2 = (2 * p0 + p1 + 1) // 3
    p3 = (p0 + 2 * p1 + 1) // 3
    pal_c = np.stack([p0, p1, p2, p3], axis=1)  # (n, 4, 3)

    dc = ((rgb[:, :, None, :] - pal_c[:, None, :, :]) ** 2).sum(axis=3)
    c_idx = dc.argmin(axis=2).astype(np.uint32)
    c_idx[c0_fixed == c1_fixed] = 0
    c_shifts = np.arange(16, dtype=np.uint32) * 2
    c_bits = np.bitwise_or.reduce(c_idx << c_shifts[None, :], axis=1)

    out_blocks = np.zeros((n, 16), dtype=np.uint8)
    out_blocks[:, 0] = a0
    out_blocks[:, 1] = a1
    for b_i in range(6):
        out_blocks[:, 2 + b_i] = ((a_bits >> (8 * b_i)) & 0xFF).astype(np.uint8)
    out_blocks[:, 8] = (c0_fixed & 0xFF).astype(np.uint8)
    out_blocks[:, 9] = (c0_fixed >> 8).astype(np.uint8)
    out_blocks[:, 10] = (c1_fixed & 0xFF).astype(np.uint8)
    out_blocks[:, 11] = (c1_fixed >> 8).astype(np.uint8)
    for b_i in range(4):
        out_blocks[:, 12 + b_i] = ((c_bits >> (8 * b_i)) & 0xFF).astype(np.uint8)

    hdr = (
        struct.pack("<4s7I11I", b"DDS ", 124, 0x00081007, h, w, w * h, 0, 0, *([0] * 11))
        + struct.pack("<II4s5I", 32, 0x4, b"DXT5", 512, 0, 0, 0, 0)
        + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    )
    assert len(hdr) == 128
    return hdr + out_blocks.tobytes()


def _extract_umd_map_icon(icon_name):
    """Extract a .dds map icon from SCDA DynamicXbox.umd if present."""
    if not os.path.isfile(ie.UMD_PATH):
        return None
    target_low = icon_name.lower()
    with open(ie.UMD_PATH, "rb") as f:
        f.seek(-20, 2)
        magic, dir_off, _, _, _ = struct.unpack("<IIIII", f.read(20))
        if magic != 0x9FE3C5A3:
            return None
        f.seek(dir_off)
        cnt = ie.read_umd_compact_index(f)
        for _ in range(cnt):
            slen = ie.read_umd_compact_index(f)
            if slen > 0:
                nm = f.read(slen).rstrip(b"\x00").decode("latin1", errors="replace")
            elif slen < 0:
                nm = f.read(-slen * 2).decode("utf-16le", errors="replace").rstrip("\x00")
            else:
                nm = ""
            off, sz, _ = struct.unpack("<III", f.read(12))
            if os.path.basename(nm).lower() == target_low:
                f.seek(off)
                return f.read(sz)
    return None


def _find_custom_loading(out_name):
    custom_dir = os.path.join(HERE, "custom_loadings")
    for ext in (".png", ".jpg", ".jpeg", ".tga", ".bmp"):
        cand = os.path.join(custom_dir, out_name + ext)
        if os.path.isfile(cand):
            return cand
    return None


def convert_loading_assets(map_name, out_name, out_root):
    """Convert custom CT loading image or SCDA loading TGA + extract map icon DDS."""
    written = []
    src = None
    custom_path = _find_custom_loading(out_name)
    if custom_path is not None:
        src = Image.open(custom_path).convert("RGBA")
    else:
        iso_entries = ie.parse_iso_list()
        tga_key = f"default/dataxb/screens/int/{map_name.lower()}_loading0.tga"
        if tga_key in iso_entries:
            sz, off, _ = iso_entries[tga_key]
            with open(ie.ISO_PATH, "rb") as f:
                f.seek(off)
                tga_bytes = f.read(sz)
            src = Image.open(io.BytesIO(tga_bytes)).convert("RGBA")
        else:
            print(f"  [WARN] Loading TGA not found in ISO: {tga_key}")

    if src is not None:
        if src.size != (640, 480):
            src = src.resize((640, 480), Image.Resampling.LANCZOS)

        canvas = Image.new("RGBA", (640, 512), (0, 0, 0, 0))
        canvas.paste(src, (0, 0))

        gen_dir = os.path.join(out_root, "Data", "Magma", "DataPC", "Textures", "GenLoadings")
        os.makedirs(gen_dir, exist_ok=True)

        p1 = os.path.join(gen_dir, f"{out_name}_loading_1.dds")
        with open(p1, "wb") as f:
            f.write(encode_dxt5_dds(canvas.crop((0, 0, 512, 512))))
        written.append(p1)

        p2 = os.path.join(gen_dir, f"{out_name}_loading_2.dds")
        with open(p2, "wb") as f:
            f.write(encode_dxt5_dds(canvas.crop((512, 0, 640, 512))))
        written.append(p2)

    short = out_name[5:] if out_name.lower().startswith("scda_") else out_name
    icon_fn = f"load_map_coop_{short}.dds"
    icon_bytes = _extract_umd_map_icon(icon_fn)
    if icon_bytes is not None:
        maps_tex_dir = os.path.join(out_root, "Data", "Magma", "DataPC", "Textures", "Maps")
        os.makedirs(maps_tex_dir, exist_ok=True)
        pi = os.path.join(maps_tex_dir, icon_fn)
        with open(pi, "wb") as f:
            f.write(icon_bytes)
        written.append(pi)
    else:
        print(f"  [WARN] Map icon not found in UMD: {icon_fn}")

    return written


def main():
    ap = argparse.ArgumentParser(description="Convert SCDA loading screen & icon for SCCT PC")
    ap.add_argument("map_name", help="Source SCDA map (e.g. x01_coop_ice_a)")
    ap.add_argument("--out-name", required=True, help="Target SCCT map stem (e.g. scda_ice_a)")
    ap.add_argument("--out-dir", required=True, help="Output root directory containing Data/")
    args = ap.parse_args()

    files = convert_loading_assets(args.map_name, args.out_name, args.out_dir)
    for fp in files:
        print(f"  [OK] {fp} ({os.path.getsize(fp)} bytes)")


if __name__ == "__main__":
    main()
