#!/usr/bin/env python3
"""Build src/symbols24.bin: 24x24 APRS symbols for the pager, from the aprs.fi symbol sheets.

Source sheets: the 48 px renderings (aprs-symbols-24-N@2x.png) of the aprs.fi symbol set by Heikki Hannikainen, OH7LZB
(https://github.com/hessu/aprs-symbols). The symbols are NOT covered by this project's own licence; see COPYRIGHT.md there.

Sheet 0 = primary table, sheet 1 = alternate table, sheet 2 = overlay characters (0-9, A-Z, drawn over an alternate-table symbol);
16 columns x 6 rows, character c is cell ord(c) - 33 (94 cells used per sheet).
Output record per symbol: 576 x RGB565 (little endian) then 576 x alpha (0..255) = 1728 bytes.  Layout: sheet 0 cells 0..93, then sheet 1, then sheet 2.
Usage: tools/make_symbols.py <dir holding symbols-0.png, symbols-1.png and symbols-2.png> [out.bin]
Get the sheets from https://github.com/hessu/aprs-symbols (the png/aprs-symbols-24-N@2x.png files, N = 0, 1, 2) and copy them to symbols-N.png.
"""
import struct, sys
from PIL import Image

SRC = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "src/symbols24.bin"
CELL, SIZE, N = 48, 24, 94

out = bytearray()
for table in (0, 1, 2):
    sheet = Image.open(f"{SRC}/symbols-{table}.png").convert("RGBA")
    for i in range(N):
        cx, cy = (i % 16) * CELL, (i // 16) * CELL
        cell = sheet.crop((cx, cy, cx + CELL, cy + CELL)).convert("RGBa")        # premultiplied: no dark halos when shrinking
        small = cell.resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")
        cols, alphas = bytearray(), bytearray()
        for (r, g, b, a) in small.get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata():
            cols += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
            alphas.append(a)
        out += cols + alphas
open(OUT, "wb").write(out)
print(f"wrote {OUT}: {len(out)} bytes ({3 * N} cells)")
