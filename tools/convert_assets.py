#!/usr/bin/env python3
"""Convert PICO-8 Celeste assets (gfx.bmp, font.bmp) to PS1 4bpp C header arrays."""

import struct
import os

# PICO-8 16-color palette (RGB888)
PICO8_PALETTE = [
    (0, 0, 0),        # 0  black (transparent)
    (29, 43, 83),      # 1  dark blue
    (126, 37, 83),     # 2  dark purple
    (0, 135, 81),      # 3  dark green
    (171, 82, 54),     # 4  brown
    (95, 87, 79),      # 5  dark grey
    (194, 195, 199),   # 6  light grey
    (255, 241, 232),   # 7  white
    (255, 0, 77),      # 8  red
    (255, 163, 0),     # 9  orange
    (255, 236, 39),    # 10 yellow
    (0, 228, 54),      # 11 green
    (41, 173, 255),    # 12 blue
    (131, 118, 156),   # 13 lavender
    (255, 119, 168),   # 14 pink
    (255, 204, 170),   # 15 peach
]


def rgb_to_ps1(r, g, b):
    """Convert RGB888 to PS1 15-bit color. Returns 0 for pure black (transparent)."""
    r5, g5, b5 = r >> 3, g >> 3, b >> 3
    return (b5 << 10) | (g5 << 5) | r5


def read_bmp(filename):
    """Read BMP, return (width, height, bpp, palette_rgb, rows_of_pixel_indices)."""
    with open(filename, "rb") as f:
        sig = f.read(2)
        assert sig == b"BM", f"Not a BMP file: {filename}"
        struct.unpack("<I", f.read(4))  # file size
        f.read(4)  # reserved
        data_offset = struct.unpack("<I", f.read(4))[0]

        dib_size = struct.unpack("<I", f.read(4))[0]
        width = struct.unpack("<i", f.read(4))[0]
        height = struct.unpack("<i", f.read(4))[0]
        struct.unpack("<H", f.read(2))  # planes
        bpp = struct.unpack("<H", f.read(2))[0]

        bottom_up = height > 0
        abs_height = abs(height)

        # Read palette
        f.seek(14 + dib_size)
        num_colors = 1 << bpp if bpp <= 8 else 0
        palette = []
        for _ in range(num_colors):
            b, g, r, _ = struct.unpack("BBBB", f.read(4))
            palette.append((r, g, b))

        # Read pixel data
        f.seek(data_offset)
        rows = []

        if bpp == 4:
            row_bytes = (width + 1) // 2
            row_pad = (4 - row_bytes % 4) % 4
            for _ in range(abs_height):
                raw = f.read(row_bytes)
                row = []
                for byte in raw:
                    row.append((byte >> 4) & 0x0F)
                    row.append(byte & 0x0F)
                rows.append(row[:width])
                f.read(row_pad)

        elif bpp == 1:
            row_bytes = (width + 7) // 8
            row_pad = (4 - row_bytes % 4) % 4
            for _ in range(abs_height):
                raw = f.read(row_bytes)
                row = []
                for byte in raw:
                    for bit in range(7, -1, -1):
                        row.append((byte >> bit) & 1)
                rows.append(row[:width])
                f.read(row_pad)
        else:
            raise ValueError(f"Unsupported BPP: {bpp}")

        if bottom_up:
            rows.reverse()

        return width, abs_height, bpp, palette, rows


def double_pixels(rows, width, height):
    """Scale each pixel 2x2."""
    out = []
    for y in range(height):
        row = []
        for x in range(width):
            row.append(rows[y][x])
            row.append(rows[y][x])
        out.append(row)
        out.append(list(row))
    return out, width * 2, height * 2


def to_ps1_4bpp(rows, width, height):
    """Pack pixel rows into PS1 4bpp uint16_t array.
    PS1 4bpp: bits 0-3 = leftmost pixel, bits 12-15 = rightmost pixel."""
    data = []
    for y in range(height):
        for x in range(0, width, 4):
            p0 = rows[y][x] if x < len(rows[y]) else 0
            p1 = rows[y][x + 1] if x + 1 < len(rows[y]) else 0
            p2 = rows[y][x + 2] if x + 2 < len(rows[y]) else 0
            p3 = rows[y][x + 3] if x + 3 < len(rows[y]) else 0
            word = (p0 & 0xF) | ((p1 & 0xF) << 4) | ((p2 & 0xF) << 8) | ((p3 & 0xF) << 12)
            data.append(word)
    return data


def write_array(f, name, data, comment):
    """Write a uint16_t C array to file handle."""
    f.write(f"// {comment}\n")
    f.write(f"static const uint16_t {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        f.write("    " + ", ".join(f"0x{v:04X}" for v in chunk) + ",\n")
    f.write("};\n\n")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.path.join(script_dir, "..", "src", "celeste", "data")
    out_dir = os.path.join(script_dir, "..", "src", "celeste")

    # --- GFX spritesheet ---
    print("Converting gfx.bmp...")
    gw, gh, _, _, grows = read_bmp(os.path.join(data_dir, "gfx.bmp"))
    print(f"  Source: {gw}x{gh}")

    # Pad to 128x128 (bottom half empty)
    while len(grows) < 128:
        grows.append([0] * gw)

    grows_2x, gw2, gh2 = double_pixels(grows, gw, 128)
    gfx_data = to_ps1_4bpp(grows_2x, gw2, gh2)
    print(f"  Output: {gw2}x{gh2} -> {len(gfx_data)} words ({len(gfx_data)*2} bytes)")

    with open(os.path.join(out_dir, "gfx_data.h"), "w") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        write_array(f, "gfx_data", gfx_data,
                    f"GFX spritesheet: {gw2}x{gh2} 4bpp (doubled from {gw}x128)")

        # Sprite CLUT (PICO-8 palette, color 0 = transparent = 0x0000)
        clut = []
        for i, (r, g, b) in enumerate(PICO8_PALETTE):
            clut.append(0x0000 if i == 0 else rgb_to_ps1(r, g, b))
        write_array(f, "pico8_clut", clut,
                    "PICO-8 palette as PS1 15-bit CLUT (color 0 = transparent)")

        # RGB888 palette for non-textured primitives
        f.write("// PICO-8 palette RGB888 for rectangles/lines\n")
        f.write("static const uint8_t pico8_rgb[16][3] = {\n")
        for r, g, b in PICO8_PALETTE:
            f.write(f"    {{{r}, {g}, {b}}},\n")
        f.write("};\n\n")

        # Text CLUTs: 16 CLUTs, one per text color
        # Font pixels use index 7. Each CLUT maps index 7 to the desired text color.
        f.write("// Text CLUTs: font index 7 -> each PICO-8 color\n")
        f.write("static const uint16_t text_cluts[16][16] = {\n")
        for tc in range(16):
            row = [0x0000] * 16
            if tc != 0:
                r, g, b = PICO8_PALETTE[tc]
                row[7] = rgb_to_ps1(r, g, b)
            line = ", ".join(f"0x{v:04X}" for v in row)
            f.write(f"    {{{line}}},\n")
        f.write("};\n")

    # --- Font ---
    print("Converting font.bmp...")
    fw, fh, _, _, frows = read_bmp(os.path.join(data_dir, "font.bmp"))
    print(f"  Source: {fw}x{fh}")

    # Expand 1bpp to 4bpp: 0 -> index 0 (transparent), 1 -> index 7 (white)
    for y in range(fh):
        for x in range(fw):
            if frows[y][x] != 0:
                frows[y][x] = 7

    frows_2x, fw2, fh2 = double_pixels(frows, fw, fh)
    font_data = to_ps1_4bpp(frows_2x, fw2, fh2)
    print(f"  Output: {fw2}x{fh2} -> {len(font_data)} words ({len(font_data)*2} bytes)")

    with open(os.path.join(out_dir, "font_data.h"), "w") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        write_array(f, "font_data", font_data,
                    f"Font: {fw2}x{fh2} 4bpp (doubled from {fw}x{fh})")

    print("Done!")


if __name__ == "__main__":
    main()
