#!/usr/bin/env python3
"""Build src/web/sym0.png and sym1.png: the aprs.fi symbol sheets at 24 px per cell (16 columns x 6 rows = 384x144) for the
phone setup page's symbol picker. Source: the 48 px sheets of the aprs.fi symbol set by Heikki Hannikainen, OH7LZB
(https://github.com/hessu/aprs-symbols; not covered by this project's own licence, see COPYRIGHT.md there).
Usage: tools/make_symbol_sheets.py <dir holding symbols-0.png and symbols-1.png>
"""
import sys
from PIL import Image
src = sys.argv[1]
for n in (0, 1):
    im = Image.open(f"{src}/symbols-{n}.png").convert("RGBa")
    out = im.resize((im.width // 2, im.height // 2), Image.LANCZOS).convert("RGBA")
    out.save(f"src/web/sym{n}.png", optimize=True)
    print(f"src/web/sym{n}.png {out.size}")
