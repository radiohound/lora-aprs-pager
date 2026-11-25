#!/usr/bin/env python3
"""Draw the map screen's status icons and write them to src/ui_icons.h. Needs Pillow. Run from the repository root.

Each icon is a list of text rows, one character per pixel: '.' see-through, K black (the thin outline), W white, G green, A grey, Y yellow, R red, P purple.
"""
from PIL import Image, ImageDraw, ImageFilter

BK, WH, GR, GY, YE = (0, 0, 0), (255, 255, 255), (0, 220, 0), (130, 130, 130), (255, 230, 0)
RD = (255, 40, 40)
PU = (200, 60, 255)
CHARS = {WH: 'W', GR: 'G', GY: 'A', YE: 'Y', RD: 'R', PU: 'P'}

def tile(w, h=22):
    im = Image.new('RGB', (w, h), BK); return im, ImageDraw.Draw(im)

def pan_x():
    im, d = tile(36)
    d.rectangle([10, 8, 25, 13], fill=WH)
    d.polygon([(2, 10), (2, 11), (11, 3), (11, 18)], fill=WH)
    d.polygon([(33, 10), (33, 11), (24, 3), (24, 18)], fill=WH)
    return im

def pan_y():
    im, d = tile(22)
    d.rectangle([8, 7, 13, 14], fill=WH)
    d.polygon([(10, 1), (11, 1), (18, 8), (3, 8)], fill=WH)
    d.polygon([(10, 20), (11, 20), (18, 13), (3, 13)], fill=WH)
    return im

def zoom():
    im, d = tile(24)
    d.ellipse([1, 1, 16, 16], outline=WH, width=2)
    d.line([(14, 14), (20, 20)], fill=WH, width=4)
    return im

def stations():                          # a person (a station) with up and down arrows: the knob moves through the stations
    im, d = tile(32)
    d.ellipse([5, 1, 12, 8], fill=YE); d.pieslice([1, 10, 16, 30], 180, 360, fill=YE)
    d.polygon([(24, 1), (25, 1), (30, 7), (19, 7)], fill=WH)
    d.polygon([(24, 20), (25, 20), (30, 14), (19, 14)], fill=WH)
    return im

def gps(c):
    im, d = tile(22)
    d.ellipse([4, 1, 17, 14], fill=c); d.polygon([(5, 11), (16, 11), (11, 20), (10, 20)], fill=c)
    d.ellipse([8, 5, 13, 10], fill=BK)
    return im

def tx(c):
    im, d = tile(22)
    d.line([(11, 9), (11, 20)], fill=c, width=2); d.ellipse([9, 6, 13, 10], fill=c)
    for r in (5, 9):
        d.arc([11 - r, 8 - r, 11 + r, 8 + r], 140, 220, fill=c, width=2)
        d.arc([11 - r, 8 - r, 11 + r, 8 + r], -40, 40, fill=c, width=2)
    return im

def lock():                              # padlock: chase mode is keeping the screen on
    im, d = tile(18, 20)
    d.arc([4, 1, 13, 14], 180, 360, fill=YE, width=3); d.rectangle([4, 7, 6, 10], fill=YE); d.rectangle([11, 7, 13, 10], fill=YE)
    d.rectangle([2, 9, 15, 18], fill=YE); d.rectangle([8, 12, 9, 15], fill=BK)
    return im

def envelope():
    im, d = tile(22, 16)
    d.rectangle([1, 1, 20, 14], fill=WH)
    d.line([(1, 1), (10, 8), (11, 8), (20, 1)], fill=BK, width=1)
    return im

def rows(im):
    """Cut to the exact shape and add the one pixel black outline."""
    pad = Image.new('RGB', (im.width + 4, im.height + 4), BK); pad.paste(im, (2, 2))
    shape = pad.convert('L').point(lambda v: 255 if v > 40 else 0)
    ring = shape.filter(ImageFilter.MaxFilter(3))
    box = ring.getbbox(); out = []
    for y in range(box[1], box[3]):
        line = ''
        for x in range(box[0], box[2]):
            if shape.getpixel((x, y)):
                p = pad.getpixel((x, y)); line += CHARS[min(CHARS, key=lambda c: sum((a - b) ** 2 for a, b in zip(c, p)))]
            else:
                line += 'K' if ring.getpixel((x, y)) else '.'
        out.append(line)
    return out

ICONS = [('PAN_X', pan_x()), ('PAN_Y', pan_y()), ('ZOOM', zoom()), ('STATIONS', stations()),
         ('GPS_FIX', gps(GR)), ('GPS_NONE', gps(GY)), ('TX_AUTO', tx(GR)), ('TX_OFF', tx(GY)), ('TX_NOW', tx(RD)), ('RX_NOW', tx(PU)), ('MAIL', envelope()), ('LOCK', lock())]
text = '// Made by tools/make_ui_icons.py: do not edit by hand.\n#pragma once\n\nstruct UiIcon { int w, h; const char *const *rows; };\n'
for name, im in ICONS:
    r = rows(im)
    text += f'\nstatic const char *const UI_{name}_ROWS[] = {{\n' + ''.join(f'  "{l}",\n' for l in r) + '};\n'
    text += f'static const UiIcon UI_{name} = {{{len(r[0])}, {len(r)}, UI_{name}_ROWS}};\n'
open('src/ui_icons.h', 'w').write(text)
print('wrote src/ui_icons.h')
