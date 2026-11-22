# pager-tracker

LoRa APRS (433.775 MHz) tracker and station map for the LilyGO T-LoRa Pager (433 MHz).
Own firmware, own map renderer; reuses the portable C frame code from the E77 tracker
(`LoRa-Tracker-Digipeater/lib/lacf3`, MIT) later, once the radio works.

Research (2026-11-22): no existing LoRa APRS firmware for the Pager and no handheld that draws
heard LoRa APRS stations on its own screen was found. "No prior art found", not proof of first.
Nearest: Trail Mate (Pager, SD tile map, Meshtastic/MeshCore only), 2E0LXY T-Deck Plus tracker
(map planned, 439.9125 MHz), dk8hc T-Deck+ (list only), CA2RXU tracker (no Pager, no map).

## Hardware (pins in `src/pins.h`, from LilyGoLib + Meshtastic variant, which agree)
ESP32-S3 16 MB flash / 8 MB QSPI PSRAM; ST7796 480x222 colour, no reset line; SX1262 (LilyGO docs)
or LR1121 (Meshtastic device page) - milestone 1 probes both; MIA-M10Q GPS 38400 baud; TCA8418
keyboard; rotary encoder; XL9555 expander powers LoRa/GPS/keyboard/SD and MUST be initialised
before them; one SPI bus shared by display, SD, LoRa, NFC.

## Milestones
1. **Bring-up (this commit):** expander, I2C scan, display test, battery, SD, GPS NMEA, radio probe and RX-only listen. NOT YET RUN ON HARDWARE.
2. Radio: decode received LoRa APRS packets.
3. Tracker: GPS beacons (needs licence/callsign settings; nothing transmits before this).
3b. Messaging (see `docs/messaging.md`): receive and show, then send with ack/retry, conversations, reply, delete.
4. Map: raster tiles from SD (`<z>/<x>/<y>.jpg|png`), own position.
5. Heard stations on the map.
6. Screens, messages, settings, power saving.

## Controls
The rotary encoder is a primary control everywhere: every screen and menu must be fully usable with the knob alone (turn = move, press = select, long press = back/context menu), plus scrolling threads and zooming the map; the keyboard types and gives shortcuts. See `docs/messaging.md`.

## Map design rules (decided 2026-11-22)
- Raster first, vector later. Raster: read the standard `<z>/<x>/<y>.jpg|png` tile layout (256 px, FAT32), the same
  layout a T-Deck LoRa APRS card uses, so one SD card works in both. No code or assets from the T-Deck firmware.
- Scan all region folders and let the user choose (the T-Deck firmware cannot); accept any zoom present on the card.
- Draw our own station symbols; draw a small data-credit line on the map screen (OSM, USGS, etc. as the tiles require).
- Vector later: render Mapbox Vector Tiles from the terrain pipeline's PMTiles (PMTiles reader + MVT decoder + a style).
  The T-Deck's `.nav` vector format is not planned.
- SD: LilyGO documents "up to 32 GB"; test the 128 GB FAT32 T-Deck card on the real pager.

## Raster benchmark (src/bench, `pio run -e bench` for the pager, `-e bench_tdeck` for a T-Deck)
Read-only (SD + screen, no radio). Per zoom: SD read, JPEG decode into a 256x256 RGB565 sprite in PSRAM, push to display,
and a 6-tile frame (about one 480x222 view, no cache).
**T-Deck result 2026-11-22** (ESP32-S3 240 MHz, 128 GB FAT32 card, BAYAREA terrain tiles, first 12 tiles per zoom,
20 MHz SD): read 22-23 ms, decode 48-51 ms, push 21 ms (40 MHz panel, 256x256) = ~91-96 ms per z10/z12 tile (15-17 KB);
a 6-tile frame with no cache is 430-490 ms. z14 sample was small (2 KB tiles, probably sparse) so 65 ms/tile is optimistic.
SD read speed saturates: 4 MHz 251 KB/s, 10 MHz 489, 20 MHz 726, 40 MHz 734 KB/s (so use 20 MHz).
Decode is about half the time, so a decoded-tile cache in PSRAM (128 KB per tile) matters more than anything else.
**PSRAM gotcha:** the T-Deck's 8 MB PSRAM is OCTAL: with `qio_qspi` it was not found at all (found=0, no error),
with `qio_opi` it works. The pager's PSRAM type is unverified (an agent said QSPI): run `-e bench` and if it prints
`PSRAM found=0` try `qio_opi`. Pager numbers still to be measured (display push should be faster at 75 MHz).

## Vector tile workload (measured on the Mac, 2026-11-22, `tools/count_vec.py`)
Pipeline archives california_r2_vector.pmtiles + _vector_osm.pmtiles (MVT, gzip, z6-14), tiles summed. Land cover is a raster
PNG archive, so forest/landcover shading is NOT in the vector data; polygons are rare (water only).
| area, zoom | gz KB | features | vertices |
| SF downtown z12 / z14 | 36 / 33 | 3,791 / 2,517 | 8.5k / 5.3k |
| San Jose downtown z12 / z14 | 49 / 25 | 4,972 / 1,769 | 12.0k / 4.3k |
| Sunnyvale suburb z14 | 11 | 638 | 2.1k |
| Mt Hamilton (open) z12 / z14 | 7 / 5 | 175 / 57 | 3.0k / 1.6k |
| Yosemite Valley z12 / z14 | 17 / 12 | 542 / 415 | 6.4k / 2.7k |
| High Sierra z12 / z14 | 8.5 / 2.8 | 239 / 45 | 3.3k / 0.8k |
z10 tiles are unfiltered and huge (San Jose 181 KB gz, 22k features, 51k vertices): a device renderer must filter by road
class per zoom. Vector is smaller than the 15-17 KB JPEGs only in open country; dense towns are 25-50 KB. Lines dominate
(roads, trails, streams, contours). Break-even vs the uncached raster frame (~450 ms): a 6-tile dense view is ~15k features /
~30k vertices, so vector must draw about 33 features/ms to match. Drawing speed on the ESP32-S3 is NOT measured yet.

## Before the first flash
Take a full flash backup so the factory firmware can be restored:

    ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
      --chip esp32s3 --port <PORT> read_flash 0x0 0x1000000 pager-factory-16MB.bin

The pager may need DFU mode (hold BOOT while plugging USB). Restore with `write_flash 0x0 <file>`.

## Unverified
- Display rotation / offset (`PAGER_ROTATION`, `offset_rotation` in `src/display.h`): check the TL/TR/BL/BR corner labels.
- BQ27220 read commands 0x08 (mV) and 0x2C (%): from memory of the datasheet, not checked.
- Which LoRa chip is fitted; the radio-detect ADC pin LilyGoLib uses is not used here.
- Whether the pager has touch (GT911 mentioned only in a search snippet).

Build: `~/.platformio/penv/bin/pio run`.
