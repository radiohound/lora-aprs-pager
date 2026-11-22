// Raster tile benchmark for the T-LoRa Pager (env: bench). Read-only: touches the SD card
// and the screen, never the radio. Measures, per zoom level found on the card:
//   read   = SD file -> PSRAM buffer
//   decode = JPEG/PNG -> 256x256 RGB565 sprite in PSRAM
//   push   = sprite -> display
//   frame6 = read+decode+push of 6 consecutive tiles (about one 480x222 view, no cache)
// Also times raw SD reads at several SPI clocks. Results go to USB serial and the screen.
// Looks for tiles in /LoRa_Tracker/Maps/<region>/<z>/<x>/<y>.(jpg|png), then /maps/<region>/...
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#ifdef BENCH_TDECK
// T-Deck (ESP32-S3, same CPU/clock as the pager): SD CS 39, LoRa CS 9, display CS 12, power-on GPIO 10.
#include "tdeck_display.h"
#define BOARD_NAME "T-Deck"
#define PIN_SD_CS 39
#define PIN_SCK 40
#define PIN_MISO 38
#define PIN_MOSI 41
#define PAGER_ROTATION 1
static TDeckDisplay tft;
static void boardPower() {
  pinMode(10, OUTPUT); digitalWrite(10, HIGH); delay(200);   // BOARD_POWERON
  for (uint8_t p : {9, 39, 12}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }   // LoRa, SD, display CS high
}
#else
#include "../pins.h"
#include "../xl9555.h"
#include "../display.h"
#define BOARD_NAME "T-LoRa Pager"
static XL9555 xp;
static PagerDisplay tft;
static void boardPower() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  if (xp.begin(ADDR_XL9555)) {
    for (uint8_t p : {XP_SD_EN, XP_GPIO_EN}) { xp.output(p, HIGH); delay(2); }
    xp.input(XP_SD_PULLEN);
  } else Serial.println("XL9555 not found; SD may be unpowered");
  for (uint8_t p : {PIN_LORA_CS, PIN_SD_CS, PIN_NFC_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
}
#endif
static lgfx::LGFX_Sprite spr(&tft);
static const int PER_ZOOM = 12;        // tiles sampled per zoom level

struct Tile { String path; bool png; uint32_t bytes; };
static int line = 0;

static void say(const char *fmt, ...) {
  char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  tft.fillRect(0, 2 + line * 12, 480, 12, TFT_BLACK);
  tft.setCursor(2, 2 + line * 12); tft.print(b);
  line = (line + 1) % 17; if (line == 0) { delay(4000); tft.fillScreen(TFT_BLACK); }
}

static bool isNum(const String &s) { if (!s.length()) return false; for (char c : s) if (!isdigit(c)) return false; return true; }

// first directory entry (not . or ..) with a given test, "" if none
static String nextDir(File &d) {
  while (true) {
    File e = d.openNextFile(); if (!e) return "";
    String n = e.name(); bool dir = e.isDirectory(); e.close();
    if (dir && isNum(n)) return n;
  }
}

static std::vector<Tile> collect(const String &zdir) {
  std::vector<Tile> out;
  File z = SD.open(zdir);
  if (!z) return out;
  while ((int)out.size() < PER_ZOOM) {
    String xs = nextDir(z); if (xs == "") break;
    File xd = SD.open(zdir + "/" + xs);
    while ((int)out.size() < PER_ZOOM) {
      File f = xd.openNextFile(); if (!f) break;
      String n = f.name(); bool dir = f.isDirectory(); uint32_t sz = f.size(); f.close();
      if (dir) continue;
      String l = n; l.toLowerCase();
      bool jpg = l.endsWith(".jpg") || l.endsWith(".jpeg"), png = l.endsWith(".png");
      if (jpg || png) out.push_back({zdir + "/" + xs + "/" + n, png, sz});
    }
    xd.close();
  }
  z.close();
  return out;
}

static uint8_t *buf = nullptr; static const size_t BUFSZ = 400 * 1024;

static bool readTile(const Tile &t, uint32_t &len, uint32_t &us) {
  uint32_t t0 = micros();
  File f = SD.open(t.path); if (!f) return false;
  len = f.size(); if (len > BUFSZ) { f.close(); return false; }
  size_t got = f.read(buf, len); f.close();
  us = micros() - t0;
  return got == len;
}
static bool decodeTile(const Tile &t, uint32_t len, uint32_t &us) {
  uint32_t t0 = micros();
  bool ok = t.png ? spr.drawPng(buf, len, 0, 0) : spr.drawJpg(buf, len, 0, 0);
  us = micros() - t0; return ok;
}

void setup() {
  Serial.begin(115200); delay(1500);
  Serial.println("\n=== " BOARD_NAME " raster benchmark ===");
  boardPower();
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  tft.init(); tft.setRotation(PAGER_ROTATION); tft.setBrightness(160);
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.setTextSize(1);
  say(BOARD_NAME " raster benchmark, %dx%d", tft.width(), tft.height());

  say("PSRAM found=%d size=%u free=%u KB; heap free=%u KB", psramFound(), (unsigned)ESP.getPsramSize(),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
  buf = (uint8_t *)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
  spr.setPsram(true); spr.setColorDepth(16);
  bool spOk = spr.createSprite(256, 256) != nullptr;
  if (!buf || !spOk) { say("Allocation failed: buffer %s, sprite %s", buf ? "ok" : "FAIL", spOk ? "ok" : "FAIL"); return; }

  // find a tile root
  String root;
  const char *cands[] = {"/LoRa_Tracker/Maps", "/maps"};
  uint32_t mhz[] = {4, 10, 20, 40};
  bool mounted = false;
  for (uint32_t f : mhz) {
    SD.end();
    if (SD.begin(PIN_SD_CS, SPI, f * 1000000)) { mounted = true; break; }
  }
  if (!mounted) { say("SD mount failed"); return; }
  say("SD %llu MB", SD.cardSize() / (1024 * 1024));
  for (auto c : cands) { if (SD.exists(c)) { root = c; break; } }
  if (root == "") { say("No tile folder found"); return; }
  File r = SD.open(root); String region;
  while (true) { File e = r.openNextFile(); if (!e) break; if (e.isDirectory()) { region = e.name(); e.close(); break; } e.close(); }
  r.close();
  if (region == "") { say("No region folder in %s", root.c_str()); return; }
  root += "/" + region;
  say("Region: %s", root.c_str());

  // raw SD read speed at several clocks, using the first tile found
  std::vector<Tile> probe;
  { File d = SD.open(root); while (probe.empty()) { String z = nextDir(d); if (z == "") break; probe = collect(root + "/" + z); } d.close(); }
  if (probe.empty()) { say("No tiles under %s", root.c_str()); return; }
  for (uint32_t f : mhz) {
    SD.end();
    if (!SD.begin(PIN_SD_CS, SPI, f * 1000000)) { say("SD @%lu MHz: mount failed", (unsigned long)f); continue; }
    uint32_t tot = 0, us = 0;
    for (int rep = 0; rep < 3; rep++) for (auto &t : probe) { uint32_t l, u; if (readTile(t, l, u)) { tot += l; us += u; } }
    say("SD @%2lu MHz: %lu KB in %lu ms = %.0f KB/s", (unsigned long)f, (unsigned long)(tot / 1024), (unsigned long)(us / 1000), us ? tot / 1.024 / us * 1000 : 0.0);
  }
  // use 20 MHz for the tile tests (a common safe maximum for a shared bus)
  SD.end(); SD.begin(PIN_SD_CS, SPI, 20000000);

  Serial.println("\nzoom  n   avgKB  read_ms  decode_ms  push_ms  total_ms  | frame6_ms");
  File rd = SD.open(root);
  std::vector<String> zooms;
  while (true) { String z = nextDir(rd); if (z == "") break; zooms.push_back(z); }
  rd.close();
  for (auto &z : zooms) {
    auto tiles = collect(root + "/" + z);
    if (tiles.empty()) continue;
    double sb = 0, sr = 0, sd = 0, sp = 0, worst = 0; int n = 0, fail = 0;
    for (auto &t : tiles) {
      uint32_t len, ur, ud;
      if (!readTile(t, len, ur)) { fail++; continue; }
      if (!decodeTile(t, len, ud)) { fail++; continue; }
      uint32_t t0 = micros(); spr.pushSprite(100, -17); uint32_t up = micros() - t0;
      sb += len; sr += ur; sd += ud; sp += up; n++;
      double tot = ur + ud + up; if (tot > worst) worst = tot;
    }
    if (!n) { say("z%s: all %d tiles failed", z.c_str(), fail); continue; }
    // frame of 6 tiles (one view, no cache)
    uint32_t f0 = micros(); int drawn = 0;
    for (int i = 0; i < 6 && i < (int)tiles.size(); i++) {
      uint32_t len, u; if (readTile(tiles[i], len, u) && decodeTile(tiles[i], len, u)) { spr.pushSprite((i % 3) * 160, (i / 3) * 111 - 17); drawn++; }
    }
    double fr = (micros() - f0) / 1000.0;
    char msg[160];
    snprintf(msg, sizeof msg, "z%-2s n=%d %.1fKB rd %.0f dec %.0f push %.0f = %.0f ms | 6 tiles %.0f ms%s", z.c_str(), n, sb / n / 1024, sr / n / 1000, sd / n / 1000, sp / n / 1000, (sr + sd + sp) / n / 1000, fr, fail ? " (fails)" : "");
    say("%s", msg);
    Serial.printf("%-4s %3d %6.1f %8.1f %10.1f %8.1f %9.1f  | %8.1f  worst %.0f ms, 6-tile frame drew %d\n", z.c_str(), n, sb / n / 1024, sr / n / 1000, sd / n / 1000, sp / n / 1000, (sr + sd + sp) / n / 1000, fr, worst / 1000, drawn);
  }
  say("Done. Free PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
void loop() { delay(1000); }
