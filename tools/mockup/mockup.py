#!/usr/bin/env python3
"""Place a device screenshot into a photo/illustration of the device.

  mockup.py DEVICE.png SHOT.png OUT.png --corners TLx,TLy,TRx,TRy,BRx,BRy,BLx,BLy
  mockup.py --cfg devices/pager.json SHOT.png OUT.png
  mockup.py DEVICE.png --grid grid.png          # draw a coordinate grid to read the corners off
  mockup.py DEVICE.png SHOT.png OUT.png --corners ... --outline   # outline the quad to check it

Corners are in DEVICE-image pixels, clockwise from the screen's top-left: TL, TR, BR, BL.
The screenshot is warped (perspective) into that quad with 4x supersampling for clean edges.
Options: --glass N (0-1, soft diagonal reflection), --inset PX (shrink the quad slightly, hides a
photo's bezel edge), --scale S (scale the finished picture). Needs Pillow and numpy.
"""
import argparse, json, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 4  # supersampling factor for the warp

def persp_coeffs(dst, src):
    """PIL PERSPECTIVE coefficients mapping each output point (dst) to the input point (src)."""
    A, B = [], []
    for (x, y), (u, v) in zip(dst, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); B.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); B.append(v)
    return np.linalg.solve(np.array(A, float), np.array(B, float)).tolist()

def shrink(quad, inset):
    if not inset: return quad
    c = np.mean(quad, axis=0); out = []
    for p in quad:
        d = c - np.array(p); n = np.linalg.norm(d)
        out.append(tuple(np.array(p) + d / n * inset))
    return out

def place(device, shot, quad, glass=0.0):
    W, H = device.size
    big = (W * SS, H * SS)
    q = [(x * SS, y * SS) for x, y in quad]
    sw, sh = shot.size
    src = [(0, 0), (sw, 0), (sw, sh), (0, sh)]
    co = persp_coeffs(q, src)
    shot_rgba = shot.convert("RGBA")
    warped = shot_rgba.transform(big, Image.PERSPECTIVE, co, Image.BICUBIC)
    mask = Image.new("L", (sw, sh), 255).transform(big, Image.PERSPECTIVE, co, Image.BILINEAR)
    if glass > 0:
        g = np.zeros((big[1], big[0]), np.float32)
        yy, xx = np.mgrid[0:big[1], 0:big[0]].astype(np.float32)
        t = (xx / big[0] * 0.8 + yy / big[1] * 0.2)
        g = np.clip(1 - np.abs(t - 0.3) * 6, 0, 1) * 0.5 + np.clip(1 - np.abs(t - 0.62) * 14, 0, 1) * 0.25
        hl = Image.new("RGBA", big, (255, 255, 255, 0)); hl.putalpha(Image.fromarray((g * 255 * glass).astype(np.uint8)))
        warped = Image.alpha_composite(warped, hl)
    layer = Image.new("RGBA", big, (0, 0, 0, 0)); layer.paste(warped, (0, 0), mask)
    layer = layer.resize((W, H), Image.LANCZOS)
    return Image.alpha_composite(device.convert("RGBA"), layer)

def grid(device, step=100):
    im = device.convert("RGBA"); d = ImageDraw.Draw(im); W, H = im.size
    try: f = ImageFont.load_default(size=14)
    except TypeError: f = ImageFont.load_default()
    for x in range(0, W, step):
        d.line([(x, 0), (x, H)], fill=(255, 0, 0, 150), width=1); d.text((x + 3, 3), str(x), fill=(255, 255, 0, 255), font=f)
    for y in range(0, H, step):
        d.line([(0, y), (W, y)], fill=(255, 0, 0, 150), width=1); d.text((3, y + 3), str(y), fill=(255, 255, 0, 255), font=f)
    return im

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="DEVICE SHOT OUT  (or SHOT OUT with --cfg; or DEVICE with --grid)")
    ap.add_argument("--cfg", help="JSON with image, corners and optional glass/inset/scale")
    ap.add_argument("--corners"); ap.add_argument("--glass", type=float); ap.add_argument("--inset", type=float)
    ap.add_argument("--scale", type=float); ap.add_argument("--grid"); ap.add_argument("--outline", action="store_true")
    a = ap.parse_args()
    cfg, f = {}, a.files
    if a.cfg:
        cfg = json.load(open(a.cfg)); dev_path = os.path.join(os.path.dirname(os.path.abspath(a.cfg)), cfg["image"])
        if len(f) != 2: ap.error("with --cfg give SHOT OUT")
        shot_path, out_path = f
    else:
        if not f: ap.error("give DEVICE ...")
        dev_path = f[0]; shot_path, out_path = (f[1:] + [None, None])[:2]
    dev = Image.open(dev_path)
    if a.grid:
        grid(dev).save(a.grid); print("wrote", a.grid, dev.size); return
    corners = a.corners or ",".join(map(str, cfg.get("corners", [])))
    if not (corners and shot_path and out_path): ap.error("need DEVICE SHOT OUT and --corners (or --cfg)")
    v = [float(t) for t in corners.split(",")]
    if len(v) != 8: ap.error("corners needs 8 numbers: TLx,TLy,TRx,TRy,BRx,BRy,BLx,BLy")
    quad = shrink([(v[i], v[i + 1]) for i in range(0, 8, 2)], a.inset if a.inset is not None else cfg.get("inset", 0))
    out = place(dev, Image.open(shot_path), quad, a.glass if a.glass is not None else cfg.get("glass", 0.0))
    if a.outline:
        ImageDraw.Draw(out).polygon(quad, outline=(255, 0, 255, 255))
    sc = a.scale if a.scale is not None else cfg.get("scale", 1.0)
    if sc != 1.0: out = out.resize((int(out.width * sc), int(out.height * sc)), Image.LANCZOS)
    out.save(out_path); print("wrote", out_path, out.size)

if __name__ == "__main__":
    main()
