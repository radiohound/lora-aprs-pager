#!/usr/bin/env python3
"""Build src/emoji18.bin: 18x18 icons for the pager's emoticons (ASCII on the air, a picture on the screen).

Icons: Twemoji graphics, (c) Twitter, Inc and other contributors, licensed CC-BY 4.0 (https://github.com/jdecked/twemoji,
LICENSE-GRAPHICS). They are NOT covered by this project's own licence; credit them wherever the firmware is distributed.

The icon order is the order in which each distinct emoticon NAME first appears in src/emoticons.h (that file is read here, so the two
cannot drift apart). Record per icon: 324 x RGB565 (little endian) then 324 x alpha = 972 bytes.
Usage: tools/make_emoji.py            (downloads the 72 px PNGs from GitHub)
"""
import io, re, struct, sys, urllib.request
from PIL import Image

SIZE = 18
CODEPOINTS = {  # emoticon name -> Twemoji file name (same pictures the Pi page uses as emoji)
    "smile": "1f642", "sad": "1f641", "wink": "1f609", "grin": "1f600", "laughing": "1f606", "tongue": "1f61b",
    "surprised": "1f62e", "neutral": "1f610", "unsure": "1f615", "crying": "1f622", "crying hard": "1f62d", "kiss": "1f618",
    "heart": "2764", "broken heart": "1f494", "thumbs up": "1f44d", "thumbs down": "1f44e", "wave": "1f44b", "cheer": "1f64c",
    "cool": "1f60e", "angel": "1f607", "angry": "1f620", "unimpressed": "1f611", "happy": "1f60a", "cute": "1f63a",
}
src = open("src/emoticons.h").read()
names = []
for n in re.findall(r'\{"[^"]*(?:\\\\[^"]*)*",\s*"([^"]+)"\}', src):
    if n not in names:
        names.append(n)
missing = [n for n in names if n not in CODEPOINTS]
if missing:
    sys.exit(f"no picture chosen for: {missing}")

out = bytearray()
for n in names:
    url = f"https://raw.githubusercontent.com/jdecked/twemoji/main/assets/72x72/{CODEPOINTS[n]}.png"
    im = Image.open(io.BytesIO(urllib.request.urlopen(url).read())).convert("RGBA")
    small = im.convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")
    cols, alphas = bytearray(), bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = small.getpixel((x, y))
            cols += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); alphas.append(a)
    out += cols + alphas
open("src/emoji18.bin", "wb").write(out)
print(f"src/emoji18.bin: {len(out)} bytes, {len(names)} icons: {', '.join(names)}")
