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
4. Map: raster tiles from SD (`<z>/<x>/<y>.jpg|png`), own position.
5. Heard stations on the map.
6. Screens, messages, settings, power saving.

## Map design rules (decided 2026-11-22)
- Raster first, vector later. Raster: read the standard `<z>/<x>/<y>.jpg|png` tile layout (256 px, FAT32), the same
  layout a T-Deck LoRa APRS card uses, so one SD card works in both. No code or assets from the T-Deck firmware.
- Scan all region folders and let the user choose (the T-Deck firmware cannot); accept any zoom present on the card.
- Draw our own station symbols; draw a small data-credit line on the map screen (OSM, USGS, etc. as the tiles require).
- Vector later: render Mapbox Vector Tiles from the terrain pipeline's PMTiles (PMTiles reader + MVT decoder + a style).
  The T-Deck's `.nav` vector format is not planned.
- SD: LilyGO documents "up to 32 GB"; test the 128 GB FAT32 T-Deck card on the real pager.

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
