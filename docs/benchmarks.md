# Measurements

Moved out of the README on 2026-11-23. Numbers are from real runs unless a line says otherwise.

## Raster benchmark (src/bench, `pio run -e bench` for the pager, `-e bench_tdeck` for a T-Deck)
Read-only (SD + screen, no radio). Per zoom: SD read, JPEG decode into a 256x256 RGB565 sprite in PSRAM, push to display,
and a 6-tile frame (about one 480x222 view, no cache).
**T-Deck result 2026-11-22** (ESP32-S3 240 MHz, 128 GB FAT32 card, BAYAREA terrain tiles, first 12 tiles per zoom,
20 MHz SD): read 22-23 ms, decode 48-51 ms, push 21 ms (40 MHz panel, 256x256) = ~91-96 ms per z10/z12 tile (15-17 KB);
a 6-tile frame with no cache is 430-490 ms. z14 sample was small (2 KB tiles, probably sparse) so 65 ms/tile is optimistic.
SD read speed saturates: 4 MHz 251 KB/s, 10 MHz 489, 20 MHz 726, 40 MHz 734 KB/s (so use 20 MHz).
Decode is about half the time, so a decoded-tile cache in PSRAM (128 KB per tile) matters more than anything else.
**PSRAM gotcha:** the T-Deck's 8 MB PSRAM is OCTAL: with `qio_qspi` it was not found at all (found=0, no error),
with `qio_opi` it works. **The pager's PSRAM is QSPI** (confirmed 2026-11-22: `qio_qspi` finds it, about 7.6 MB free at boot).

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

**Pager result 2026-11-22** (same BAYAREA card, 20 MHz SD, z8 tiles, 4 tiles at that zoom on the card): 17.3 KB average,
read 26 ms, decode 53 ms, push 25 ms = about 105 ms per tile; a frame of those tiles took 392 ms with no cache.
With the viewer's 20-tile decoded cache in PSRAM, a redraw of a view whose tiles are all cached takes 22-25 ms (the time shown in the
viewer's top line); a pan that needs new tiles costs about 100 ms per new tile. The other zoom levels were not read off the screen
(each zoom overwrote the previous result line); the viewer shows its own redraw time at every zoom.
