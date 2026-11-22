# Device mockups

Put a screenshot of the device's screen into a picture of the device (no phone photo of the screen).

    # 1. once per device: read the screen corners off a coordinate grid
    python mockup.py devices/pager.png --grid grid.png
    # 2. write devices/pager.json (copy devices/example.json), then for every screenshot:
    python mockup.py --cfg devices/pager.json shot.png pager-map.png

Corners are clockwise from the screen's top-left (TL, TR, BR, BL) in the device image's pixels, so a photo taken
slightly off-axis works (perspective warp). `--outline` draws the quad to check it, `--inset N` shrinks it to hide a
photo's bezel edge, `--glass 0-1` adds a soft reflection, `--scale` resizes the result.

Use your OWN photo (flat, square-on, screen off) or an illustration of each device: LilyGO's product photos are theirs.
Needs Pillow and numpy (`pip install pillow numpy`). `devices/example_device.png` is a synthetic test picture, not a real device.
