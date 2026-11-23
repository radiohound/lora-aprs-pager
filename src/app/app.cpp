// The application: LoRa APRS tracker, station map and messenger for the T-LoRa Pager (see README.md).
// Map from the T-Deck card layout (/LoRa_Tracker/Maps/<REGION>/<z>/<x>/<y>.jpg|png, read-only), heard stations, beacon, messaging, phone setup.
// Knob: turn = pan / zoom / select, press = switch what the knob does, long press = centre on me.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include "pins.h"
#include "xl9555.h"
#include "display.h"
#include "haptic.h"
#include <RadioLib.h>
#include "aprs.h"
#include "nmea.h"
#include "inputs.h"
#include "keymap.h"
#include "geo.h"
#include "beacon.h"
#include "settings_core.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "webui.h"
#include <LittleFS.h>
#include "messaging.h"
#include "clock.h"

static XL9555 xp;
static PagerDisplay tft;
static Haptic haptic;
static Keys keys;
static lgfx::LGFX_Sprite fb(&tft);   // off-screen frame buffer (PSRAM) so redraws never show half-drawn
static bool fbOk = false;

// ---- LoRa APRS receive (433.775 MHz, SF12, BW125, CR4/5, private sync 0x12). Receive only: this build never transmits. ----
static SX1262 *radio = nullptr;
static volatile bool rxFlag = false;
static void IRAM_ATTR onRx() { rxFlag = true; }
static bool radioOk = false;
static unsigned long rxCount = 0;

// ---- My beacon (transmit). Auto-beaconing starts OFF at every boot; the T key switches it on, B sends one now. ----
static const char *TOCALL = "APZPGR";          // experimental tocall (APZ...), pager tracker; to be replaced by a registered one
static PagerSettings S;                         // call sign, path, symbol, power, intervals...: set on the phone setup page, kept in flash
static PagerSettings loadSettings();           // defined further down
static bool autoBeacon = false;
static bool demoMode = false;     // test hook D: simulated stations and position for screenshots; NOTHING is transmitted while it is on (RST ends it)
static BeaconScheduler beacon;
static unsigned long txCount = 0;

// ---- Stations heard (positions only for now) ----
struct Station { char call[10]; double lat, lon; char table, sym; uint32_t heard, first; float rssi, snr; bool used;
                 uint16_t count; char raw[121], path[40], via[10], comment[44]; };   // raw = the last packet as text; via = the digipeater that repeated it ("" = heard directly)
static Station stations[48];
static char lastHeardCall[10] = "";                 // the station heard most recently, and when (the full list lights it up)
static uint32_t lastHeardAt = 0, lastKnobAt = 0;      // lastKnobAt: when the knob last moved in the full list (it stops following new packets for a while)
static int stationCount() { int n = 0; for (auto &s : stations) if (s.used) n++; return n; }
static void stationUpdate(const AprsPos &p, float rssi, float snr, const char *rawText, size_t rawLen) {
  Station *slot = nullptr, *oldest = nullptr;
  for (auto &s : stations) {
    if (s.used && strcmp(s.call, p.call) == 0) { slot = &s; break; }
    if (!s.used && !slot) slot = &s;
    if (s.used && (!oldest || s.heard < oldest->heard)) oldest = &s;
  }
  if (!slot) slot = oldest;
  bool same = slot->used && strcmp(slot->call, p.call) == 0;
  if (!same) { slot->count = 0; slot->first = millis(); }
  if (slot->count < 65535) slot->count++;
  strncpy(slot->call, p.call, 9); slot->call[9] = 0;
  slot->lat = p.lat; slot->lon = p.lon; slot->table = p.table; slot->sym = p.sym;
  slot->heard = millis(); slot->rssi = rssi; slot->snr = snr; slot->used = true;
  strncpy(slot->comment, p.comment, sizeof slot->comment - 1); slot->comment[sizeof slot->comment - 1] = 0;
  size_t k = std::min(rawLen, sizeof slot->raw - 1);
  for (size_t i = 0; i < k; i++) slot->raw[i] = (rawText[i] >= 32 && rawText[i] < 127) ? rawText[i] : '.';
  slot->raw[k] = 0;
  aprsPathInfo(rawText, rawLen, slot->path, sizeof slot->path, slot->via, sizeof slot->via);
  strncpy(lastHeardCall, p.call, 9); lastHeardCall[9] = 0; lastHeardAt = millis();
}

// ---- GPS ----
static NmeaParser gps;

// Demo data for screenshots (test hook D<lat>,<lon>): simulated stations around a place, with obviously fake call signs.
// The pager acts as if it were at that place and sends nothing at all until it is restarted.
static void loadDemo(double lat, double lon) {
  struct D { const char *call; char table, sym; double dx, dy; int rssi; double snr; const char *path, *via, *comment; int count, ageS; };
  static const D demo[] = {
    {"N0DEMO-1", 'L', '&', 1800, 1200, -78,  4.5, "WIDE1-1",            "",         "LoRa iGate and digipeater", 42, 20},
    {"N0DEMO-2", '/', '[',  -400,  350, -62,  7.0, "N0DEMO-1*,WIDE1",   "N0DEMO-1", "walking the park", 17, 95},
    {"N0DEMO-3", '/', '>', 2500, -900, -91, -3.5, "N0DEMO-1*,WIDE1",   "N0DEMO-1", "driving, 38 km/h", 9, 300},
    {"N0DEMO-4", '/', '-', -1500, -600, -85,  2.0, "WIDE1-1",            "",         "home station", 63, 40},
    {"N0DEMO-5", '/', 'b',   45,   35, -54,  9.5, "WIDE1-1",            "",         "bike ride", 5, 12},
    {"N0DEMO-6", '/', 'k',   95,  -40, -57,  8.0, "N0DEMO-4*,WIDE1",   "N0DEMO-4", "supply truck", 3, 150},
    {"N0DEMO-7", '/', '#',  -55,   80, -49, 10.5, "WIDE1-1",            "",         "digipeater", 120, 5},
    {"N0DEMO-8", '/', 'O', -3000, 2200, -108, -12.5, "N0DEMO-1*,N0DEMO-4*", "N0DEMO-4", "balloon, 1520 m", 2, 1500},
  };
  memset(stations, 0, sizeof stations);
  int i = 0;
  for (auto &d : demo) {
    Station &st = stations[i++]; st.used = true;
    strncpy(st.call, d.call, 9); st.table = d.table; st.sym = d.sym;
    st.lat = lat + d.dy / 111320.0; st.lon = lon + d.dx / (111320.0 * cos(lat * M_PI / 180.0));
    st.rssi = d.rssi; st.snr = (float)d.snr; st.count = d.count; st.heard = millis() - (uint32_t)d.ageS * 1000UL; st.first = st.heard - 600000UL;
    strncpy(st.path, d.path, sizeof st.path - 1); strncpy(st.via, d.via, sizeof st.via - 1); strncpy(st.comment, d.comment, sizeof st.comment - 1);
    char pos[24]; aprsFormatPosition(pos, sizeof pos, st.lat, st.lon, d.table, d.sym);
    snprintf(st.raw, sizeof st.raw, "%s>APZDEM,%s:!%s%s", d.call, d.path, pos, d.comment);
  }
  gps.fix.valid = true; gps.fix.lat = lat; gps.fix.lon = lon; gps.fix.sats = 9;       // "me" is at the demo place too
  demoMode = true; autoBeacon = false;
  strcpy(lastHeardCall, "N0DEMO-7"); lastHeardAt = millis();                              // lights up in the full list for a few seconds
}

static bool dirty = false;
static uint32_t lastDraw = 0;
static const char *notice = nullptr; static uint32_t noticeUntil = 0;
static void say(const char *m) { notice = m; noticeUntil = millis() + 2500; dirty = true; }

// ---- Clock and battery. Time: from GPS (UTC), kept running with millis(), and saved to the RTC chip (PCF85063) so it is right at power-on
// even with no fix. Shown in local time using the offset from the setup page. Battery: the BQ27220 fuel gauge (percent), BQ25896 charger (charging).
static uint32_t clkEpoch = 0, clkAt = 0, clkGpsSeen = 0, rtcWrittenAt = 0; static bool rtcWritten = false;
static int batPct = -1; static bool batCharging = false;
static int i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr); Wire.write(reg); if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(addr, n) != n) return -1;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return 0;
}
static void rtcRead() {                                  // seconds, minutes, hours, days, weekday, months, years (BCD); bit 7 of seconds = the clock stopped
  uint8_t r[7]; if (i2cRead(ADDR_PCF85063, 0x04, r, 7) != 0 || (r[0] & 0x80)) return;
  int y = 2000 + clockFromBcd(r[6]), mo = clockFromBcd(r[5] & 0x1F), d = clockFromBcd(r[3] & 0x3F);
  if (y < 2024 || mo < 1 || mo > 12 || d < 1 || d > 31) return;
  clkEpoch = (uint32_t)(clockDays(y, mo, d) * 86400 + clockFromBcd(r[2] & 0x3F) * 3600 + clockFromBcd(r[1] & 0x7F) * 60 + clockFromBcd(r[0] & 0x7F)); clkAt = millis();
}
static void rtcWrite(uint32_t utc) {
  LocalTime t = clockBreak(utc);
  Wire.beginTransmission(ADDR_PCF85063); Wire.write(0x04);
  Wire.write(clockToBcd(t.sec)); Wire.write(clockToBcd(t.min)); Wire.write(clockToBcd(t.hour)); Wire.write(clockToBcd(t.day));
  Wire.write((uint8_t)t.wday); Wire.write(clockToBcd(t.month)); Wire.write(clockToBcd(t.year % 100)); Wire.endTransmission();
}
static uint32_t utcNow() { return clkEpoch ? clkEpoch + (millis() - clkAt) / 1000 : 0; }
static void clockTick() {                                // once a second: take new GPS time, save it to the RTC now and then, read the battery
  if (gps.fix.epoch && gps.fix.epoch != clkGpsSeen && gps.fix.epoch > 1700000000UL) {
    clkGpsSeen = gps.fix.epoch; clkEpoch = gps.fix.epoch; clkAt = millis();
    if (!rtcWritten || millis() - rtcWrittenAt > 600000UL) { rtcWrite(clkEpoch); rtcWritten = true; rtcWrittenAt = millis(); }
  }
  static uint32_t batAt = 0;
  if (!batAt || millis() - batAt > 5000) {
    batAt = millis(); uint8_t b[2];
    if (i2cRead(ADDR_BQ27220, 0x2C, b, 2) == 0) { int p = b[0] | (b[1] << 8); batPct = p > 100 ? 100 : p; }
    uint8_t st; if (i2cRead(ADDR_BQ25896, 0x0B, &st, 1) == 0) { int cs = (st >> 3) & 3; batCharging = cs == 1 || cs == 2; }   // 1 = pre-charge, 2 = fast charge
  }
}
// Battery shape for the top right corner: white outline, green fill for the charge (yellow below 30%, red below 15%), black for the empty part,
// a white "+" on it while charging. Drawn only when switched on in the setup page.
static void drawBatIcon(lgfx::LovyanGFX &D, int W) {
  if (!S.batIcon || batPct < 0) return;
  const int w = 28, h = 12, x = W - w - 5, y = 2;
  uint16_t c = batPct > 30 ? TFT_GREEN : (batPct > 15 ? TFT_YELLOW : TFT_RED);
  D.fillRect(x - 1, y - 1, w + 5, h + 2, TFT_BLACK);
  D.drawRect(x, y, w, h, TFT_WHITE); D.fillRect(x + w, y + 3, 2, h - 6, TFT_WHITE);
  D.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
  int fw = (w - 4) * batPct / 100; if (fw < 1 && batPct > 0) fw = 1;
  D.fillRect(x + 2, y + 2, fw, h - 4, c);
  if (batCharging) { D.setTextSize(1); D.setTextColor(TFT_WHITE); D.setCursor(x + w / 2 - 2, y + 2); D.print("+"); }
}
// "Mon 23 Nov 14:05  87%+" (or just the time) drawn with its left edge at x; returns the width in pixels
static int drawClockBat(lgfx::LovyanGFX &D, int x, int y, uint16_t bg, bool withDate, bool rightEdge = false) {   // rightEdge: x is where the text ends
  char t[24] = "--:--", b[10] = "";
  uint32_t u = utcNow();
  if (u) { LocalTime lt = clockLocal(u, S.utcOffset, S.usDst); if (withDate) clockFmt(t, sizeof t, lt); else snprintf(t, sizeof t, "%02d:%02d", lt.hour, lt.min); }
  else if (withDate) strcpy(t, "no time yet");
  if (batPct >= 0 && !S.batIcon) snprintf(b, sizeof b, " %d%%%s", batPct, batCharging ? "+" : "");
  if (rightEdge) x -= 6 * ((int)strlen(t) + (int)strlen(b));
  D.setTextSize(1); D.setCursor(x, y); D.setTextColor(TFT_WHITE, bg); D.print(t);
  if (b[0]) { uint16_t c = batCharging ? TFT_CYAN : (batPct > 30 ? TFT_GREEN : (batPct > 15 ? TFT_YELLOW : TFT_RED)); D.setTextColor(c, bg); D.print(b); }
  return 6 * ((int)strlen(t) + (int)strlen(b));
}

static String root;
static uint8_t *buf = nullptr;
static const size_t BUFSZ = 400 * 1024;
static const int ZOOMS[] = {6, 7, 8, 10, 12, 14};   // the zoom levels the T-Deck card holds
static const int NZ = 6;
static int zi = 3;                                   // start at z10
static double clat = 37.45, clon = -122.15;          // map centre (middle of the BAYAREA box)
enum Mode { M_ZOOM, M_PANX, M_PANY, M_LIST };
static Mode mode = M_PANX;
static const char *MODE_NAME[] = {"ZOOM", "PAN W-E", "PAN N-S", "STATIONS"};
// Station view, stepped by the L key: 0 = list + markers, 1 = markers only (full-width map), 2 = clean map (no stations), 3 = full list (no map)
static int stView = 0;
static bool sortByDistance = true;          // S key: nearest first, or most recently heard first
static char selCall[10] = "";               // selected station (kept by call sign so it survives re-sorting)
static const int LIST_W = 130;
// The four views of the L key, named: shown as a pop-up when you switch and permanently in the top line.
//   LIST+MAP      list of heard stations on the left, map on the right, station icons on the map
//   MAP+STATIONS  the map full width, station icons and call signs on it (no list)
//   MAP ONLY      the map full width with NO stations at all (only you): for reading the map itself
//   FULL LIST     no map: every heard station with its details and packet text
static const uint32_t FOLLOW_PAUSE_MS = 20000;   // after you turn the knob in the full list it stays where you put it this long
static const uint32_t FRESH_MS = 8000;            // a station heard this recently is lit up in the full list
static const char *VIEW_TAG[] = {"LIST+MAP", "MAP+STATIONS", "MAP ONLY", "FULL LIST"};
static const char *VIEW_NAME[] = {"View: LIST + MAP", "View: MAP + STATIONS (no list)", "View: MAP ONLY (no stations)", "View: FULL LIST (no map)"};
static Mode nextMode(Mode m) {
  static const Mode cycle[] = {M_PANX, M_PANY, M_ZOOM, M_LIST};
  int i = 0; for (int k = 0; k < 4; k++) if (cycle[k] == m) i = k;
  Mode n = cycle[(i + 1) % 4];
  if (n == M_LIST && stView != 0) n = cycle[0];    // no list on screen: skip the list mode
  return n;
}

// ---- Web-Mercator maths for 256 px tiles ----
static double worldSize(int z) { return 256.0 * (double)(1UL << z); }
static void toPx(double lat, double lon, int z, double &x, double &y) {
  double n = worldSize(z), r = lat * M_PI / 180.0;
  x = (lon + 180.0) / 360.0 * n;
  y = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n;
}
static void fromPx(double x, double y, int z, double &lat, double &lon) {
  double n = worldSize(z);
  lon = x / n * 360.0 - 180.0;
  lat = atan(sinh(M_PI * (1.0 - 2.0 * y / n))) * 180.0 / M_PI;
}

// ---- Knob: interrupt driven so no detent is lost while a frame is being drawn ----
static volatile int32_t knobSteps = 0;
static volatile uint8_t kLast = 0; static volatile int8_t kAcc = 0;
static void IRAM_ATTR knobIsr() {
  static const int8_t T[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  uint8_t s = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B);
  kAcc += T[(kLast << 2) | s]; kLast = s;
  if (kAcc >= 4) { kAcc = 0; knobSteps++; } else if (kAcc <= -4) { kAcc = 0; knobSteps--; }
}

// ---- Tile cache of decoded 256x256 sprites in PSRAM (a null sprite = tile not on the card) ----
struct CT { int z, x, y; lgfx::LGFX_Sprite *s; uint32_t used; };
static std::vector<CT> cache;
static const size_t MAXC = 20;                       // 20 x 128 KB = 2.5 MB of the ~7.6 MB free PSRAM
static uint32_t tick = 0;
static int misses = 0;

static lgfx::LGFX_Sprite *loadTile(int z, int x, int y) {
  String base = root + "/" + String(z) + "/" + String(x) + "/" + String(y);
  bool png = false;
  File f = SD.open(base + ".jpg");
  if (!f) { f = SD.open(base + ".png"); png = true; }
  if (!f) return nullptr;
  size_t len = f.size(); lgfx::LGFX_Sprite *s = nullptr;
  if (len <= BUFSZ && f.read(buf, len) == len) {
    s = new lgfx::LGFX_Sprite(&tft);
    s->setPsram(true); s->setColorDepth(16);
    bool ok = s->createSprite(256, 256) != nullptr && (png ? s->drawPng(buf, len, 0, 0) : s->drawJpg(buf, len, 0, 0));
    if (!ok) { s->deleteSprite(); delete s; s = nullptr; }
  }
  f.close();
  return s;
}

static lgfx::LGFX_Sprite *getTile(int z, int x, int y) {
  for (auto &c : cache) if (c.z == z && c.x == x && c.y == y) { c.used = ++tick; return c.s; }
  misses++;
  lgfx::LGFX_Sprite *s = loadTile(z, x, y);
  if (cache.size() >= MAXC) {
    size_t lru = 0;
    for (size_t i = 1; i < cache.size(); i++) if (cache[i].used < cache[lru].used) lru = i;
    if (cache[lru].s) { cache[lru].s->deleteSprite(); delete cache[lru].s; }
    cache.erase(cache.begin() + lru);
  }
  cache.push_back({z, x, y, s, ++tick});
  return s;
}

static msg::Messenger M;                       // conversations, acks, retries (src/messaging.h)

// ---- APRS symbols: 24x24 icons (the size the T-Deck uses) baked into the firmware (src/symbols24.bin, made by tools/make_symbols.py from the aprs.fi set) ----
static const int ICON = 24;
extern const uint8_t symbolsBin[] asm("_binary_src_symbols24_bin_start");
extern const uint8_t emojiBin[] asm("_binary_src_emoji18_bin_start");      // 18 x 18 emoticon pictures (src/emoji18.bin)
// Draw the symbol centred on (cx, cy), alpha-blended over what is already in the frame buffer. Table '/' = primary, '\\' = alternate,
// and an overlay character (0-9, A-Z, or a-j in compressed packets for 0-9) = alternate symbol with that letter drawn on top (e.g. I& = iGate).
// Returns false when the code has no icon or the icon would run off the screen.
static void blendRec(uint16_t *px, const uint8_t *rec, int n = ICON * ICON) {      // n = pixels in the icon (24 x 24 symbols, 18 x 18 emoticons)
  // LovyanGFX reads and pushes uint16_t pixels BYTE-SWAPPED (swap565), while the icon files hold ordinary little-endian RGB565:
  // so swap each frame-buffer word before blending and swap the result back.
  auto sw = [](uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); };
  for (int k = 0; k < n; k++) {
    uint8_t a = rec[n * 2 + k];
    if (!a) continue;
    uint16_t fg = rec[k * 2] | (rec[k * 2 + 1] << 8);
    if (a == 255) { px[k] = sw(fg); continue; }
    uint16_t bg = sw(px[k]);
    int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
    int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
    int b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
    px[k] = sw((uint16_t)((r << 11) | (g << 5) | b));
  }
}
static bool drawSymbol(lgfx::LovyanGFX &D, int cx, int cy, char table, char code) {
  int i = (int)code - 33;
  if (i < 0 || i >= 94) return false;
  int x0 = cx - ICON / 2, y0 = cy - ICON / 2;
  if (x0 < 0 || y0 < 0 || x0 + ICON > D.width() || y0 + ICON > D.height()) return false;
  const int recSize = ICON * ICON * 3;
  char ov = table >= 'a' && table <= 'j' ? (char)('0' + (table - 'a')) : table;       // compressed packets send overlay digits as a-j
  bool overlay = (ov >= '0' && ov <= '9') || (ov >= 'A' && ov <= 'Z');
  uint16_t px[ICON * ICON];
  D.readRect(x0, y0, ICON, ICON, px);
  blendRec(px, symbolsBin + ((table == '/' ? 0 : 94) + i) * recSize);
  if (overlay) blendRec(px, symbolsBin + (2 * 94 + (ov - 33)) * recSize);
  D.pushImage(x0, y0, ICON, ICON, px);
  return true;
}

// A label: text on a coloured box with the same padding on all four sides. The 6 x 8 font leaves a blank column at the right and a blank row at the
// bottom of every character cell, so a box drawn exactly around the cells looks too big at the right and bottom; this one is 2 px clear of the ink all round.
static void drawLabel(lgfx::LovyanGFX &D, int x, int y, const char *t, uint16_t fg, uint16_t bg) {
  D.fillRect(x - 2, y - 2, 6 * (int)strlen(t) + 3, 11, bg);
  D.setTextColor(fg); D.setCursor(x, y); D.print(t);                 // one colour argument: no background of its own
}

// ---- Messages screens (they use the stations table, drawSymbol(), haptic and M above) ----
#include "emoticons.h"
#include "msg_ui.h"
static MsgUi ui;

// Messages are kept in the pager's flash (LittleFS on the "spiffs" partition), never on the SD card.
static void msgSave() {
  std::string d = M.serialize(); File f = LittleFS.open("/msgs.bin", "w");
  if (f) { f.write((const uint8_t *)d.data(), d.size()); f.close(); }
}
static void msgLoad() {
  if (!LittleFS.begin(true, "/lfs", 10, "spiffs")) return;
  File f = LittleFS.open("/msgs.bin", "r"); if (!f) return;
  size_t n = f.size(); std::string d(n, 0); if (f.read((uint8_t *)&d[0], n) == n) M.deserialize((const uint8_t *)d.data(), n);
  f.close();
}

// ---- station list: sorted view of the heard stations ----
static int order[48]; static double sdist[48]; static int nOrd = 0;
static void buildOrder() {
  nOrd = 0;
  for (int i = 0; i < 48; i++) {
    if (!stations[i].used) continue;
    sdist[i] = gps.fix.valid ? geoDistKm(gps.fix.lat, gps.fix.lon, stations[i].lat, stations[i].lon) : -1.0;
    order[nOrd++] = i;
  }
  bool byDist = sortByDistance && gps.fix.valid;
  for (int i = 1; i < nOrd; i++) {                      // insertion sort, at most 48 entries
    int v = order[i], j = i - 1;
    while (j >= 0 && (byDist ? sdist[order[j]] > sdist[v] : stations[order[j]].heard < stations[v].heard)) { order[j + 1] = order[j]; j--; }
    order[j + 1] = v;
  }
}
static int selIndex() {                                  // position of the selected call in the sorted list (0 if none)
  for (int i = 0; i < nOrd; i++) if (strcmp(stations[order[i]].call, selCall) == 0) return i;
  return 0;
}

static void drawListPanel(lgfx::LovyanGFX &D, int H) {
  D.fillRect(0, 0, LIST_W, H, TFT_BLACK); D.drawFastVLine(LIST_W - 1, 0, H, TFT_DARKGREY);
  D.setTextSize(1); D.setTextColor(TFT_CYAN, TFT_BLACK); D.setCursor(2, 2);
  D.printf("%d heard  %s", nOrd, (sortByDistance && gps.fix.valid) ? "nearest" : "recent");
  const int rowH = 12, y0 = 14, detailH = 50, rows = (H - y0 - detailH) / rowH;
  int sel = selIndex(), first = constrain(sel - rows / 2, 0, nOrd > rows ? nOrd - rows : 0);
  for (int r = 0; r < rows && first + r < nOrd; r++) {
    const Station &st = stations[order[first + r]];
    unsigned long age = (millis() - st.heard) / 1000;
    char ds[10] = "--", as[8]; if (sdist[order[first + r]] >= 0) geoFmtDist(ds, sizeof ds, sdist[order[first + r]]);
    geoFmtAge(as, sizeof as, age);
    bool isSel = (first + r == sel);
    uint16_t fg = isSel ? TFT_BLACK : (age > 3600 ? TFT_DARKGREY : TFT_WHITE), bg = isSel ? TFT_YELLOW : TFT_BLACK;
    D.fillRect(0, y0 + r * rowH, LIST_W - 1, rowH, bg);
    D.setTextColor(fg, bg); D.setCursor(2, y0 + r * rowH + 2);
    D.printf("%-9.9s%5s %4s", st.call, ds, as);
  }
  // detail of the selected station
  int dy = H - detailH; D.drawFastHLine(0, dy - 1, LIST_W - 1, TFT_DARKGREY);
  D.setTextColor(TFT_WHITE, TFT_BLACK);
  if (nOrd) {
    const Station &st = stations[order[sel]];
    D.setCursor(2, dy + 2); D.setTextColor(TFT_YELLOW, TFT_BLACK); D.printf("%-15.15s", st.call); drawSymbol(D, LIST_W - 17, dy + 10, st.table, st.sym);
    D.setTextColor(TFT_WHITE, TFT_BLACK);
    D.setCursor(2, dy + 14);
    if (gps.fix.valid) { double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[sel]]);
                         D.printf("%3.0f %-2s  %-9s", b, geoCompass(b), ds); }
    else D.printf("no GPS: no distance ");
    char as[8]; geoFmtAge(as, sizeof as, (millis() - st.heard) / 1000);
    D.setCursor(2, dy + 26); D.printf("rssi %.0f snr %+.1f", st.rssi, st.snr);
    D.setCursor(2, dy + 38); D.printf("%.4f %.4f", st.lat, st.lon);
  }
}

// Full-screen list of the heard stations (view 3, no map): a list on the left and everything known about the selected station on the right,
// including how its last packet reached us and the packet text itself.
static void drawFullList(lgfx::LovyanGFX &D, int W, int H) {
  D.fillScreen(TFT_BLACK);
  const int LW = 196, rowH = 26, top = 14, rows = (H - top - 12) / rowH;
  D.fillRect(0, 0, W, 13, C_BAR); D.setTextSize(1); D.setTextColor(TFT_CYAN); D.setCursor(4, 3);
  D.printf("FULL LIST   %d heard   %s first", nOrd, (sortByDistance && gps.fix.valid) ? "nearest" : "most recent");
  bool follow = lastKnobAt == 0 || millis() - lastKnobAt > FOLLOW_PAUSE_MS;
  const char *ft = follow ? "FOLLOWING NEW PACKETS" : "HOLDING (knob in use)";
  D.setTextColor(follow ? TFT_GREEN : TFT_YELLOW); D.setCursor(W - 6 * (int)strlen(ft) - 4 - (S.batIcon ? 36 : 0), 3); D.print(ft);
  drawClockBat(D, 222, 3, C_BAR, true); drawBatIcon(D, W);
  D.drawFastVLine(LW, top, H - top - 12, TFT_DARKGREY);
  int sel = selIndex(), first = constrain(sel - rows / 2, 0, std::max(0, nOrd - rows));
  if (nOrd == 0) { D.setTextColor(C_GRAY); D.setCursor(10, 60); D.print("No stations heard yet."); }
  for (int r = 0; r < rows && first + r < nOrd; r++) {
    const Station &st = stations[order[first + r]]; bool isSel = first + r == sel; int y = top + r * rowH;
    unsigned long age = (millis() - st.heard) / 1000;
    bool fresh = !strcmp(st.call, lastHeardCall) && millis() - lastHeardAt < FRESH_MS;                // heard a moment ago: lit up in green
    uint16_t bg = fresh ? 0x0460 : (isSel ? C_SEL : TFT_BLACK), fg = fresh ? TFT_WHITE : (isSel ? TFT_YELLOW : (age > 3600 ? C_GRAY : TFT_WHITE));
    D.fillRect(0, y, LW, rowH, bg);
    if (!drawSymbol(D, 14, y + 13, st.table, st.sym)) D.fillCircle(14, y + 13, 4, TFT_YELLOW);
    D.setTextSize(2); D.setTextColor(fg); D.setCursor(30, y + 1); D.print(st.call);
    char as[8]; geoFmtAge(as, sizeof as, age); if (fresh) strcpy(as, "NEW"); D.setTextSize(1); D.setTextColor(fresh ? TFT_GREEN : C_GRAY); D.setCursor(LW - 6 * (int)strlen(as) - 4, y + 4); D.print(as);
    if (isSel) D.drawRect(0, y, LW, rowH, TFT_YELLOW);                                                // the selected row keeps a yellow frame even when lit green
    char info[40] = "";
    if (gps.fix.valid) { char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[first + r]]); double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); snprintf(info, sizeof info, "%s %s  ", ds, geoCompass(b)); }
    D.setTextColor(st.via[0] ? TFT_YELLOW : TFT_GREEN); D.setCursor(30, y + 17);
    D.print(info); D.print(st.via[0] ? "digipeated" : "direct");
  }
  // detail of the selected station
  const int X = LW + 8; int y = 18;
  if (nOrd) {
    const Station &st = stations[order[sel]];
    if (!drawSymbol(D, X + 12, y + 9, st.table, st.sym)) D.fillCircle(X + 12, y + 9, 4, TFT_YELLOW);
    D.setTextSize(2); D.setTextColor(TFT_YELLOW); D.setCursor(X + 30, y + 2); D.print(st.call);
    if (!strcmp(st.call, lastHeardCall) && millis() - lastHeardAt < FRESH_MS) { D.setTextSize(1); D.setTextColor(TFT_GREEN); D.setCursor(X + 30 + 12 * (int)strlen(st.call) + 8, y + 9); D.print("JUST HEARD"); D.setTextSize(2); }
    y += 30; D.setTextSize(1);
    char a1[8], a2[8]; geoFmtAge(a1, sizeof a1, (millis() - st.heard) / 1000); geoFmtAge(a2, sizeof a2, (millis() - st.first) / 1000);
    D.setTextColor(TFT_WHITE); D.setCursor(X, y); D.printf("Heard %s ago   packet %u   first %s ago", a1, (unsigned)st.count, a2); y += 11;
    D.setCursor(X, y);
    if (st.via[0]) { D.setTextColor(TFT_YELLOW); D.printf("Digipeated by %s", st.via); } else { D.setTextColor(TFT_GREEN); D.print("Heard directly from the sender"); }
    y += 11; D.setTextColor(TFT_WHITE); D.setCursor(X, y); D.printf("Path: %s", st.path[0] ? st.path : "(none)"); y += 11;
    D.setCursor(X, y); D.printf("RSSI %.0f dBm    SNR %+.1f dB", st.rssi, st.snr); y += 11;
    D.setCursor(X, y); D.printf("Position %.4f %.4f", st.lat, st.lon); y += 11;
    if (gps.fix.valid) { double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[sel]]);
                         D.setCursor(X, y); D.printf("%s from you, %.0f deg %s", ds, b, geoCompass(b)); y += 11; }
    D.setCursor(X, y); D.printf("Symbol  table %c  code %c", st.table, st.sym); y += 11;
    if (st.comment[0]) { std::vector<std::string> w; wrapInto(w, std::string("Comment: ") + st.comment, 46); for (size_t i = 0; i < w.size() && i < 2; i++) { D.setCursor(X, y); D.print(w[i].c_str()); y += 11; } }
    y += 3; D.setTextColor(C_GRAY); D.setCursor(X, y); D.print("Packet:"); y += 11; D.setTextColor(0xC618);
    std::vector<std::string> raw; wrapInto(raw, st.raw, 46);
    for (size_t i = 0; i < raw.size() && y <= H - 22; i++) { D.setCursor(X, y); D.print(raw[i].c_str()); y += 10; }
  }
  D.fillRect(0, H - 12, W, 12, C_BAR); D.setTextSize(1); D.setCursor(4, H - 10);
  if (notice && millis() < noticeUntil) { D.setTextColor(TFT_GREEN); D.print(notice); }
  else { D.setTextColor(C_GRAY); D.print("turn: select  press: message  hold: show on map  S: sort  L: next view"); }
}

// ---- Main menu (Backspace on the map): knob turn = move, press = choose, long press or Backspace = close ----
enum MenuAct { MA_NONE, MA_MESSAGES, MA_SETUP, MA_BEACON, MA_AUTO, MA_POWEROFF_ASK, MA_POWEROFF_YES, MA_NO, MA_CLOSE };
struct MainMenu { bool on = false; int sel = 0, n = 0; char label[7][32]; MenuAct act[7]; char title[34] = "Menu"; };
static MainMenu mainMenu;
static void mmAdd(const char *t, MenuAct a) { if (mainMenu.n < 7) { strncpy(mainMenu.label[mainMenu.n], t, 31); mainMenu.label[mainMenu.n][31] = 0; mainMenu.act[mainMenu.n++] = a; } }
static void drawMainMenu(lgfx::LovyanGFX &D) {
  int h = 26 + mainMenu.n * 18, w = 260, x = (480 - w) / 2, y = (222 - h) / 2;
  D.fillRect(x - 2, y - 2, w + 4, h + 4, TFT_WHITE); D.fillRect(x, y, w, h, TFT_BLACK);
  D.setTextSize(1); D.setTextColor(TFT_CYAN); D.setCursor(x + 8, y + 7); D.print(mainMenu.title);
  for (int i = 0; i < mainMenu.n; i++) {
    int yy = y + 22 + i * 18; bool sel = i == mainMenu.sel;
    D.fillRect(x + 2, yy, w - 4, 16, sel ? TFT_YELLOW : TFT_BLACK);
    D.setTextColor(sel ? TFT_BLACK : TFT_WHITE); D.setCursor(x + 10, yy + 4); D.print(mainMenu.label[i]);
  }
}

static void render() {
  uint32_t t0 = millis(); misses = 0;
  lgfx::LovyanGFX &D = fbOk ? (lgfx::LovyanGFX &)fb : (lgfx::LovyanGFX &)tft;   // draw off-screen, then send in one go
  int z = ZOOMS[zi], W = tft.width(), H = tft.height(), nmax = 1 << z;
  int x0 = (stView == 0) ? LIST_W : 0;                // map area is x0 .. W-1
  int xc = x0 + (W - x0) / 2;                         // map centre on screen
  buildOrder();
  if (stView != 2 && nOrd && (selCall[0] == 0 || strcmp(stations[order[selIndex()]].call, selCall) != 0)) {
    strncpy(selCall, stations[order[0]].call, 9); selCall[9] = 0;     // nothing selected yet: take the first
  }
  if (stView == 3) { drawFullList(D, W, H); if (mainMenu.on) drawMainMenu(D); if (fbOk) fb.pushSprite(0, 0); return; }    // full list: no map at all
  double cx, cy; toPx(clat, clon, z, cx, cy);
  double ox = cx - xc, oy = cy - H / 2.0;
  int tx0 = (int)floor((ox + x0) / 256.0), tx1 = (int)floor((ox + W - 1) / 256.0);
  int ty0 = (int)floor(oy / 256.0), ty1 = (int)floor((oy + H - 1) / 256.0);
  for (int ty = ty0; ty <= ty1; ty++) for (int tx = tx0; tx <= tx1; tx++) {
    int dx = (int)floor(tx * 256.0 - ox), dy = (int)floor(ty * 256.0 - oy);
    lgfx::LGFX_Sprite *s = (tx < 0 || ty < 0 || tx >= nmax || ty >= nmax) ? nullptr : getTile(z, tx, ty);
    if (s) s->pushSprite(&D, dx, dy); else D.fillRect(dx, dy, 256, 256, 0x2104);   // dark grey = no tile here
  }
  uint32_t tTiles = millis() - t0;
  // station markers (not in the clean-map view). Stations too close to show separately are fanned out around the middle of the group:
  // each keeps a small dot at its true position with a thin line to its icon, and the labels are stacked so none covers another.
  D.setTextSize(1);
  // my own position (symbol with a ring, call sign in a blue label); it takes part in the fan-out so stations beside me ring around me
  int mx = 0, my = 0; bool hasMe = false;
  if (gps.fix.valid) {
    double sx, sy; toPx(gps.fix.lat, gps.fix.lon, z, sx, sy);
    mx = (int)(sx - ox); my = (int)(sy - oy);
    hasMe = mx > x0 - 8 && mx < W + 8 && my > -8 && my < H + 8;
  }
  int lx[49], ly[49], lw[49], nl = 0;                          // label boxes placed so far (left, top, width; all 11 px high), mine first
  int ownTx = 0, ownTy = 0;                                    // where my label's text starts
  if (hasMe) { int w = 6 * (int)strlen(S.call); ownTx = mx + 18; if (ownTx + w + 2 > W) ownTx = mx - 18 - w - 1; ownTy = my - 4; lx[0] = ownTx - 2; ly[0] = ownTy - 2; lw[0] = w + 3; nl = 1; }
  if (stView != 2) {
    struct Mk { const Station *st; int x, y, ix, iy; };
    Mk mk[48]; int nm = 0;
    for (auto &st : stations) {
      if (!st.used) continue;
      double sx, sy; toPx(st.lat, st.lon, z, sx, sy);
      int x = (int)(sx - ox), y = (int)(sy - oy);
      if (x < x0 - 60 || x > W + 60 || y < -10 || y > H + 10) continue;
      mk[nm++] = {&st, x, y, x, y};
    }
    std::sort(mk, mk + nm, [](const Mk &a, const Mk &b) { return strcmp(a.st->call, b.st->call) < 0; });   // stable order, so the fan does not shuffle
    bool grouped[48] = {};
    for (int i = 0; i < nm; i++) {
      if (grouped[i]) continue;
      int mem[48], n = 0;
      for (int j = i; j < nm; j++) if (!grouped[j] && abs(mk[j].x - mk[i].x) < 26 && abs(mk[j].y - mk[i].y) < 26) { mem[n++] = j; grouped[j] = true; }
      long cx = 0, cy = 0; for (int k = 0; k < n; k++) { cx += mk[mem[k]].x; cy += mk[mem[k]].y; } cx /= n; cy /= n;
      bool withMe = hasMe && abs((int)cx - mx) < 28 && abs((int)cy - my) < 28;                                // my own icon is in this group
      if (n < 2 && !withMe) continue;
      if (withMe) { cx = mx; cy = my; }                                                                        // I stay in the middle, the others ring around me
      float R = std::max(withMe ? 30.0f : 20.0f, 26.0f / (2.0f * sinf((float)M_PI / std::max(n, 2))) + 2.0f);   // far enough apart that 24 px icons do not touch
      for (int k = 0; k < n; k++) {
        float ang = -(float)M_PI / 2 + 2.0f * (float)M_PI * k / n;
        mk[mem[k]].ix = constrain((int)(cx + R * cosf(ang)), x0 + 14, W - 14); mk[mem[k]].iy = constrain((int)(cy + R * sinf(ang)), 14, H - 14);
      }
    }
    for (int i = 0; i < nm; i++) {                           // lines and true-position dots for the icons that were moved
      if (mk[i].ix == mk[i].x && mk[i].iy == mk[i].y) continue;
      D.drawLine(mk[i].x, mk[i].y, mk[i].ix, mk[i].iy, TFT_BLACK);
      D.fillCircle(mk[i].x, mk[i].y, 3, TFT_BLACK); D.fillCircle(mk[i].x, mk[i].y, 2, TFT_YELLOW);
    }
    bool hasIcon[48];
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nm; i++) {                // the selected station is drawn last, on top
      bool isSel = strcmp(mk[i].st->call, selCall) == 0;
      if (isSel != (pass == 1)) continue;
      bool icon = drawSymbol(D, mk[i].ix, mk[i].iy, mk[i].st->table, mk[i].st->sym);
      hasIcon[i] = icon;
      if (!icon) { D.fillCircle(mk[i].ix, mk[i].iy, 4, TFT_BLACK); D.fillCircle(mk[i].ix, mk[i].iy, 3, TFT_YELLOW); }      // no icon for this code: plain dot
      if (isSel) D.drawCircle(mk[i].ix, mk[i].iy, icon ? 15 : 8, TFT_WHITE);
    }
    for (int i = 0; i < nm; i++) {
      int w = 6 * (int)strlen(mk[i].st->call), off = hasIcon[i] ? 14 : 6, x = mk[i].ix + off, y = mk[i].iy - 4;   // x, y: where the text starts
      if (x + w + 2 > W) x = mk[i].ix - off - w - 1;                                    // no room on the right: put it on the left
      for (int tries = 0; tries < 8; tries++) {                                          // slide down until the box overlaps no other label
        bool hit = false;
        for (int k = 0; k < nl; k++) if (x - 2 < lx[k] + lw[k] && lx[k] < x - 2 + w + 3 && y - 2 < ly[k] + 11 && ly[k] < y - 2 + 11) { hit = true; break; }
        if (!hit) break;
        y += 11;
      }
      lx[nl] = x - 2; ly[nl] = y - 2; lw[nl] = w + 3; nl++;
      drawLabel(D, x, y, mk[i].st->call, TFT_BLACK, TFT_YELLOW);
    }
  }
  // my own marker: my symbol inside a blue ring (a blue dot when the symbol will not fit), and my call sign in a blue label
  if (hasMe) {
    D.drawCircle(mx, my, 15, TFT_WHITE); D.drawCircle(mx, my, 14, TFT_BLUE);
    if (!drawSymbol(D, mx, my, S.table, S.sym)) { D.fillCircle(mx, my, 6, TFT_WHITE); D.fillCircle(mx, my, 4, TFT_BLUE); }
    drawLabel(D, ownTx, ownTy, S.call, TFT_WHITE, TFT_BLUE);
  }
  // crosshair at the map centre
  D.drawFastHLine(xc - 6, H / 2, 13, TFT_RED); D.drawFastVLine(xc, H / 2 - 6, 13, TFT_RED);
  if (stView == 0) drawListPanel(D, H);
  // status at the top of the map, position at the bottom
  D.setTextColor(TFT_WHITE, TFT_BLACK); D.setTextSize(1);
  D.setCursor(x0 + 2, 2);
  if (notice && millis() < noticeUntil) D.printf(" %s                         ", notice);
  else {
    char tx[16] = "TX off";
    if (autoBeacon) { if (beacon.hasSent()) { char a[8]; geoFmtAge(a, sizeof a, beacon.lastSentAgeMs(millis()) / 1000); snprintf(tx, sizeof tx, "TX auto %s", a); } else snprintf(tx, sizeof tx, "TX auto"); }
    char um[12] = ""; if (M.unreadTotal()) snprintf(um, sizeof um, " MSG %d", M.unreadTotal());
    D.printf(" %s z%d %s %lums GPS %s %s%s ", VIEW_TAG[stView], z, MODE_NAME[mode], (unsigned long)tTiles, gps.fix.valid ? "fix" : "none", demoMode ? "DEMO MODE" : tx, demoMode ? "" : um);
  }
  D.setCursor(x0 + 2, H - 10);
  D.printf(" %.4f %.4f ", clat, clon);
  drawClockBat(D, W - 3, H - 10, TFT_BLACK, true, true);
  drawBatIcon(D, W);
  if (mainMenu.on) drawMainMenu(D);
  if (fbOk) fb.pushSprite(0, 0);
}

void setup() {
  Serial.begin(115200); delay(500);
  S = loadSettings();
  M.setCall(S.call); msgLoad(); M.restoreTimers(millis()); M.startIds(esp_random());
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  rtcRead();                                // the time the RTC chip kept, until GPS gives a better one
  bool xpOk = xp.begin(ADDR_XL9555);
  if (xpOk) {
    const uint8_t on[] = {XP_LORA_EN, XP_GPS_EN, XP_GPS_RST, XP_KB_EN, XP_KB_RST, XP_SD_EN, XP_GPIO_EN};
    for (uint8_t p : on) { xp.output(p, HIGH); delay(2); }
    xp.input(XP_SD_PULLEN);
  }
  for (uint8_t p : {PIN_LORA_CS, PIN_LORA_RST, PIN_SD_CS, PIN_NFC_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  tft.init(); tft.setRotation(PAGER_ROTATION); tft.setBrightness(S.brightness); tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.setTextSize(1);
  if (xpOk) { haptic.begin(xp); haptic.enabled = S.haptic; }
  fb.setPsram(true); fb.setColorDepth(16); fbOk = fb.createSprite(tft.width(), tft.height()) != nullptr;

  buf = (uint8_t *)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
  bool mounted = SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000);
  if (!buf || !mounted) { tft.setCursor(2, 2); tft.printf("Start-up: buffer %s, SD %s", buf ? "ok" : "FAIL", mounted ? "ok" : "FAIL"); return; }

  const char *cands[] = {"/LoRa_Tracker/Maps", "/maps"};
  String base;
  for (auto c : cands) if (SD.exists(c)) { base = c; break; }
  if (base == "") { tft.setCursor(2, 2); tft.print("No /LoRa_Tracker/Maps folder on the SD card"); return; }
  File r = SD.open(base); String region;
  while (true) { File e = r.openNextFile(); if (!e) break; if (e.isDirectory()) { region = e.name(); e.close(); break; } e.close(); }
  r.close();
  if (region == "") { tft.setCursor(2, 2); tft.print("No region folder under /LoRa_Tracker/Maps"); return; }
  root = base + "/" + region;

  // GPS on Serial1 (38400 baud), keyboard, and the radio (receive only)
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  keys.begin();
  digitalWrite(PIN_LORA_RST, LOW); delay(10); digitalWrite(PIN_LORA_RST, HIGH); delay(50);
  radio = new SX1262(new Module(PIN_LORA_CS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, SPI));
  if (radio->begin(433.775, 125.0, 12, 5, 0x12, 10, 8, 3.0) == RADIOLIB_ERR_NONE) {
    radio->setDio2AsRfSwitch(true); radio->setCurrentLimit(140.0); radio->setOutputPower(S.txDbm);
    radio->setPacketReceivedAction(onRx); radio->startReceive(); radioOk = true;
  }

  pinMode(PIN_ROT_A, INPUT_PULLUP); pinMode(PIN_ROT_B, INPUT_PULLUP); pinMode(PIN_ROT_PRESS, INPUT_PULLUP);
  kLast = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B);
  beacon.movingMs = (uint32_t)S.movingSec * 1000UL; beacon.stationaryMs = (uint32_t)S.stationaryMin * 60000UL; beacon.moveM = S.moveMetres;
  if (S.autoAtBoot) { autoBeacon = true; beacon.start(millis()); }
  attachInterrupt(PIN_ROT_A, knobIsr, CHANGE); attachInterrupt(PIN_ROT_B, knobIsr, CHANGE);
  render();
}

// Screenshot: send the off-screen frame buffer (exactly what is on the display) over USB serial.
// Format: "\nSHOT <w> <h>\n" + w*h*2 bytes (RGB565 as stored in the sprite) + "\nEND\n". Triggered by the P key or an 'S' byte from the Mac (tools/screenshot.py).
static void sendScreenshot() {
  if (!fbOk) return;
  const uint8_t *d = (const uint8_t *)fb.getBuffer();
  size_t n = (size_t)fb.width() * fb.height() * 2, off = 0;
  Serial.printf("\nSHOT %d %d\n", (int)fb.width(), (int)fb.height());
  uint32_t t0 = millis();
  while (off < n && millis() - t0 < 8000) {
    size_t w = Serial.write(d + off, min((size_t)512, n - off));
    off += w; if (!w) delay(1);
  }
  Serial.print("\nEND\n");
  haptic.play(H_TICK);
}

// ---- Settings kept in the pager's flash (NVS namespace "pgr-aprs"; not the SD card). Loaded values go through the same checks as the web form. ----
static void saveSettings(const PagerSettings &x) {
  Preferences p; p.begin("pgr-aprs", false);
  char t[2] = {x.table, 0}, y[2] = {x.sym, 0};
  p.putString("call", x.call); p.putString("path", x.path); p.putString("table", t); p.putString("sym", y); p.putString("comment", x.comment);
  p.putString("txdbm", String(x.txDbm)); p.putString("moving", String(x.movingSec)); p.putString("stationary", String(x.stationaryMin));
  p.putString("movem", String(x.moveMetres)); p.putString("auto", x.autoAtBoot ? "1" : "0"); p.putString("haptic", x.haptic ? "1" : "0");
  p.putString("bright", String(x.brightness)); p.putString("tz", String(x.utcOffset)); p.putString("dst", x.usDst ? "1" : "0"); p.putString("baticon", x.batIcon ? "1" : "0"); p.putString("dim", String(x.dimSec)); p.putString("off", String(x.offSec)); p.end();
}
static PagerSettings loadSettings() {
  PagerSettings x; Preferences p; p.begin("pgr-aprs", true);
  std::string err;
  bool ok = settingsParse([&](const char *k, std::string &v) {
    if (!p.isKey(k)) { if (!strcmp(k, "tz")) { v = "-8"; return true; } if (!strcmp(k, "dst") || !strcmp(k, "baticon")) { v = "1"; return true; } if (!strcmp(k, "dim")) { v = "60"; return true; } if (!strcmp(k, "off")) { v = "300"; return true; } return false; }   // settings saved by an older build: default clock
    v = p.getString(k, "").c_str(); return true; }, x, err);
  p.end();
  return ok ? x : PagerSettings();            // nothing saved yet, or damaged: the defaults
}
// ---- Screen timeout: dims, then the backlight goes off. No lock screen: any key or knob turn brings it back at once, showing what was there.
// When the backlight was fully off, that first touch only wakes the screen (a key pressed in a pocket does nothing); when merely dimmed it also acts.
static int screenState = 0;                               // 0 full, 1 dim, 2 off
static uint32_t lastActivity = 0;
static bool screenTouch() {                                // returns true when the screen was off: the caller then ignores that key press
  lastActivity = millis(); bool wasOff = screenState == 2;
  if (screenState) { screenState = 0; tft.setBrightness(S.brightness); dirty = true; }
  return wasOff;
}
static void screenTick() {
  if (!S.dimSec && !S.offSec) { if (screenState) { screenState = 0; tft.setBrightness(S.brightness); } return; }
  uint32_t idle = (millis() - lastActivity) / 1000;
  int want = (S.offSec && idle >= (uint32_t)S.offSec) ? 2 : ((S.dimSec && idle >= (uint32_t)S.dimSec) ? 1 : 0);
  if (want != screenState) { screenState = want; tft.setBrightness(want == 2 ? 0 : (want == 1 ? std::min(S.brightness, 20) : S.brightness)); }
}
static uint8_t keyFiltered() { uint8_t e = keys.poll(); if (e && screenTouch()) return 0; return e; }
static void applySettings() {
  screenState = 0; lastActivity = millis();
  tft.setBrightness(S.brightness);
  haptic.enabled = S.haptic; M.setCall(S.call);
  if (radioOk) radio->setOutputPower(S.txDbm);
  beacon.movingMs = (uint32_t)S.movingSec * 1000UL; beacon.stationaryMs = (uint32_t)S.stationaryMin * 60000UL; beacon.moveM = S.moveMetres;
}

// ---- Phone setup: the pager opens its own WiFi hotspot and serves the setup page (Enter key). Off again when you leave. ----
static WebServer web(80);
static DNSServer dns;     // answers every name with the pager's own address, so phones show the "sign in to network" prompt and open the page
static bool setupMode = false;
static char apSsid[16], apPass[10];
static uint32_t savedAt = 0;
// diagnostics shown on the setup screen: what the hotspot has seen so far
static volatile int evJoined = 0, evLeft = 0, evIp = 0, evReq = 0;
static char lastReq[28] = "";
static void noteReq() { evReq++; strncpy(lastReq, web.uri().c_str(), sizeof lastReq - 1); lastReq[sizeof lastReq - 1] = 0; }
extern const uint8_t sym0Start[] asm("_binary_src_web_sym0_png_start");
extern const uint8_t sym0End[] asm("_binary_src_web_sym0_png_end");
extern const uint8_t sym1Start[] asm("_binary_src_web_sym1_png_start");
extern const uint8_t sym1End[] asm("_binary_src_web_sym1_png_end");
static void webReply(int code, const char *type, const String &body) { web.sendHeader("Cache-Control", "no-store"); web.send(code, type, body); }
static void handleSave() {
  PagerSettings n; std::string err;
  bool ok = settingsParse([&](const char *k, std::string &v) { if (!web.hasArg(k)) return false; v = web.arg(k).c_str(); return true; }, n, err);
  if (!ok) { webReply(400, "application/json", String("{\"ok\":false,\"error\":\"") + settingsJsonEscape(err.c_str()).c_str() + "\"}"); return; }
  S = n; saveSettings(S); applySettings(); savedAt = millis();
  webReply(200, "application/json", "{\"ok\":true}");
}
static void startSetup() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "Pager-%02X%02X", mac[4], mac[5]);
  static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";             // no look-alike characters
  for (int i = 0; i < 8; i++) apPass[i] = alpha[esp_random() % (sizeof alpha - 1)];
  apPass[8] = 0;
  evJoined = evLeft = evIp = evReq = 0; lastReq[0] = 0;
  WiFi.onEvent([](WiFiEvent_t e) {
    if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED) evJoined++;
    else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) evLeft++;
    else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) evIp++;
  });
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPass);
  WiFi.setSleep(false);                                    // WiFi power saving makes phones drop or stall on a soft AP
  web.on("/", HTTP_GET, []() { noteReq(); web.sendHeader("Cache-Control", "no-store"); web.send_P(200, "text/html", SETUP_PAGE); });
  web.on("/settings.json", HTTP_GET, []() { noteReq(); webReply(200, "application/json", settingsToJson(S).c_str()); });
  web.on("/save", HTTP_POST, []() { noteReq(); handleSave(); });
  web.on("/sym0.png", HTTP_GET, []() { noteReq(); web.send_P(200, "image/png", (const char *)sym0Start, sym0End - sym0Start); });
  web.on("/sym1.png", HTTP_GET, []() { noteReq(); web.send_P(200, "image/png", (const char *)sym1Start, sym1End - sym1Start); });
  web.onNotFound([]() { noteReq(); web.sendHeader("Location", "http://192.168.4.1/", true); web.send(302, "text/plain", ""); });   // phones probe a few fixed addresses: send them to the page
  web.begin();
  dns.start(53, "*", WiFi.softAPIP());
  setupMode = true; savedAt = 0;
}
static void stopSetup() {
  dns.stop(); web.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  setupMode = false; dirty = true;
}
static void drawSetup() {
  lgfx::LovyanGFX &D = fbOk ? (lgfx::LovyanGFX &)fb : (lgfx::LovyanGFX &)tft;
  D.fillScreen(TFT_BLACK);
  D.fillRect(0, 0, 480, 15, C_BAR); D.setTextSize(1);
  D.setTextColor(TFT_CYAN); D.setCursor(4, 4); D.print("1  JOIN THE WIFI");
  D.setCursor(194, 4); D.print("PHONE SETUP");
  D.setCursor(312, 4); D.print("2  OPEN THE PAGE");
  // left: QR that joins the hotspot. right: QR that opens the setup page (works once the phone is on the hotspot; no pop-up needed)
  char pay[64]; snprintf(pay, sizeof pay, "WIFI:T:WPA;S:%s;P:%s;;", apSsid, apPass);
  D.fillRect(2, 18, 168, 168, TFT_WHITE);   D.qrcode(pay, 3, 19, 165, 4);                       // version 4 = 33 modules, 5 px each
  D.fillRect(310, 18, 168, 168, TFT_WHITE); D.qrcode("http://192.168.4.1", 319, 27, 150, 2);    // version 2 = 25 modules, 6 px each
  D.setTextColor(C_GRAY);
  D.setCursor(4, 192); D.print("Scan with the camera,"); D.setCursor(4, 203); D.print("then tap Join.");
  D.setCursor(312, 192); D.print("After joining, scan this"); D.setCursor(312, 203); D.print("to open the setup page.");
  // middle: the same things in words, for typing, and what the hotspot has seen
  const int x = 174;
  D.setTextColor(C_GRAY); D.setCursor(x, 22); D.print("WiFi name");
  D.setTextSize(2); D.setTextColor(TFT_YELLOW); D.setCursor(x, 32); D.print(apSsid);
  D.setTextSize(1); D.setTextColor(C_GRAY); D.setCursor(x, 58); D.print("Password");
  D.setTextSize(2); D.setTextColor(TFT_YELLOW); D.setCursor(x, 68); D.print(apPass);
  D.setTextSize(1); D.setTextColor(C_GRAY); D.setCursor(x, 94); D.print("Page address");
  D.setTextSize(2); D.setTextColor(TFT_YELLOW); D.setCursor(x, 104); D.print("192.168.4.1");
  D.setTextSize(1); D.setTextColor(TFT_WHITE); D.setCursor(x, 136); D.printf("Phones joined: %d", WiFi.softAPgetStationNum());
  D.setTextColor(C_GRAY);
  D.setCursor(x, 148); D.printf("joined %d  left %d", evJoined, evLeft);
  D.setCursor(x, 159); D.printf("IP given %d", evIp);
  D.setCursor(x, 170); D.printf("pages %d %.8s", evReq, lastReq);
  if (savedAt && millis() - savedAt < 6000) { D.setTextColor(TFT_GREEN); D.setCursor(x, 183); D.print("Settings saved"); }
  D.setTextColor(C_GRAY); D.setCursor(x, 203); D.print("Exit: Enter, Backspace");
  D.setCursor(x, 212); D.print("or a knob press");
  if (fbOk) fb.pushSprite(0, 0);
}
static void setupTick() {
  dns.processNextRequest();
  web.handleClient();
  noInterrupts(); knobSteps = 0; interrupts();
  static uint32_t lastDrawS = 0; static int lastClients = -1; static bool lastSaved = false; static bool wasPressed = false;
  bool saved = savedAt && millis() - savedAt < 6000;
  int clients = WiFi.softAPgetStationNum();
  static int lastEv = -1; int evSum = evJoined + evLeft * 7 + evIp * 31 + evReq * 101;
  if (millis() - lastDrawS > 1000 || clients != lastClients || saved != lastSaved || evSum != lastEv) { lastDrawS = millis(); lastClients = clients; lastSaved = saved; lastEv = evSum; drawSetup(); }
  bool exitNow = false;
  if (uint8_t e = keys.poll()) { if ((e & 0x80) && ((e & 0x7F) == KEY_ENTER || (e & 0x7F) == KEY_BACKSPACE)) exitNow = true; }
  bool pressed = digitalRead(PIN_ROT_PRESS) == LOW;
  if (pressed && !wasPressed) exitNow = true;
  wasPressed = pressed;
  if (exitNow) { haptic.play(H_CLICK); stopSetup(); }
}

// Put one frame on the air after listening first (channel activity detection, up to 3 tries with a random wait).
// Returns 0 = sent, 1 = channel busy (nothing sent), 2 = the radio failed. Blocks about 3 s at SF12.
static int txFrame(const unsigned char *f, size_t len) {
  if (demoMode) return 1;                              // demo mode never transmits
  bool free = false;
  for (int tries = 0; tries < 3 && !free; tries++) {
    int c = radio->scanChannel();
    if (c == RADIOLIB_CHANNEL_FREE) free = true; else delay(300 + (esp_random() % 1200));
  }
  if (!free) { radio->startReceive(); return 1; }
  int st = radio->transmit(f, len);
  rxFlag = false;                                   // the TX-done interrupt shares the receive pin
  radio->startReceive();
  M.noteTx(millis());
  return st == RADIOLIB_ERR_NONE ? 0 : 2;
}
// A message or ack from the engine: "CALL>TOCALL,PATH" + the engine's info part.
static int txEngineItem(const msg::TxItem &it) {
  std::string t = msg::tnc2(S.call, TOCALL, S.path, it.info);
  unsigned char f[260]; if (t.size() + 3 > sizeof f) return 2;
  f[0] = 0x3C; f[1] = 0xFF; f[2] = 0x01; memcpy(f + 3, t.data(), t.size());
  int r = txFrame(f, t.size() + 3);
  Serial.printf("tx %s (%u bytes): %s\n", r == 0 ? "sent" : (r == 1 ? "BUSY" : "FAILED"), (unsigned)(t.size() + 3), t.c_str());
  return r;
}

// Send one position beacon now. Listens first (channel activity detection, up to 3 tries with a random wait) and gives up if the
// channel stays busy. Returns true if it went out. Blocks about 3 s at SF12.
static bool sendBeacon() {
  if (demoMode) { say("Demo mode: nothing is sent"); return false; }
  if (!radioOk) { say("No radio"); return false; }
  if (strcmp(S.call, "N0CALL") == 0) { say("Set your call sign: press Enter"); haptic.play(H_ERROR); return false; }
  if (!gps.fix.valid) { say("No GPS fix: no beacon"); haptic.play(H_ERROR); return false; }
  unsigned char f[200];
  size_t len = aprsBuildBeacon(f, sizeof f, S.call, TOCALL, S.path, gps.fix.lat, gps.fix.lon, S.table, S.sym, S.comment);
  if (!len) { say("Beacon too long"); return false; }
  say("Sending beacon..."); if (!ui.active()) render();
  int r = txFrame(f, len);
  if (r == 1) { say("Channel busy: beacon skipped"); return false; }
  if (r != 0) { say("TX failed"); haptic.play(H_ERROR); return false; }
  txCount++; beacon.sent(millis(), gps.fix.lat, gps.fix.lon);
  haptic.play(H_MESSAGE); say("Beacon sent");
  return true;
}

// ---- Power off: the charger chip (BQ25896, I2C 0x6B) can disconnect the battery ("ship mode": register 09, bit 5, BATFET_DIS), which cuts all power.
// It does not work while USB is connected (the chip's own rule), and the pager comes back with the POWER button or by plugging in USB.
static int bqRead(uint8_t reg) {
  Wire.beginTransmission(ADDR_BQ25896); Wire.write(reg); if (Wire.endTransmission(false) != 0) return -1;
  Wire.requestFrom((uint8_t)ADDR_BQ25896, (uint8_t)1); return Wire.available() ? Wire.read() : -1;
}
static bool bqWrite(uint8_t reg, uint8_t v) { Wire.beginTransmission(ADDR_BQ25896); Wire.write(reg); Wire.write(v); return Wire.endTransmission() == 0; }
static bool usbPresent() {                              // register 0B bits 7..5: 0 = no input, 1..6 = a USB or adapter input, 7 = on-the-go (we supply)
  int v = bqRead(0x0B); if (v < 0) return true;         // cannot tell: act as if USB is there and refuse
  int st = (v >> 5) & 7; return st >= 1 && st <= 6;
}
static void powerOff() {
  if (usbPresent()) { say("Unplug USB first, then power off"); haptic.play(H_ERROR); return; }
  autoBeacon = false;                                   // tidy up first: nothing on the air, messages saved
  if (M.changed) { msgSave(); M.changed = false; }
  if (radioOk) radio->sleep();
  lgfx::LovyanGFX &D = fbOk ? (lgfx::LovyanGFX &)fb : (lgfx::LovyanGFX &)tft;
  D.fillScreen(TFT_BLACK); D.setTextSize(2); D.setTextColor(TFT_WHITE); D.setCursor(150, 98); D.print("Powering off...");
  if (fbOk) fb.pushSprite(0, 0);
  haptic.play(H_CLICK); delay(700);
  int v = bqRead(0x09);
  if (v >= 0) bqWrite(0x09, (uint8_t)(v | 0x20));      // battery path off: the pager loses power here
  delay(1500);
  if (v >= 0) bqWrite(0x09, (uint8_t)(v & ~0x20));     // still running: it did not work; put the battery path back
  if (radioOk) radio->startReceive();
  say("Power off did not work"); haptic.play(H_ERROR); dirty = true;
}
static void openMainMenu() {
  mainMenu.n = 0; mainMenu.sel = 0; mainMenu.on = true; strcpy(mainMenu.title, "Menu");
  mmAdd("Messages", MA_MESSAGES); mmAdd("Phone setup", MA_SETUP); mmAdd("Beacon now", MA_BEACON);
  mmAdd(autoBeacon ? "Auto-beacon is ON: turn off" : "Auto-beacon is OFF: turn on", MA_AUTO);
  mmAdd("Power off", MA_POWEROFF_ASK); mmAdd("Close menu", MA_CLOSE);
  dirty = true;
}
static void mainMenuAct(MenuAct a) {
  mainMenu.on = false; dirty = true;
  switch (a) {
    case MA_MESSAGES: ui.open(); break;
    case MA_SETUP: startSetup(); drawSetup(); break;
    case MA_BEACON: if (beacon.manualOk(millis())) sendBeacon(); else say("Wait: beacon just sent"); break;
    case MA_AUTO: autoBeacon = !autoBeacon; if (autoBeacon) beacon.start(millis()); say(autoBeacon ? "Auto beacon ON" : "Auto beacon off"); break;
    case MA_POWEROFF_ASK:
      mainMenu.n = 0; mainMenu.sel = 0; mainMenu.on = true; strcpy(mainMenu.title, "Power off the pager?");
      mmAdd("No, keep it on", MA_NO); mmAdd("Yes, power off", MA_POWEROFF_YES); break;      // No first: a stray press cannot switch it off
    case MA_POWEROFF_YES: powerOff(); break;
    default: break;
  }
}

static void centerOnGps() {
  if (!gps.fix.valid) { haptic.play(H_ERROR); say("No GPS fix yet"); return; }
  clat = gps.fix.lat; clon = gps.fix.lon; haptic.play(H_CLICK); dirty = true;
}

void loop() {
  if (root == "") { delay(1000); return; }

  // GPS characters
  while (Serial1.available()) {
    if (demoMode) { Serial1.read(); continue; }                // demo: the real GPS is ignored
    if (!gps.feed((char)Serial1.read())) continue;
    // only redraw when my dot would move at least a pixel at this zoom, or the fix appeared/vanished
    static bool lastValid = false; static double lastX = 0, lastY = 0;
    double gx = 0, gy = 0; if (gps.fix.valid) toPx(gps.fix.lat, gps.fix.lon, ZOOMS[zi], gx, gy);
    if (gps.fix.valid != lastValid || (gps.fix.valid && (fabs(gx - lastX) >= 1.0 || fabs(gy - lastY) >= 1.0))) {
      lastValid = gps.fix.valid; lastX = gx; lastY = gy; dirty = true;
    }
  }

  // 'S' from the Mac = send a screenshot
  // Test hooks from the Mac (used to take screenshots): "S" screenshot, "M" open Messages, "G<lat>,<lon>,<zoom>\n" put the map there,
  // "V<0-3>\n" choose the L view, "C<call>\n" select a heard station, "X\n" back to the map.
  {
    static char cmd[40]; static int cmdN = -1;
    while (Serial.available()) {
      int c = Serial.read();
      if (cmdN >= 0) {
        if (c != '\n' && c != '\r') { if (cmdN < (int)sizeof cmd - 1) cmd[cmdN++] = (char)c; continue; }
        cmd[cmdN] = 0; cmdN = -1;
        if (cmd[0] == 'G') {
          double la, lo; int z;
          if (sscanf(cmd + 1, "%lf,%lf,%d", &la, &lo, &z) == 3 && fabs(la) < 85 && fabs(lo) <= 180) {
            for (int i = 0; i < NZ; i++) if (ZOOMS[i] == z) zi = i;
            clat = la; clon = lo; dirty = true; Serial.printf("map: %.4f %.4f z%d\n", la, lo, ZOOMS[zi]);
          }
        } else if (cmd[0] == 'V') { int v = atoi(cmd + 1); if (v >= 0 && v <= 3) { stView = v; if (stView != 0 && mode == M_LIST) mode = M_PANX; dirty = true; } }
        else if (cmd[0] == 'C') { strncpy(selCall, cmd + 1, 9); selCall[9] = 0; lastKnobAt = millis(); dirty = true; }
        else if (cmd[0] == 'X') { ui.close(); mainMenu.on = false; dirty = true; }
        else if (cmd[0] == 'D') { double la, lo; if (sscanf(cmd + 1, "%lf,%lf", &la, &lo) == 2) { loadDemo(la, lo); clat = la; clon = lo; dirty = true; Serial.println("demo mode on: simulated stations, nothing is transmitted until RST"); } }
        else if (cmd[0] == 'R') { Serial.println("restarting"); delay(100); esp_restart(); }
        continue;
      }
      if (c == 'S') sendScreenshot();
      else if (c == 'M' && !ui.active() && !setupMode) { ui.open(); Serial.println("ui: opened by serial"); }
      else if (c == 'G' || c == 'V' || c == 'C' || c == 'X' || c == 'D' || c == 'R') { cmd[0] = (char)c; cmdN = 1; }
    }
  }

  // received packets
  if (rxFlag && radioOk) {
    rxFlag = false;
    String f; int st = radio->readData(f);
    if (st == RADIOLIB_ERR_NONE) {
      rxCount++;
      { char shown[200]; size_t n = std::min<size_t>(f.length(), sizeof shown - 1); for (size_t i = 0; i < n; i++) shown[i] = (f[i] >= 32 && f[i] < 127) ? f[i] : '.'; shown[n] = 0;
        Serial.printf("rx rssi=%.0f snr=%.1f (%u bytes): %s\n", radio->getRSSI(), radio->getSNR(), (unsigned)f.length(), shown); }
      AprsPos pos;
      if (aprsParse((const unsigned char *)f.c_str(), f.length(), pos)) {
        if (strcasecmp(pos.call, S.call) != 0) { stationUpdate(pos, radio->getRSSI(), radio->getSNR(), f.c_str() + ((uint8_t)f[0] == 0x3C ? 3 : 0), f.length() - ((uint8_t)f[0] == 0x3C ? 3 : 0));
          // full list: show the station that was just heard (unless the knob was used recently)
          if (stView == 3 && (lastKnobAt == 0 || millis() - lastKnobAt > FOLLOW_PAUSE_MS)) { strncpy(selCall, pos.call, 9); selCall[9] = 0; } dirty = true; haptic.play(H_ACK); }   // our own beacon coming back from a digipeater is not a station
      }
      else if (f.length() > 12 && (uint8_t)f[0] == 0x3C && (uint8_t)f[1] == 0xFF && (uint8_t)f[2] == 0x01) {
        msg::Parsed pm = msg::parse(f.c_str() + 3, f.length() - 3);
        msg::Event ev = M.onRx(pm, millis(), epochNow(), (int)radio->getRSSI(), (int)lroundf(radio->getSNR() * 4.0f));
        if (ev == msg::EV_NEW) { screenTouch(); haptic.play(H_MESSAGE); ui.onIncoming(pm.from); if (!ui.active()) { static char note[40]; snprintf(note, sizeof note, "Message from %s", pm.from); say(note); } dirty = true; }
        else if (ev == msg::EV_ACKED) { haptic.play(H_ACK); ui.dirty = true; }
        else if (ev == msg::EV_REJECTED) { haptic.play(H_ERROR); ui.dirty = true; }
      }
    }
    radio->startReceive();
  }

  if (setupMode) { setupTick(); return; }   // while the phone setup screen is open, nothing else runs (receiving still does)

  screenTick();
  { static uint32_t clkTickAt = 0; static int lastMin = -1, lastPct = -2; static bool lastChg = false;
    if (millis() - clkTickAt > 1000) {
      clkTickAt = millis(); clockTick();
      uint32_t u = utcNow(); int mn = u ? (int)(u / 60) : -1;
      if (mn != lastMin || batPct != lastPct || batCharging != lastChg) { lastMin = mn; lastPct = batPct; lastChg = batCharging; if (ui.active()) ui.dirty = true; else dirty = true; }
    } }

  // the keyboard chip can lose its setup (a radio transmission right next to it, a power glitch): check once a second and set it up again
  static uint32_t kbChecked = 0; static int kbResets = 0;
  if (millis() - kbChecked > 1000) {
    kbChecked = millis();
    if (!keys.configured()) { bool ok = keys.begin(); kbResets++; Serial.printf("keyboard: setup was lost or the chip did not answer, set up again (%s), %d time(s)\n", ok ? "ok" : "no answer", kbResets); }
  }

  // keyboard: C centres on my GPS position, L steps the station view (list + map / map only / hidden), S changes the list order
  if (uint8_t e = keyFiltered()) {
    Serial.printf("key raw=%u %s char='%c' ui=%d\n", e & 0x7F, (e & 0x80) ? "down" : "up", keyLookup(e & 0x7F).ch ? keyLookup(e & 0x7F).ch : '?', (int)ui.scr);
    if (ui.active()) { ui.key(e & 0x7F, e & 0x80); if (!ui.active()) dirty = true; }
    else if (mainMenu.on) {
      if (e & 0x80) { if ((e & 0x7F) == KEY_ENTER) mainMenuAct(mainMenu.act[mainMenu.sel]); else if ((e & 0x7F) == KEY_BACKSPACE) { mainMenu.on = false; dirty = true; } }
    }
    else if (e & 0x80) {
      char ch = keyLookup(e & 0x7F).ch;
      if ((e & 0x7F) == KEY_BACKSPACE) { haptic.play(H_CLICK); openMainMenu(); }
      else if (ch == 'm') { ui.open(); haptic.play(H_CLICK); }
      else if (e == 0x80 + KEY_ENTER) { haptic.play(H_CLICK); startSetup(); drawSetup(); }
      else if (ch == 'p') sendScreenshot();
      else if (ch == 't') { autoBeacon = !autoBeacon; if (autoBeacon) beacon.start(millis()); haptic.play(H_CLICK); say(autoBeacon ? "Auto beacon ON" : "Auto beacon off"); }
      else if (ch == 'b') { if (beacon.manualOk(millis())) sendBeacon(); else say("Wait: beacon just sent"); }
      else if (ch == 'c') centerOnGps();
      // L: next view; entering the full list starts on the latest station and follows new packets
      else if (ch == 'l') { stView = (stView + 1) % 4; if (stView == 3) { lastKnobAt = 0; if (lastHeardCall[0]) { strncpy(selCall, lastHeardCall, 9); selCall[9] = 0; } } if (stView != 0 && mode == M_LIST) mode = M_PANX; haptic.play(H_CLICK); say(VIEW_NAME[stView]); }
      else if (ch == 's') { sortByDistance = !sortByDistance; haptic.play(H_TICK); say(sortByDistance ? "list: nearest first" : "list: most recent first"); }
    }
  }

  // knob press: short = next knob mode, long (>= 600 ms) = centre on my GPS position
  static bool wasPressed = false; static uint32_t pressedAt = 0; static bool longDone = false;
  bool pressed = digitalRead(PIN_ROT_PRESS) == LOW;
  { static bool ignorePress = false;                       // a press that only woke the screen is ignored until the knob is let go
    if (pressed && !wasPressed && screenTouch()) ignorePress = true;
    if (ignorePress) { if (!pressed) ignorePress = false; pressed = false; } }
  if (pressed && !wasPressed) { pressedAt = millis(); longDone = false; }
  if (pressed && !longDone && millis() - pressedAt >= 600) { longDone = true; if (mainMenu.on) { mainMenu.on = false; dirty = true; haptic.play(H_CLICK); } else if (ui.active()) { ui.press(true); haptic.play(H_CLICK); if (!ui.active()) dirty = true; } else if (stView == 3) { stView = 0; haptic.play(H_CLICK); say(VIEW_NAME[0]); dirty = true; } else centerOnGps(); }
  if (!pressed && wasPressed && !longDone) { if (mainMenu.on) { haptic.play(H_CLICK); mainMenuAct(mainMenu.act[mainMenu.sel]); } else if (ui.active()) { ui.press(false); haptic.play(H_TICK); if (!ui.active()) dirty = true; } else if (stView == 3) { haptic.play(H_CLICK); ui.open(); ui.openCompose(selCall); } else { mode = nextMode(mode); haptic.play(H_TICK); dirty = true; } }
  wasPressed = pressed;

  int32_t d; noInterrupts(); d = knobSteps; knobSteps = 0; interrupts();
  if (d && screenTouch()) d = 0;
  if (d && mainMenu.on) { mainMenu.sel = constrain(mainMenu.sel + (d > 0 ? -1 : 1), 0, mainMenu.n - 1); haptic.play(H_TICK); dirty = true; }
  else if (d && ui.active()) { for (int i = 0, n = abs(d); i < n; i++) ui.knob(d > 0 ? 1 : -1); haptic.play(H_TICK); }
  else if (d) {
    haptic.play(H_TICK);
    if (stView == 3) {                               // full list: the knob moves the selection (up on the knob = up the list)
      lastKnobAt = millis();                         // and stops it following new packets for a while
      if (nOrd) {
        int i = constrain(selIndex() + (d > 0 ? -1 : 1), 0, nOrd - 1);
        const Station &st = stations[order[i]];
        strncpy(selCall, st.call, 9); selCall[9] = 0; clat = st.lat; clon = st.lon;
      }
    } else if (mode == M_ZOOM) {
      int n = constrain(zi + (d > 0 ? 1 : -1), 0, NZ - 1);
      if (n != zi) zi = n;
    } else if (mode == M_LIST) {
      if (nOrd) {                                    // up on the knob = up the list; the map follows the selection
        int i = constrain(selIndex() + (d > 0 ? -1 : 1), 0, nOrd - 1);
        const Station &st = stations[order[i]];
        strncpy(selCall, st.call, 9); selCall[9] = 0; clat = st.lat; clon = st.lon;
      }
    } else {
      int z = ZOOMS[zi]; double cx, cy; toPx(clat, clon, z, cx, cy);
      double step = 64.0 * d;
      if (mode == M_PANX) cx += step; else cy -= step;   // up on the knob = north
      fromPx(cx, cy, z, clat, clon);
    }
    dirty = true;
  }
  // auto beacon
  static uint32_t nextTry = 0;
  if (autoBeacon && (int32_t)(millis() - nextTry) >= 0 && beacon.due(millis(), gps.fix.valid, gps.fix.lat, gps.fix.lon)) { if (!sendBeacon()) nextTry = millis() + 10000; }

  // messages and acks waiting to go out
  if (radioOk && M.callOk()) {
    msg::TxItem it;
    if (M.nextTx(millis(), it)) {
      if (!ui.active()) { say(it.isAck ? "Sending ack..." : "Sending message..."); render(); }
      int r = txEngineItem(it); M.txDone(r == 0, millis());
      if (r == 2) haptic.play(H_ERROR); else if (r == 0 && !it.isAck) haptic.play(H_TICK);
      ui.dirty = true; dirty = true;
    }
  }
  // save the messages a few seconds after the last change (flash, not the SD card)
  static uint32_t chgAt = 0;
  if (M.changed) { if (!chgAt) chgAt = millis(); if (millis() - chgAt > 3000) { msgSave(); M.changed = false; chgAt = 0; } }

  // the messages screens replace the map while they are open
  if (ui.active()) {
    static uint32_t uiDrawn = 0;
    if (ui.dirty || millis() - uiDrawn > 1000) {
      uiDrawn = millis(); ui.draw(fbOk ? (lgfx::LovyanGFX &)fb : (lgfx::LovyanGFX &)tft); if (fbOk) fb.pushSprite(0, 0);
    }
    return;
  }

  // keep the "age" column current: refresh every 15 s while the list is showing
  static uint32_t lastAge = 0;
  if ((stView == 0 || stView == 3) && millis() - lastAge > 15000) { lastAge = millis(); dirty = true; }
  static uint32_t lastFade = 0;
  if (stView == 3 && millis() - lastHeardAt < FRESH_MS + 1500 && millis() - lastFade > 1000) { lastFade = millis(); dirty = true; }     // let the green highlight fade out
  // redraw at most every 300 ms (stations and GPS arrive in bursts)
  if (dirty && millis() - lastDraw > 300) { dirty = false; lastDraw = millis(); render(); }
  else if (notice && millis() > noticeUntil && !dirty) { notice = nullptr; dirty = true; }
}
