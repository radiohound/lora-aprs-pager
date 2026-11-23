#!/usr/bin/env python3
"""Take a screenshot of the T-LoRa Pager running the application firmware, over USB.

Usage: tools/screenshot.py [out.png] [--port /dev/cu.usbmodemXXXX] [--send 'V1\\nG37.77,-122.49,12\\n'] [--wait 1.5]
Needs pyserial and Pillow. On this Mac: PYTHONPATH=$HOME/.platformio/penv/lib/python3.11/site-packages python3 tools/screenshot.py out.png
(system python3 has Pillow, the PlatformIO venv has pyserial).
The firmware answers an 'S' byte with its off-screen frame buffer. The port is opened WITHOUT touching DTR/RTS:
setting them makes the ESP32-S3's USB-Serial/JTAG reset the chip.
"""
import glob, sys, time
import serial
from PIL import Image

# optional: --send "V1\nG37.77,-122.49,12\n" sends test commands first (set the view / map position), --wait 1.5 waits that long before the screenshot
def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
send = opt("--send"); wait = float(opt("--wait", "1.5"))
skip = {opt("--send"), opt("--wait"), opt("--port")}
args = [a for a in sys.argv[1:] if not a.startswith("--") and a not in skip]
out = args[0] if args else "pager-screenshot.png"
port = sys.argv[sys.argv.index("--port") + 1] if "--port" in sys.argv else (glob.glob("/dev/cu.usbmodem*") or [None])[0]
if not port:
    sys.exit("no /dev/cu.usbmodem* port found")

s = serial.Serial(port, 115200, timeout=0.5)
s.reset_input_buffer()
if send:
    s.write(send.encode().decode("unicode_escape").encode()); s.flush(); time.sleep(wait)
    s.reset_input_buffer()
s.write(b"S")
buf = bytearray(); t0 = time.time(); w = h = 0
while time.time() - t0 < 15:
    buf += s.read(65536)
    i = buf.find(b"SHOT ")
    if i >= 0 and b"\n" in buf[i:]:
        j = buf.index(b"\n", i); w, h = map(int, buf[i + 5:j].split()); start = j + 1
        if len(buf) >= start + w * h * 2:
            raw = bytes(buf[start:start + w * h * 2]); break
else:
    sys.exit("no screenshot received (is the application firmware running?)")

img = Image.new("RGB", (w, h))
px = img.load()
for k in range(w * h):
    v = (raw[2 * k] << 8) | raw[2 * k + 1]          # the sprite stores RGB565 big-endian
    px[k % w, k // w] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
img.save(out)
print(f"saved {out} ({w}x{h})")
