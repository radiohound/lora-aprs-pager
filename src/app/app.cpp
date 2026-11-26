// The application: LoRa APRS tracker, station map and messenger for the T-LoRa Pager (see README.md).
// Map from the T-Deck card layout (/LoRa_Tracker/Maps/<REGION>/<z>/<x>/<y>.jpg|png, read-only), heard stations, beacon, messaging, phone setup.
// Knob: turn = pan / zoom / select, press = switch what the knob does, long press = centre on me.
#include <Arduino.h>
#include <Wire.h>
#include "motion.h"
#include "esp_private/panic_internal.h"
#include "../rxduty.h"
static const int LORA_SF = 12; static const float LORA_BW_KHZ = 125.0f;   // the channel: LoRa APRS 433.775 MHz, SF12, 125 kHz, 8-symbol preamble
static void rxApply();
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
#include "ubx.h"
#include "ui_icons.h"
#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>
#include "chase.h"
#include "txbackoff.h"
#include "repeater.h"
#include "rsthist.h"
#include "gpsduty.h"
#include <esp_task_wdt.h>
#include <esp_system.h>

static XL9555 xp;
static PagerDisplay tft;
static Haptic haptic;
static Keys keys;
static lgfx::LGFX_Sprite fb(&tft);   // off-screen frame buffer (PSRAM) so redraws never show half-drawn
static bool fbOk = false;

// ---- LoRa APRS receive (433.775 MHz, SF12, BW125, CR4/5, private sync 0x12). It also transmits beacons and messages once a call sign is saved. ----
static SX1262 *radio = nullptr;
static TxBackoff txBack;                             // radio failures in a row: automatic sends wait longer, one buzz
// The SD card shares the SPI bus with the radio. With the card's power off and its chip-select (CS) not driven high, the radio stops working
// (shown by serial command PX: floating, pulled up or low all fail; high works). Driving CS high all the time costs about 14 mA (it leaks into
// the unpowered card), so it is driven only while the radio is being talked to, and floats the rest of the time.
static bool sdUnpowered = false;
// The pause after driving CS high matters: with none the radio failed. In the PX test 0, 20 and 100 us failed; 500 us, 2 ms and 10 ms passed
// every check and reset (one pager, indoors). 2 ms leaves a margin.
static const uint32_t SD_SETTLE_US = 2000;
static uint32_t sdSettleUs = SD_SETTLE_US;           // PX tries other values and puts this one back
static bool sdParkLines = false;                    // test only (PX): also hold SCK and MOSI low between radio exchanges
static uint32_t busSaved[2];                        // their pin routing, put back before each exchange
static void busPark(bool park) {
  const int pins[2] = {PIN_SCK, PIN_MOSI};
  for (int i = 0; i < 2; i++) {
    if (park) { busSaved[i] = GPIO.func_out_sel_cfg[pins[i]].val; ::pinMode(pins[i], OUTPUT); ::digitalWrite(pins[i], LOW); GPIO.func_out_sel_cfg[pins[i]].func_sel = SIG_GPIO_OUT_IDX; }
    else GPIO.func_out_sel_cfg[pins[i]].val = busSaved[i];
  }
}
class RadioBusHal : public ArduinoHal {
public:
  explicit RadioBusHal(SPIClass &spi) : ArduinoHal(spi) {}
  void spiBeginTransaction() override { if (sdParkLines) busPark(false); if (sdUnpowered) { ::pinMode(PIN_SD_CS, OUTPUT); ::digitalWrite(PIN_SD_CS, HIGH); if (sdSettleUs) delayMicroseconds(sdSettleUs); } ArduinoHal::spiBeginTransaction(); }
  void spiEndTransaction() override { ArduinoHal::spiEndTransaction(); if (sdUnpowered) ::pinMode(PIN_SD_CS, INPUT); if (sdParkLines) busPark(true); }
};
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
struct Station { char call[10]; double lat, lon; char table, sym; uint32_t heard, first, heardUtc, firstUtc; float rssi, snr; bool used;      // heard/first: millis(); heardUtc/firstUtc: real time (0 = clock not set)

                 uint16_t count; char raw[121], path[40], via[10], comment[44]; bool noPos; };   // noPos: heard (it repeated our packet) but we do not know where it is   // raw = the last packet as text; via = the digipeater that repeated it ("" = heard directly)
static Station stations[48];
static uint32_t utcNow();
// Wakes the receiver from backup: a low level on its receive line. A 0x00 byte holds the line low for 9 bit times, 0xFF for only 1. With 0xFF
// many wake-ups went unanswered and needed several retries; PW's GPS window log shows how many each search needs now.
static const uint8_t GPS_WAKE_BYTE = 0x00;
static void gpsSendWake() { for (int i = 0; i < 4; i++) Serial1.write(GPS_WAKE_BYTE); }
static void gpsHint();
static void plogInit();
static void rstHistBoot();
static void rstHistAlive();
static void rstHistPrint();
static bool radioSetup();
static bool plogRestoreAuto();                               // after a watchdog or crash restart: should auto beacon come back on?
static void blogBeacon(char tag, int result);                // the beacon log and the GPS window log (PW prints both)
static void blogGpsWindow(bool gotFix, uint32_t awakeMs);
static uint32_t motionWakesTotal();
static uint32_t motionPinWakesTotal();
static void gpsPosSave();                           // the clock, defined further down
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
  uint32_t u = utcNow();
  if (!same) { slot->count = 0; slot->first = millis(); slot->firstUtc = u; }
  slot->heardUtc = u;
  if (slot->count < 65535) slot->count++;
  strncpy(slot->call, p.call, 9); slot->call[9] = 0;
  slot->lat = p.lat; slot->lon = p.lon; slot->table = p.table; slot->sym = p.sym; slot->noPos = false;
  slot->heard = millis(); slot->rssi = rssi; slot->snr = snr; slot->used = true;
  strncpy(slot->comment, p.comment, sizeof slot->comment - 1); slot->comment[sizeof slot->comment - 1] = 0;
  size_t k = std::min(rawLen, sizeof slot->raw - 1);
  for (size_t i = 0; i < k; i++) slot->raw[i] = (rawText[i] >= 32 && rawText[i] < 127) ? rawText[i] : '.';
  slot->raw[k] = 0;
  aprsPathInfo(rawText, rawLen, slot->path, sizeof slot->path, slot->via, sizeof slot->via);
  strncpy(lastHeardCall, p.call, 9); lastHeardCall[9] = 0; lastHeardAt = millis();
}
// A station repeated one of our packets and we heard it: it goes on the list (or its entry is refreshed). The signal figures are its own.
// What we already know from its own packets stays; a new entry has no position until it sends one.
static void stationRepeatedUs(const char *call, float rssi, float snr, const char *rawText, size_t rawLen) {
  Station *slot = nullptr, *oldest = nullptr; bool same = false;
  for (auto &s : stations) {
    if (s.used && strcmp(s.call, call) == 0) { slot = &s; same = true; break; }
    if (!s.used && !slot) slot = &s;
    if (s.used && (!oldest || s.heard < oldest->heard)) oldest = &s;
  }
  if (!slot) slot = oldest;
  uint32_t u = utcNow();
  if (!same) {
    memset(slot, 0, sizeof *slot);
    strncpy(slot->call, call, 9); slot->table = '/'; slot->sym = '#'; slot->noPos = true; slot->first = millis(); slot->firstUtc = u; slot->used = true;
    strncpy(slot->comment, "Repeated our packet", sizeof slot->comment - 1);
    size_t k = std::min(rawLen, sizeof slot->raw - 1);
    for (size_t i = 0; i < k; i++) slot->raw[i] = (rawText[i] >= 32 && rawText[i] < 127) ? rawText[i] : '.';
    slot->raw[k] = 0;
    aprsPathInfo(rawText, rawLen, slot->path, sizeof slot->path, slot->via, sizeof slot->via); slot->via[0] = 0;   // what we heard was its own transmission
  }
  slot->heard = millis(); slot->heardUtc = u; slot->rssi = rssi; slot->snr = snr; if (slot->count < 65535) slot->count++;
  strncpy(lastHeardCall, call, 9); lastHeardCall[9] = 0; lastHeardAt = millis();
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
    { uint32_t u = utcNow(); st.heardUtc = u ? u - (uint32_t)d.ageS : 0; st.firstUtc = u ? st.heardUtc - 600 : 0; }
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
static uint32_t lastBeaconUtc = 0;                         // when we last transmitted a beacon (0 = none since power-on, or clock not set)
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
    uint8_t st; if (i2cRead(ADDR_BQ25896, 0x0B, &st, 1) == 0) { int cs = (st >> 3) & 3; batCharging = cs >= 1; }   // 1 = pre-charge, 2 = fast charge, 3 = charge done (still on USB: the bolt stays, as on a phone)
  }
}
// Battery shape for the top right corner: white outline, green fill for the charge (yellow below 30%, red below 15%), black for the empty part,
// a white lightning bolt on it while the charger is working or the battery is full on USB. Drawn only when switched on in the setup page.
static void drawBatIcon(lgfx::LovyanGFX &D, int W) {
  if (!S.batIcon || batPct < 0) return;
  const int w = 28, h = 12, x = W - w - 5, y = 2;
  uint16_t c = batPct > 30 ? TFT_GREEN : (batPct > 15 ? TFT_YELLOW : TFT_RED);
  D.drawRect(x, y, w, h, TFT_WHITE); D.fillRect(x + w, y + 3, 2, h - 6, TFT_WHITE);
  D.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
  int fw = (w - 4) * batPct / 100; if (fw < 1 && batPct > 0) fw = 1;
  D.fillRect(x + 2, y + 2, fw, h - 4, c);
  if (batCharging) {                                                     // white lightning bolt with a black edge, like a phone; 5 x 7 pixels in the middle of the body
    static const char *const BOLT[7] = {"...XX", "..XX.", ".XX..", "XXXXX", "..XX.", ".XX..", "XX..."};
    const int bx = x + w / 2 - 2, by = y + 3;
    for (int pass = 0; pass < 2; pass++)
      for (int r = 0; r < 7; r++) for (int k = 0; k < 5; k++) if (BOLT[r][k] == 'X') {
        if (pass == 0) { D.drawPixel(bx + k - 1, by + r, TFT_BLACK); D.drawPixel(bx + k + 1, by + r, TFT_BLACK); D.drawPixel(bx + k, by + r - 1, TFT_BLACK); D.drawPixel(bx + k, by + r + 1, TFT_BLACK); }
        else D.drawPixel(bx + k, by + r, TFT_WHITE);
      }
  }
}
// Status icons down the right edge of the map, under the battery (shapes: src/ui_icons.h). The map shows around them.
static const int ICON_COL_W = 40, ICON_COL_BOTTOM = 150;     // station labels keep out of this corner
static const uint32_t KNOB_ICON_MS = 5000;                   // the knob-mode icon shows this long after a press changes the mode
static uint32_t knobIconUntil = 0;                           // 0 = hidden
static const uint32_t RX_ICON_MS = 2000;                     // the antenna is purple this long after a packet was read
static uint32_t rxIconUntil = 0;                             // 0 = no recent packet
static void drawUiIcon(lgfx::LovyanGFX &D, const UiIcon &ic, int cx, int cy) {       // centred on cx, cy
  int x0 = cx - ic.w / 2, y0 = cy - ic.h / 2;
  for (int y = 0; y < ic.h; y++) for (int x = 0; x < ic.w; x++) {
    uint16_t c;
    switch (ic.rows[y][x]) {
      case 'K': c = TFT_BLACK; break;
      case 'W': c = TFT_WHITE; break;
      case 'G': c = D.color565(0, 220, 0); break;
      case 'A': c = D.color565(130, 130, 130); break;
      case 'Y': c = D.color565(255, 230, 0); break;
      case 'R': c = D.color565(255, 40, 40); break;
      case 'P': c = D.color565(200, 60, 255); break;
      default: continue;
    }
    D.drawPixel(x0 + x, y0 + y, c);
  }
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
static Chase chase;                          // what the map follows (me unless a station is chosen) and "keep screen on" (chase.h)
static const int LIST_W = 130;
// The four views of the L key, named: shown as a pop-up when you switch.
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
volatile int32_t knobSteps = 0;
volatile uint8_t kLast = 0; static volatile int8_t kAcc = 0;
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
    sdist[i] = (gps.fix.valid && !stations[i].noPos) ? geoDistKm(gps.fix.lat, gps.fix.lon, stations[i].lat, stations[i].lon) : -1.0;
    order[nOrd++] = i;
  }
  bool byDist = sortByDistance && gps.fix.valid;
  for (int i = 1; i < nOrd; i++) {                      // insertion sort, at most 48 entries
    int v = order[i], j = i - 1;
    while (j >= 0 && (byDist ? (sdist[order[j]] < 0 ? 1e12 : sdist[order[j]]) > (sdist[v] < 0 ? 1e12 : sdist[v]) : stations[order[j]].heard < stations[v].heard)) { order[j + 1] = order[j]; j--; }
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
    if (gps.fix.valid && !st.noPos) { double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[sel]]);
                         D.printf("%3.0f %-2s  %-9s", b, geoCompass(b), ds); }
    else D.printf(st.noPos ? "position not known  " : "no GPS: no distance ");
    char as[8]; geoFmtAge(as, sizeof as, (millis() - st.heard) / 1000);
    D.setCursor(2, dy + 26); D.printf("rssi %.0f snr %+.1f", st.rssi, st.snr);
    D.setCursor(2, dy + 38); if (st.noPos) D.print("repeated our packet"); else D.printf("%.4f %.4f", st.lat, st.lon);
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
  const char *ft = follow ? "Auto-showing each new packet" : "Paused: you turned the knob";   // the list jumps to each newly heard station until the knob is used, then waits 20 s
  D.setTextColor(follow ? TFT_GREEN : TFT_YELLOW); D.setCursor(W - 6 * (int)strlen(ft) - 4 - (S.batIcon ? 36 : 0), 3); D.print(ft);
  drawClockBat(D, 226, 3, C_BAR, false); drawBatIcon(D, W);        // time only here: the date would run into the status text
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
    if (gps.fix.valid && !st.noPos) { char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[first + r]]); double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); snprintf(info, sizeof info, "%s %s  ", ds, geoCompass(b)); }
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
    D.setTextColor(TFT_WHITE); D.setCursor(X, y); if (st.heardUtc) { char h[24], f[24]; clockStamp(h, sizeof h, st.heardUtc, utcNow(), S.utcOffset, S.usDst); clockStamp(f, sizeof f, st.firstUtc, utcNow(), S.utcOffset, S.usDst); D.printf("Heard %s (%s ago)  #%u  first %s", h, a1, (unsigned)st.count, f); }
    else D.printf("Heard %s ago   packet %u   first %s ago", a1, (unsigned)st.count, a2); y += 11;
    D.setCursor(X, y);
    if (st.via[0]) { D.setTextColor(TFT_YELLOW); D.printf("Digipeated by %s", st.via); } else { D.setTextColor(TFT_GREEN); D.print("Heard directly from the sender"); }
    y += 11; D.setTextColor(TFT_WHITE); D.setCursor(X, y); D.printf("Path: %s", st.path[0] ? st.path : "(none)"); y += 11;
    D.setCursor(X, y); D.printf("RSSI %.0f dBm    SNR %+.1f dB", st.rssi, st.snr); y += 11;
    D.setCursor(X, y); if (st.noPos) D.print("Position not known yet"); else D.printf("Position %.4f %.4f", st.lat, st.lon); y += 11;
    if (gps.fix.valid && !st.noPos) { double b = geoBearing(gps.fix.lat, gps.fix.lon, st.lat, st.lon); char ds[10]; geoFmtDist(ds, sizeof ds, sdist[order[sel]]);
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
enum MenuAct { MA_NONE, MA_MESSAGES, MA_SETUP, MA_BEACON, MA_AUTO, MA_POWEROFF_ASK, MA_POWEROFF_YES, MA_NO, MA_CLOSE, MA_HELP, MA_FOLLOW_ME, MA_FOLLOW_STATION, MA_KEEP_ASK, MA_KEEP_YES, MA_KEEP_OFF };
struct MainMenu { bool on = false; int sel = 0, n = 0; char label[9][32]; MenuAct act[9]; char title[34] = "Menu"; char info1[56] = "", info2[56] = "", info3[56] = ""; };
static MainMenu mainMenu;
static RstHist rstHist;                                          // restart history, kept in flash (rsthist.h)
static void drawRestarts(lgfx::LovyanGFX &D) {
  D.setTextColor(TFT_WHITE); D.setCursor(8, 28); D.printf("Starts: %u    crashes: %u    (newest first)", (unsigned)rstHist.starts, (unsigned)rstHist.crashes);
  for (int i = 0; i < rstHist.count; i++) {
    const RstRec &e = rstGet(rstHist, i); char when[20] = "time not set";
    if (e.utc) { LocalTime t = clockLocal(e.utc, S.utcOffset, S.usDst); snprintf(when, sizeof when, "%02d/%02d %02d:%02d", t.month, t.day, t.hour, t.min); }
    D.setTextColor(rstIsCrash(e.reason) ? TFT_RED : (e.reason == 9 ? TFT_YELLOW : TFT_WHITE));
    D.setCursor(8, 44 + i * 15); D.printf("%-12s  %s", when, rstName(e.reason));
    D.setTextColor(0xC618); D.setCursor(290, 44 + i * 15);
    if (i < rstHist.count - 1 || rstHist.starts > 1) D.printf("ran %s%uh %02um", rstIsCrash(e.reason) ? "" : "at least ", e.ranMin / 60, e.ranMin % 60);
    if (rstIsCrash(e.reason)) D.printf("  stage %u", e.stage);
  }
}
static bool helpOn = false; static int helpPage = 0;       // Help screen: page 0 = keys, page 1 = QR code for the SD card guide
static const char *HELP_URL = "https://github.com/radiohound/lora-aprs-pager";
static void drawHelp(lgfx::LovyanGFX &D) {
  D.fillRect(0, 0, 480, 222, 0x0014); D.drawFastHLine(0, 20, 480, TFT_CYAN);
  D.setTextSize(1); D.setTextColor(TFT_CYAN); D.setCursor(6, 6); D.print(helpPage == 0 ? "HELP  1: keys" : helpPage == 1 ? "HELP  2: building the map SD card" : "HELP  3: restart history");
  D.setTextColor(0xC618); D.setCursor(336, 6); D.print("turn=page press=close");
  D.setTextColor(TFT_WHITE);
  if (helpPage == 2) drawRestarts(D);
  else if (helpPage == 0) {
    static const char *L[] = {
      "Backspace   menu: follow, keep screen on, setup, power off",
      "M           messages          Enter   phone setup",
      "L           next view: list + map, map, map only, full list",
      "S           list order: nearest first or newest first",
      "C           centre the map on me",
      "B           beacon now        T   auto-beacon on/off",
      "Knob turn   pan left-right, pan up-down, zoom, or move the list",
      "Knob press  next knob mode (an icon shows which)",
      "Knob hold   centre the map on me",
      "Screen off: the first key press only wakes it.",
      "Map tiles are only read from the SD card, never written."};
    for (int i = 0; i < 11; i++) { D.setCursor(8, 30 + i * 16); D.print(L[i]); }
  } else {
    D.fillRect(10, 30, 186, 186, TFT_WHITE);                // 29 modules x 6 px plus a quiet border
    D.qrcode(HELP_URL, 16, 36, 174, 3);
    D.setTextColor(TFT_WHITE);
    D.setCursor(230, 40);  D.print("Scan to open the project page.");
    D.setCursor(230, 62);  D.print("README > SD card tells how to");
    D.setCursor(230, 74);  D.print("make the map tiles:");
    D.setCursor(230, 96);  D.print("/LoRa_Tracker/Maps/<REGION>/");
    D.setCursor(230, 108); D.print("      <z>/<x>/<y>.png or .jpg");
    D.setCursor(230, 130); D.print("256 px tiles, FAT32 card.");
    D.setCursor(230, 142); D.print("T-Deck LoRa APRS tile packs");
    D.setCursor(230, 154); D.print("work as they are.");
    D.setTextColor(0xC618); D.setCursor(230, 184); D.print("github.com/radiohound/");
    D.setCursor(230, 196); D.print("lora-aprs-pager");
  }
}
static void mmAdd(const char *t, MenuAct a) { if (mainMenu.n < 9) { strncpy(mainMenu.label[mainMenu.n], t, 31); mainMenu.label[mainMenu.n][31] = 0; mainMenu.act[mainMenu.n++] = a; } }
static void drawMainMenu(lgfx::LovyanGFX &D) {
  bool info = mainMenu.info1[0] != 0; int top = info ? (mainMenu.info3[0] ? 58 : 48) : 22;
  int h = top + 4 + mainMenu.n * 17, w = info ? 340 : 260, x = (480 - w) / 2, y = (222 - h) / 2;
  D.fillRect(x - 2, y - 2, w + 4, h + 4, TFT_WHITE); D.fillRect(x, y, w, h, TFT_BLACK);
  D.fillRect(x, y, w, top - 3, 0x0014);                          // the heading sits on a dark blue band, with a line under it before the choices
  D.fillRect(x, y + top - 3, w, 2, TFT_CYAN);
  D.setTextSize(1); D.setTextColor(TFT_CYAN); D.setCursor(x + 8, y + 6); D.print(mainMenu.title);
  if (info) { D.setTextColor(TFT_WHITE); D.setCursor(x + 8, y + 19); D.print(mainMenu.info1); D.setCursor(x + 8, y + 31); D.print(mainMenu.info2);
                if (mainMenu.info3[0]) { D.setTextColor(0xC618); D.setCursor(x + 8, y + 43); D.print(mainMenu.info3); } }
  for (int i = 0; i < mainMenu.n; i++) {
    int yy = y + top + i * 17; bool sel = i == mainMenu.sel;
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
  if (stView == 3) { drawFullList(D, W, H); if (mainMenu.on) drawMainMenu(D); if (helpOn) drawHelp(D); if (fbOk) fb.pushSprite(0, 0); return; }    // full list: no map at all
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
      if (!st.used || st.noPos) continue;
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
      if (!chase.followingMe() && strcmp(mk[i].st->call, chase.call) == 0) { D.drawCircle(mk[i].ix, mk[i].iy, 17, TFT_RED); D.drawCircle(mk[i].ix, mk[i].iy, 18, TFT_RED); }   // the one being chased
    }
    for (int i = 0; i < nm; i++) {
      int w = 6 * (int)strlen(mk[i].st->call), off = hasIcon[i] ? 14 : 6, x = mk[i].ix + off, y = mk[i].iy - 4;   // x, y: where the text starts
      if (x + w + 2 > (y < (chase.keepScreen ? ICON_COL_BOTTOM + 28 : ICON_COL_BOTTOM) ? W - ICON_COL_W : W)) x = mk[i].ix - off - w - 1;                                    // no room on the right: put it on the left
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
  if (notice && millis() < noticeUntil) { D.setCursor(x0 + 2, 2); D.printf(" %s ", notice); }     // the top row is only used for passing notices
  else if (demoMode) { D.setCursor(x0 + 2, 2); D.print(" DEMO MODE "); }
  else if (!chase.followingMe()) {                          // following me is normal and needs no words
    char t[72];
    if (!chase.have) snprintf(t, sizeof t, "FOLLOW %s  no position yet", chase.call);
    else {
      char age[8]; geoFmtAge(age, sizeof age, (millis() - chase.heard) / 1000);
      if (gps.fix.valid) { char ds[10]; geoFmtDist(ds, sizeof ds, geoDistKm(gps.fix.lat, gps.fix.lon, chase.lat, chase.lon)); double b = geoBearing(gps.fix.lat, gps.fix.lon, chase.lat, chase.lon);
                           snprintf(t, sizeof t, "FOLLOW %s  %s %.0f %s  heard %s", chase.call, ds, b, geoCompass(b), age); }
      else snprintf(t, sizeof t, "FOLLOW %s  heard %s  (no GPS fix)", chase.call, age);
    }
    D.setCursor(x0 + 2, 2); D.printf(" %s%s ", t, chase.paused ? "  PAUSED" : "");
  }
  if (chase.keepScreen) drawUiIcon(D, UI_LOCK, W - ICON_COL_W / 2, 164);
  {
    int ix = W - ICON_COL_W / 2;
    drawUiIcon(D, gps.fix.valid ? UI_GPS_FIX : UI_GPS_NONE, ix, 33);
    drawUiIcon(D, rxIconUntil ? UI_RX_NOW : (autoBeacon && !demoMode) ? UI_TX_AUTO : UI_TX_OFF, ix, 65);
    if (knobIconUntil)                                                      // what the knob does now
      drawUiIcon(D, mode == M_ZOOM ? UI_ZOOM : mode == M_PANX ? UI_PAN_X : mode == M_PANY ? UI_PAN_Y : UI_STATIONS, ix, 97);
    if (M.unreadTotal() && !demoMode) {                                     // unread messages: envelope and count
      drawUiIcon(D, UI_MAIL, ix, 128);
      D.setTextColor(TFT_WHITE, TFT_BLACK); D.setTextSize(1); char um[8]; snprintf(um, sizeof um, "%d", (int)M.unreadTotal());
      D.setCursor(ix - 3 * (int)strlen(um), 139); D.print(um);
    }
  }
  D.setCursor(x0 + 2, H - 10);
  D.printf(" %.4f %.4f ", clat, clon);
  if (lastBeaconUtc) { LocalTime bt = clockLocal(lastBeaconUtc, S.utcOffset, S.usDst); D.printf("TX %02d:%02d ", bt.hour, bt.min); }
  drawClockBat(D, W - 3, H - 10, TFT_BLACK, true, true);
  drawBatIcon(D, W);
  if (mainMenu.on) drawMainMenu(D);
  if (helpOn) drawHelp(D);
  if (fbOk) fb.pushSprite(0, 0);
}

// Reset the radio chip, set it up and start receiving.
static bool radioSetup() {
  digitalWrite(PIN_LORA_RST, LOW); delay(10); digitalWrite(PIN_LORA_RST, HIGH); delay(50);
  if (radio->begin(433.775, LORA_BW_KHZ, LORA_SF, 5, 0x12, 10, 8, 3.0) != RADIOLIB_ERR_NONE) return false;
  radio->setDio2AsRfSwitch(true); radio->setCurrentLimit(140.0); radio->setOutputPower(S.txDbm);
  radio->setPacketReceivedAction(onRx); radio->startReceive();
  return true;
}

void setup() {
  Serial.begin(115200); delay(500);
  plogInit();
  esp_task_wdt_deinit(); esp_task_wdt_init(30, true); esp_task_wdt_add(NULL);     // if the main loop stops for 30 s the pager resets itself (and the log survives)
  S = loadSettings();
  M.setCall(S.call); msgLoad(); M.restoreTimers(millis()); M.startIds(esp_random());
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  rtcRead();                                // the time the RTC chip kept, until GPS gives a better one
  rstHistBoot();                            // after the clock, so the record has the time
  bool xpOk = xp.begin(ADDR_XL9555);
  if (xpOk) {
    const uint8_t on[] = {XP_LORA_EN, XP_GPS_EN, XP_GPS_RST, XP_KB_EN, XP_KB_RST, XP_SD_EN};   // header rail (XP_GPIO_EN) stays off: nothing uses it
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
  gpsSendWake();   // wake the GPS if a restart caught it in backup mode (harmless if it is awake)
  delay(300);
  gpsHint();
  keys.begin();
  radio = new SX1262(new Module(new RadioBusHal(SPI), PIN_LORA_CS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY));
  if (radioSetup()) radioOk = true;

  pinMode(PIN_ROT_A, INPUT_PULLUP); pinMode(PIN_ROT_B, INPUT_PULLUP); pinMode(PIN_ROT_PRESS, INPUT_PULLUP);
  kLast = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B);
  beacon.movingMs = (uint32_t)S.movingSec * 1000UL; beacon.stationaryMs = (uint32_t)S.stationaryMin * 60000UL; beacon.moveM = S.moveMetres;
  if (S.autoAtBoot) { autoBeacon = true; beacon.start(millis()); }
  else if (plogRestoreAuto()) { autoBeacon = true; beacon.start(millis()); }
  attachInterrupt(PIN_ROT_A, knobIsr, CHANGE); attachInterrupt(PIN_ROT_B, knobIsr, CHANGE);
  if (rstIsCrash((int)esp_reset_reason())) say("Restarted after a crash: see Help, page 3");
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
  p.putString("bright", String(x.brightness)); p.putString("tz", String(x.utcOffset)); p.putString("dst", x.usDst ? "1" : "0"); p.putString("baticon", x.batIcon ? "1" : "0"); p.putString("dim", String(x.dimSec)); p.putString("off", String(x.offSec));
  p.putString("gpssl", x.gpsSleep ? "1" : "0"); p.putString("gpsmv", String(x.gpsMoveSec)); p.putString("gpsst", String(x.gpsStillMin)); p.putString("gpsmo", x.gpsMotion ? "1" : "0"); p.putString("gpsbk", String(x.gpsBackstopMin)); p.putString("slow40", x.slow40 ? "1" : "0"); p.putString("rxd", x.rxDuty ? "1" : "0"); p.end();
}
static PagerSettings loadSettings() {
  PagerSettings x; Preferences p; p.begin("pgr-aprs", true);
  std::string err;
  bool ok = settingsParse([&](const char *k, std::string &v) {
    if (!p.isKey(k)) return settingsLaterDefault(k, v);              // saved by an older version: the default for settings added later, else the load fails
    v = p.getString(k, "").c_str(); return true; }, x, err);
  p.end();
  return ok ? x : PagerSettings();            // nothing saved yet, or damaged: the defaults
}
// ---- Screen timeout: dims, then the backlight goes off. No lock screen: any key or knob turn brings it back at once, showing what was there.
// When the backlight was fully off, that first touch only wakes the screen (a key pressed in a pocket does nothing); when merely dimmed it also acts.
static int screenState = 0;                               // 0 full, 1 dim, 2 off
static uint32_t lastActivity = 0;
static long asleepSum = 0; static int asleepN = 0, asleepAvg = 0; static bool asleepHave = false; static int asleepMin = 0, asleepMax = -32768, asleepUsbSkipped = 0; static bool usbPresent();   // battery current sampled while the backlight was off
// Power items, switched while the screen is off. The P serial command turns each off or on to measure what it saves.
static bool setupMode = false;
static bool pwrPanel = true, pwrSd = true;                  // true = this saving is active
static bool pwrGps = true, pwrSleep = true;                 // pwrSleep: light sleep between events while the screen is off
static bool gpsEverFix = false;                                      // any GPS fix since start-up
static uint32_t nmeaOk[2] = {0, 0}, nmeaBad[2] = {0, 0};           // GPS lines read, [0] at 80 MHz or more, [1] below (PW prints them)
static uint32_t gpsWinBytes = 0;                                       // characters the GPS has sent during this wake-up (for the GPS window log)
static uint32_t lastGpsByte = 0;                                  // when the GPS last sent a character (light sleep waits until it has been quiet a moment)
// Position and time hint: after a power-up the receiver starts with an empty head. Tell it roughly where it is (the last saved fix) and the time (from the RTC), and it finds the sky faster.
static void ubxSend(const uint8_t *f, size_t n) { Serial1.write(f, n); }
// Development check: ask the receiver whether it took each hint (it answers only after CFG-NAVSPG-ACKAIDING is switched on) and keep
// the answers for PW. Waits up to about 1.5 s per message at start-up, three tries each. Set to 0 to just send the hints, as before.
#define GPS_HINT_CHECK 1
#if GPS_HINT_CHECK
struct HintRes { int tries = 0; int type = -1, info = -1; uint32_t ms = 0; };      // type: -1 no answer, 0 not used, 1 accepted
static HintRes hintPos, hintTime; static int hintAckOn = -1; static bool hintHavePos = false, hintHaveTime = false;
// Send one message and read the receiver's output until the wanted answer arrives or 1.5 s pass.
template <class F> static bool gpsAsk(const uint8_t *f, size_t n, F wanted) {
  UbxScan sc; ubxSend(f, n); uint32_t t0 = millis();
  while (millis() - t0 < 1500) { while (Serial1.available()) if (sc.feed((uint8_t)Serial1.read()) && wanted(sc)) return true; delay(2); }
  return false;
}
static void gpsAskHint(const uint8_t *f, size_t n, uint8_t msgType, HintRes &r) {
  uint32_t t0 = millis();
  for (r.tries = 1; r.tries <= 3; r.tries++) {
    if (gpsAsk(f, n, [&](const UbxScan &s) { if (!ubxIsMgaAck(s, msgType)) return false; r.type = s.pl[0]; r.info = s.pl[2]; return true; }) && r.type == 1) break;
    esp_task_wdt_reset();
  }
  if (r.tries > 3) r.tries = 3;
  r.ms = millis() - t0;
}
static void gpsHintLine(const char *name, bool have, const HintRes &r) {
  static const char *const WHY[7] = {"accepted", "it does not know the time yet", "message version not supported", "wrong message size", "could not store it", "not ready", "unknown message type"};
  if (!have) Serial.printf("GPS hint %s: not sent (nothing to send)\n", name);
  else if (r.type < 0) Serial.printf("GPS hint %s: NO ANSWER after %d tries (%lu ms)\n", name, r.tries, (unsigned long)r.ms);
  else Serial.printf("GPS hint %s: %s (code %d: %s), try %d, %lu ms\n", name, r.type == 1 ? "ACCEPTED" : "NOT USED", r.info, (r.info >= 0 && r.info < 7) ? WHY[r.info] : "?", r.tries, (unsigned long)r.ms);
}
static void gpsHintReport() {
  Serial.printf("GPS acknowledgments: %s\n", hintAckOn == 1 ? "switched on" : (hintAckOn == 0 ? "REFUSED" : "no answer"));
  gpsHintLine("position", hintHavePos, hintPos); gpsHintLine("time", hintHaveTime, hintTime);
}
#endif
static void gpsHint() {
  if (demoMode) return;
  Preferences p; p.begin("pgr-aprs", true); bool have = p.isKey("hlat") && p.isKey("hlon"); int32_t la = p.getInt("hlat", 0), lo = p.getInt("hlon", 0); p.end();
  uint8_t f[40];
#if GPS_HINT_CHECK
  { size_t n = ubxAckAidingOn(f); for (int k = 0; k < 3 && hintAckOn != 1; k++) gpsAsk(f, n, [](const UbxScan &s) { int a = ubxValsetAnswer(s); if (a < 0) return false; hintAckOn = a; return true; }); esp_task_wdt_reset(); }
  hintHavePos = have;
  if (have) gpsAskHint(f, ubxMgaPos(la, lo, 10000000UL, f), 0x01, hintPos);   // the saved fix, told to be good to 100 km so a long trip since then does no harm
  uint32_t t = utcNow(); hintHaveTime = t > 1700000000UL;
  if (hintHaveTime) gpsAskHint(f, ubxMgaTime(t = utcNow(), 5, f), 0x10, hintTime);    // the RTC time, good to a few seconds
  gpsHintReport();
#else
  if (have) ubxSend(f, ubxMgaPos(la, lo, 10000000UL, f));   // the saved fix, told to be good to 100 km so a long trip since then does no harm
  uint32_t t = utcNow();
  if (t > 1700000000UL) ubxSend(f, ubxMgaTime(t, 5, f));    // the RTC time, good to a few seconds
#endif
}
static void gpsPosSave() {                                  // remember where we are, but rarely: after a move of 3 km or more, never more often than every 10 minutes
  static double sLat = 999, sLon = 999; static uint32_t at = 0;
  if (demoMode || !gps.fix.valid || gps.fix.sats < 4) return;
  if (at && millis() - at < 600000UL) return;
  if (sLat < 900 && geoDistKm(sLat, sLon, gps.fix.lat, gps.fix.lon) < 3.0) return;
  Preferences p; p.begin("pgr-aprs", false); p.putInt("hlat", (int32_t)lround(gps.fix.lat * 1e7)); p.putInt("hlon", (int32_t)lround(gps.fix.lon * 1e7)); p.end();
  sLat = gps.fix.lat; sLon = gps.fix.lon; at = millis();
}
// A small event log kept in memory (serial command PW prints it): what the power code did and when. It survives as long as the app keeps running,
// so after a pager that "would not wake" is plugged in, the log shows what happened while it was asleep.
struct PEv { uint32_t ms; char tag; int a, b; };
// The log lives in RTC memory, which survives a watchdog or software reset (not a power loss or the RST button), so after the pager has reset itself
// the log still shows what it was doing just before. Each start adds an 'R' entry with the reset reason (1 power-on, 2 RST button, 3 software, 4 crash,
// 5 interrupt watchdog, 6 task watchdog, 9 brown-out).
struct BEv { uint32_t ms, utc; char tag; uint8_t a; uint16_t b; uint32_t c; uint16_t d, e; };
static const int BLOG_N = 128, GLOG_N = 96;
struct GInfo { uint8_t mot[4]; uint16_t movedM, nextS; };     // beside each GPS window entry: motion reports since the previous one (tilt, significant,
                                                             // steps, motion detect), metres from the previous fix (65535 = nothing to compare), sleep chosen
struct PRst { uint32_t bootMs, upMs; int reason, stage; };                       // upMs: how long the run that ended had been up; stage: the code stage it was in
struct PLogStore { uint32_t magic; int n; PEv buf[96]; uint32_t stage, stageMs, beats; int pins; PRst rst[8]; int rstN; uint32_t gpsWakes; uint32_t panics, pPc, pA0, pSp, pCore, pExc, pMs, pStack[8]; uint32_t cadTimeouts, cadMs, cadIrq; int cadStart, cadBusy, cadReinit, autoOn, autoRestores; BEv blog[BLOG_N], glog[GLOG_N]; int bN, gN; uint32_t bSent, bBusy, bFail, bNoFix, gFix, gNoFix; GInfo ginfo[GLOG_N]; };   // stage: where the code was when the last marker was written
static RTC_NOINIT_ATTR PLogStore plogRtc;
// What the chip was doing when a watchdog or crash reset hit: the panic handler is wrapped (link option -Wl,--wrap=esp_panic_handler in platformio.ini) so the
// interrupted program counter, return address, stack pointer and a few code addresses found on the stack go into RTC memory before the normal handler runs.
// Serial command PW prints them; look the addresses up with the toolchain's addr2line against the matching firmware.elf.
extern "C" void __real_esp_panic_handler(panic_info_t *info);
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
  if (info && plogRtc.magic == 0x504C4F4D) {
    const uint32_t *f = (const uint32_t *)info->frame;
    plogRtc.panics++; plogRtc.pCore = info->core; plogRtc.pExc = (uint32_t)info->exception; plogRtc.pMs = millis();
    plogRtc.pPc = (uint32_t)(uintptr_t)info->addr;
    for (int i = 0; i < 8; i++) plogRtc.pStack[i] = 0;
    if (f) {
      plogRtc.pA0 = f[3]; plogRtc.pSp = f[4];
      if (f[4] >= 0x3FC80000u && f[4] < 0x3FD00000u) {                        // stack in internal RAM: collect up to 8 words that look like code addresses
        const uint32_t *w = (const uint32_t *)f[4]; int k = 0;
        for (int i = 0; i < 160 && k < 8; i++) { uint32_t v = w[i]; if ((v >= 0x42000000u && v < 0x44000000u) || (v >= 0x40370000u && v < 0x403E0000u)) plogRtc.pStack[k++] = v; }
      }
    }
  }
  __real_esp_panic_handler(info);
}
#define plogBuf plogRtc.buf
#define plogN plogRtc.n
static inline void pstage(uint32_t st) { plogRtc.stage = st; plogRtc.stageMs = millis(); }   // 1 arming wake sources, 2 about to sleep, 3 woke, 4 pins restored, 5 loop pass started, 6 received packets, 7 screen/GPS duty, 8 keyboard check, 9 auto beacon, 10 message queue (19-24 inside a transmission: 19 beacon start, 20 listening, 21 listened, 22 transmitting, 23 sent, 24 done), 11 message screens, 12 redraw
static bool plogSleepOpen = false; static uint32_t plogTimerWakes = 0;
static void plog(char tag, int a = 0, int b = 0) { PEv &e = plogBuf[plogN % 96]; e.ms = millis(); e.tag = tag; e.a = a; e.b = b; plogN++; plogSleepOpen = (tag == 's'); }
static uint32_t bootPrevStage = 0, bootPrevMs = 0;          // from RTC memory, for the restart history: valid after a crash restart
// Every start adds a record to flash. How long the run before lasted: after a crash from RTC memory; otherwise from the note this run
// leaves in flash after 1 minute and then every 10 (so "at least").
static void rstHistBoot() {
  Preferences p; p.begin("pgr-rst", false);
  if (p.getBytes("hist", &rstHist, sizeof rstHist) != sizeof rstHist || !rstValid(rstHist)) rstClear(rstHist);
  int why = (int)esp_reset_reason(); uint32_t ran = p.getUShort("ran", 0);
  if (rstIsCrash(why) && bootPrevMs / 60000UL > ran) ran = bootPrevMs / 60000UL;
  rstAdd(rstHist, utcNow(), why, rstIsCrash(why) ? (int)bootPrevStage : 0, ran);
  p.putBytes("hist", &rstHist, sizeof rstHist); p.putUShort("ran", 0); p.end();
}
static void rstHistAlive() {                                  // every pass
  static uint32_t next = 60000UL;
  if (millis() < next) return;
  Preferences p; p.begin("pgr-rst", false); p.putUShort("ran", (uint16_t)std::min<uint32_t>(millis() / 60000UL, 65535)); p.end();
  next = millis() + 600000UL;
}
static void rstHistPrint() {
  Serial.printf("restart history (flash): %u starts, %u crashes\n", (unsigned)rstHist.starts, (unsigned)rstHist.crashes);
  for (int i = 0; i < rstHist.count; i++) { const RstRec &e = rstGet(rstHist, i); LocalTime c = clockBreak(e.utc);
    Serial.printf("  %02d/%02d %02d:%02d UTC  reason %u %s, stage %u, run before %u min\n", c.month, c.day, c.hour, c.min, e.reason, rstName(e.reason), e.stage, e.ranMin); }
}
static void plogInit() {                                    // at start-up: keep the log if the RTC memory still holds one, then note why we restarted
  if (plogRtc.magic != 0x504C4F4D || plogRtc.n < 0) { memset(&plogRtc, 0, sizeof plogRtc); plogRtc.magic = 0x504C4F4D; }
  bootPrevStage = plogRtc.stage; bootPrevMs = plogRtc.stageMs;
  { PRst &r = plogRtc.rst[plogRtc.rstN % 8]; r.reason = (int)esp_reset_reason(); r.stage = (int)plogRtc.stage; r.upMs = plogRtc.stageMs; r.bootMs = 0; plogRtc.rstN++; }   // never overwritten by ordinary entries
  plog('R', (int)esp_reset_reason(), (int)plogRtc.stage);
}
// Beacon log: 'B' a beacon was tried (a: 0 sent, 1 channel busy, 2 radio failed; b: satellites), 'N' one was due or asked for with no GPS fix
// (a: 1 = by auto beacon), 'M' a movement woke the GPS from its long sleep (a: 1 tilt, 2 significant motion, 4 step, 8 motion detect, 16 any motion, added), 'F' the first fix since this start (b: satellites). GPS log, one entry per wake window while the screen is off:
// 'W' (a: 1 = fresh fix, 0 = gave up; b: seconds awake; third number: characters received from the GPS, 0 = it sent nothing; fourth: extra wake-ups needed; fifth: ms until data arrived, 0 = none seen). Kept apart from the event log so its routine entries cannot push these out.
static void blogPut(BEv *ring, int size, int &n, char tag, int a, int b) { BEv &e = ring[n % size]; e.ms = millis(); e.utc = utcNow(); e.tag = tag; e.a = (uint8_t)a; e.b = (uint16_t)b; e.c = tag == 'W' ? gpsWinBytes : 0; e.d = 0; e.e = 0; n++; }
static void blogBeacon(char tag, int a) {
  if (tag == 'B') { if (a == 0) plogRtc.bSent++; else if (a == 1) plogRtc.bBusy++; else plogRtc.bFail++; }
  else if (tag == 'N') plogRtc.bNoFix++;
  blogPut(plogRtc.blog, BLOG_N, plogRtc.bN, tag, a, gps.fix.sats);
}
static void gpsWinInfo(int moved, uint32_t nextMs);          // fills in the GInfo beside the newest GPS window entry
static void gpsWinLogWake(struct BEv &e);                  // fills in this wake-up's retry count and time to first data
static void blogGpsWindow(bool gotFix, uint32_t awakeMs) {
  if (gotFix) plogRtc.gFix++; else plogRtc.gNoFix++;
  blogPut(plogRtc.glog, GLOG_N, plogRtc.gN, 'W', gotFix, (int)(awakeMs / 1000));
  gpsWinLogWake(plogRtc.glog[(plogRtc.gN - 1) % GLOG_N]);
}
static void blogPrint(const char *name, const BEv *ring, int size, int n) {
  int k = n < size ? n : size;
  Serial.printf("%s: %d entries, %d shown\n", name, n, k);
  for (int i = n - k; i < n; i++) { const BEv &e = ring[i % size]; LocalTime c = clockBreak(e.utc); Serial.printf("  %9lu ms  %02d:%02d:%02d UTC  %c  %d %d %lu\n", (unsigned long)e.ms, c.hour, c.min, c.sec, e.tag, e.a, e.b, (unsigned long)e.c); if (e.tag == 'W') { const GInfo &g = plogRtc.ginfo[i % size]; Serial.printf("    rewakes %u, first data after %u ms; motion since the last window: tilt %u, significant %u, steps %u, detect %u (255 = that many or more); moved ", e.d, e.e, g.mot[0], g.mot[1], g.mot[2], g.mot[3]);
      if (g.movedM == 65535) Serial.print("?"); else Serial.printf("%u m", g.movedM); Serial.printf("; then sleeps %u s\n", g.nextS); } esp_task_wdt_reset(); }
}
// Auto beacon comes back after a crash (4) or watchdog (5, 6, 7) restart if it was on. At most AUTO_RESTORE_MAX times in a row, so a pager stuck
// in a crash loop cannot keep transmitting; the count clears after 30 minutes without a restart. Power-on, RST and "restart" never restore it.
static const int AUTO_RESTORE_MAX = 3;
static bool plogRestoreAuto() {
  int why = (int)esp_reset_reason();
  if (why < 4 || why > 7) { plogRtc.autoOn = 0; plogRtc.autoRestores = 0; return false; }
  if (plogRtc.autoOn != 1 || plogRtc.autoRestores >= AUTO_RESTORE_MAX) return false;
  plogRtc.autoRestores++; plog('A', why, plogRtc.autoRestores);
  return true;
}
static bool lightSlept = false;                              // the processor light-slept while the screen was off: the display's bus needs a full re-init on wake
static int pinBits() { return digitalRead(PIN_LORA_IRQ) | (digitalRead(PIN_KB_INT) << 1) | (digitalRead(PIN_ROT_PRESS) << 2) | (digitalRead(PIN_ROT_A) << 3) | (digitalRead(PIN_ROT_B) << 4) | (digitalRead(PIN_GPS_RX) << 5); }
static bool aSd = false, aPanel = false, aGps = false;     // what is applied right now
// GPS while the screen is off: it sleeps (backup mode, about 0.03 mA) and wakes every minute for a fix, then sleeps again. It stays powered, so it keeps
// its satellite data and wakes hot. While the screen is on it runs all the time. Wake-up is any byte sent to it.
static const uint32_t GPS_ON_MIN_MS = 6000;      // stay awake at least this long for a fresh fix; when to give up: gpsAwakeLimit() in gpsduty.h
static bool gpsSleeping = false; static uint32_t gpsWakeAt = 0, gpsOnAt = 0, gpsEpochAtOn = 0, gpsIntervalMs = 60000;
static int gpsMisses = 0;                                              // wake-ups in a row without a fresh fix
static bool gpsAwaiting = false; static uint32_t gpsSentAt = 0;        // woken, no answer yet; when the wake bytes last went out
static int gpsRewakes = 0; static uint32_t gpsFirstDataMs = 0;         // this wake-up: retries needed, and ms until data arrived (0 = none)
static GpsAdaptive gpsAd;                                              // adaptive wake interval, see src/gpsduty.h (settings: moving seconds, still minutes)
static void gpsWinInfo(int moved, uint32_t nextMs) {
  static uint32_t seen[4] = {0, 0, 0, 0};
  GInfo &g = plogRtc.ginfo[(plogRtc.gN - 1) % GLOG_N];
  for (int k = 0; k < 4; k++) { uint32_t d = motionCount[k] - seen[k]; seen[k] = motionCount[k]; g.mot[k] = (uint8_t)std::min<uint32_t>(d, 255); }
  g.movedM = moved < 0 ? 65535 : (uint16_t)moved; g.nextS = (uint16_t)std::min<uint32_t>(nextMs / 1000, 65535);
}
static void gpsWinLogWake(BEv &e) { e.d = (uint16_t)gpsRewakes; e.e = (uint16_t)std::min<uint32_t>(gpsFirstDataMs, 65535); }
static void gpsBackup(bool gotFix) {
  blogGpsWindow(gotFix, millis() - gpsOnAt);
  gpsAwaiting = false;
  int moved = -1;                                              // metres from the fix the last window compared against
  if (gotFix && gps.fix.valid && gpsAd.prevLat < 900) moved = (int)std::min(geoDistKm(gpsAd.prevLat, gpsAd.prevLon, gps.fix.lat, gps.fix.lon) * 1000.0, 65000.0);
  gpsIntervalMs = gpsAdaptiveNext(gpsAd, gotFix && gps.fix.valid, gps.fix.lat, gps.fix.lon, S.gpsMoveSec, S.gpsStillMin);
  if (gotFix) gpsMisses = 0;
  else { if (gpsMisses < 100) gpsMisses++; gpsIntervalMs = gpsMissSleep(gpsEverFix, gpsMisses, S.gpsStillMin); }
  gpsWinInfo(moved, gpsIntervalMs);
  uint8_t f[24]; ubxSend(f, ubxBackup(f)); Serial1.flush(); delay(20);
  // With the GPS in backup its serial line floats. The processor's serial port then sees noise and every restart so far was an interrupt-watchdog reset inside light sleep
  // about 12 to 14 s after this point (suspected cause, not proven). So: close the port, hold both lines idle-high, reopen at wake-up.
  Serial1.end(); pinMode(PIN_GPS_RX, INPUT_PULLUP); pinMode(PIN_GPS_TX, OUTPUT); digitalWrite(PIN_GPS_TX, HIGH);
  gpsSleeping = true; gpsWakeAt = millis() + gpsIntervalMs; plog('b', (int)(gpsIntervalMs / 1000), gpsAd.still);
}
static void gpsWake() {
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  gpsSendWake();
  gpsSleeping = false; gpsOnAt = millis(); gpsEpochAtOn = gps.fix.epoch; gpsWinBytes = 0; plog('g');
  gpsAwaiting = true; gpsSentAt = gpsOnAt; gpsRewakes = 0; gpsFirstDataMs = 0;
}
static void gpsWakeCheck() {                                // every pass, screen on or off: did the receiver hear the wake-up? (gpsduty.h)
  if (!gpsAwaiting || gpsSleeping) return;
  uint32_t now = millis();
  if (gpsWinBytes >= GPS_ANSWER_BYTES) { gpsAwaiting = false; gpsFirstDataMs = std::max<uint32_t>(now - gpsOnAt, 1); return; }
  GpsRewake r = gpsRewakeStep(now - gpsSentAt, gpsWinBytes, gpsRewakes);
  if (r == GPS_WAIT) return;
  if (r == GPS_REOPEN_AND_SEND) { Serial1.end(); delay(5); Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX); }
  gpsSendWake();
  gpsRewakes++; gpsSentAt = now;
}
// Motion wake (setting "gpsmo"). While the sensor is being proven, the 5 minute check while still stays as it is, so a movement the sensor
// misses costs no more than before; the long backstop setting is not used yet.
static uint32_t motionGpsWakes = 0, motionPinWakes = 0;
static uint32_t motionWakesTotal() { return motionGpsWakes; }
static uint32_t motionPinWakesTotal() { return motionPinWakes; }
static void motionDuty() {                                  // every pass
  static bool tried = false;
  if (!S.gpsMotion) { gpsAd.motionAware = false; return; }
  if (!motionOk) { if (!tried) { tried = true; motionBegin(); } if (!motionOk) return; }
  gpsAd.motionAware = true;
  motionPoll();
  if (!motionNew) return;
  uint8_t kinds = motionNew; motionNew = 0;
  bool wake = gpsAdaptiveOnMotion(gpsAd, millis());              // once per report: it also puts the short interval back
  if (wake && aGps && gpsSleeping) {                             // only cuts the long sleep short, at most once a minute
    motionGpsWakes++; blogBeacon('M', kinds);
    gpsMisses = 0; gpsWakeAt = millis();
  }
}
static void gpsDuty() {                                     // called every pass while the screen is off
  if (!aGps) return;
  uint32_t now = millis();
  if (gpsSleeping) { if ((int32_t)(now - gpsWakeAt) >= 0) gpsWake(); return; }
  bool fresh = gps.fix.valid && gps.fix.epoch != gpsEpochAtOn;        // a fix from this wake-up, not the one from before
  if (fresh && now - gpsOnAt > GPS_ON_MIN_MS) gpsBackup(true);
  else if (now - gpsOnAt > gpsAwakeLimit(gpsEverFix, gpsMisses)) gpsBackup(false);
}
// What the screen being off switches off. The SD card's chip-select is floated too: driven high into an unpowered card it leaked about 14 mA.
// The panel's sleep command saves about 13 mA.
static uint32_t panelRetryUntil = 0, panelRetryAt = 0;      // after a wake, the panel's sleep-out is repeated for a few seconds (see below)
// Receive mode: continuous, or (screen off, setting on, and the channel's preamble long enough) in short bursts. Restarts reception either way.
static void rxApply() {
  if (!radioOk) return;
  RxDutyPlan p = rxDutyPlan(LORA_SF, LORA_BW_KHZ, RXD_SENDER_PREAMBLE, RXD_MIN_SYMBOLS, 5000);
  if (S.rxDuty && screenState == 2 && !setupMode && p.usable) radio->startReceiveDutyCycle(p.wakeUs, p.sleepUs); else radio->startReceive();
}
static void powerApply() {
  static bool rxWasDuty = false; bool rxWantDuty = S.rxDuty && screenState == 2 && !setupMode;
  if (rxWantDuty != rxWasDuty) { rxWasDuty = rxWantDuty; rxApply(); }                       // the screen went off or on: switch the receive mode
  bool off = screenState == 2 && !setupMode;
  bool wSd = off && pwrSd, wPanel = off && pwrPanel, wGps = off && pwrGps && S.gpsSleep && !demoMode;
  if (wSd != aSd || wPanel != aPanel || wGps != aGps) plog(off ? 'O' : 'o', wSd | (wPanel << 1) | (wGps << 2));
  if (wGps != aGps) { if (wGps) { gpsSleeping = false; gpsOnAt = millis(); gpsEpochAtOn = gps.fix.epoch; gpsMisses = 0; gpsWinBytes = 0; gpsAwaiting = true; gpsSentAt = millis(); gpsRewakes = 0; gpsFirstDataMs = 0; gpsAdaptiveReset(gpsAd, gps.fix.valid, gps.fix.lat, gps.fix.lon); } else if (gpsSleeping) gpsWake(); aGps = wGps; }
  // Waking: the SD card first, then the panel. After light sleep the panel's first sleep-out command, sent straight away, did not take; the same command
  // sent later (after the SD card was back) did. Going to sleep: panel first, then the SD card.
  if (!wSd && aSd) {
    sdUnpowered = false; xp.output(XP_SD_EN, HIGH); pinMode(PIN_SD_CS, OUTPUT); digitalWrite(PIN_SD_CS, HIGH); delay(10);
    bool ok = SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000); plog('d', ok);
    aSd = false;
  }
  if (wPanel != aPanel) {
    if (wPanel) tft.sleep();
    else {
      tft.wakeup(); delay(130);                                           // sleep out; the panel needs 120 ms before it takes more commands
      tft.startWrite(); tft.writeCommand(0x29); tft.endWrite();           // display on
      ledcSetup(7, 12000, 8); ledcAttachPin(PIN_DISP_BL, 7);               // the backlight's PWM channel is set up again: it can stop in light sleep
      panelRetryUntil = millis() + 3000; panelRetryAt = millis();         // and screenTick repeats the sleep-out for a few seconds, in case the first did not take
    }
    aPanel = wPanel; plog('p', wPanel, (int)(ESP.getFreeHeap() / 1024));
  }
  if (wSd && !aSd) { SD.end(); xp.output(XP_SD_EN, LOW); pinMode(PIN_SD_CS, INPUT); sdUnpowered = true; aSd = true; }
}
static bool usbPresent();
// One place that changes the processor speed. The GPS serial port's baud rate is set again afterwards, as its clock may have changed.
static void cpuSet(int mhz) {
  if ((int)getCpuFrequencyMhz() == mhz) return;
  setCpuFrequencyMhz(mhz);
  if (!gpsSleeping) Serial1.updateBaudRate(GPS_BAUD);
}
static bool screenTouch() {
  if (screenState == 2 && asleepN >= 2) { asleepAvg = (int)(asleepSum / asleepN); asleepHave = true; }                                // returns true when the screen was off: the caller then ignores that key press
  lastActivity = millis(); bool wasOff = screenState == 2; plog('t', wasOff, screenState);
  if (getCpuFrequencyMhz() < 80) cpuSet(80);                 // back to full bus speed before the card and the display are woken
  if (screenState) { screenState = 0; powerApply(); tft.setBrightness(S.brightness); dirty = true; }
  return wasOff;
}
static void toPx(double lat, double lon, int z, double &x, double &y);
// Following, every pass: remember where the followed station is, move the map when it has moved a pixel or more, warn when the battery is low.
static void chaseTick() {
  if (demoMode) return;                                        // demo screenshots set the map position themselves
  if (!chase.followingMe()) for (auto &s : stations) if (s.used && !s.noPos && strcmp(s.call, chase.call) == 0) { chase.have = true; chase.lat = s.lat; chase.lon = s.lon; chase.heard = s.heard; break; }
  if (int w = chaseBatteryWarning(chase, batPct, batCharging)) { say(w == 10 ? "Battery 10 %: the screen is kept on" : "Battery 20 %: the screen is kept on"); haptic.play(H_ERROR); }
  bool wasPaused = chase.paused;
  if (!chaseFollowing(chase, millis())) return;
  if (wasPaused) dirty = true;
  bool me = chase.followingMe();
  if (me ? !gps.fix.valid : !chase.have) return;
  double lat = me ? gps.fix.lat : chase.lat, lon = me ? gps.fix.lon : chase.lon, ax, ay, bx, by;
  toPx(lat, lon, ZOOMS[zi], ax, ay); toPx(clat, clon, ZOOMS[zi], bx, by);
  if (fabs(ax - bx) >= 1.0 || fabs(ay - by) >= 1.0) { clat = lat; clon = lon; dirty = true; }
}
static void screenTick() {
  if (panelRetryUntil && screenState != 2) {                  // the panel's sleep-out, repeated for 3 s after a wake (harmless when it is already out)
    if ((int32_t)(millis() - panelRetryUntil) >= 0) panelRetryUntil = 0;
    else if (millis() - panelRetryAt > 500) { panelRetryAt = millis(); tft.wakeup(); tft.setBrightness(S.brightness); dirty = true; plog('x'); }
  }
  // Processor speed: 240 MHz while in use, 80 MHz when nothing was touched for 5 s. With the setting "slow40", 40 MHz while the screen is off,
  // but not with USB power (the USB port) or in phone setup (WiFi). Measured with the GPS on: 52 mA at 80, 42 at 40; at 20 MHz GPS data broke.
  static bool usbIn = true; static uint32_t usbAt = 0;
  if (!(S.slow40 && screenState == 2)) { usbIn = true; usbAt = 0; }
  else if (!usbAt || millis() - usbAt > 2000) { usbAt = millis() | 1; usbIn = usbPresent(); }
  cpuSet(millis() - lastActivity <= 5000 ? 240 : ((S.slow40 && screenState == 2 && !setupMode && !demoMode && !usbIn) ? 40 : 80));
  if (chase.keepScreen) { if (screenState) { screenState = 0; powerApply(); tft.setBrightness(S.brightness); dirty = true; } return; }   // never dims or switches off
  if (!S.dimSec && !S.offSec) { if (screenState) { screenState = 0; tft.setBrightness(S.brightness); } return; }
  uint32_t idle = (millis() - lastActivity) / 1000;
  int want = (S.offSec && idle >= (uint32_t)S.offSec) ? 2 : ((S.dimSec && idle >= (uint32_t)S.dimSec) ? 1 : 0);
  if (want == 2 && screenState != 2) { asleepSum = 0; asleepN = 0; asleepMin = 0; asleepMax = -32768; asleepUsbSkipped = 0; }
  if (want == 2) { static uint32_t sampledAt = 0; if (millis() - sampledAt > 2000) { sampledAt = millis(); uint8_t b[2]; if (i2cRead(ADDR_BQ27220, 0x0C, b, 2) == 0) { if (sampledAt - lastActivity > 6000) { if (usbPresent()) asleepUsbSkipped++; else { int ma = (int16_t)(b[0] | (b[1] << 8)); asleepSum += ma; asleepN++; if (ma < asleepMin) asleepMin = ma; if (ma > asleepMax) asleepMax = ma; } } } } }
  if (want != screenState) { screenState = want; powerApply(); tft.setBrightness(want == 2 ? 0 : (want == 1 ? std::min(S.brightness, 20) : S.brightness)); }
}
static uint8_t keyFiltered() { uint8_t e = keys.poll(); if (e) plog('k', e); if (e && screenTouch()) return 0; return e; }
static void applySettings() {
  screenState = 0; powerApply(); lastActivity = millis();
  tft.setBrightness(S.brightness);
  haptic.enabled = S.haptic; M.setCall(S.call);
  if (radioOk) radio->setOutputPower(S.txDbm);
  beacon.movingMs = (uint32_t)S.movingSec * 1000UL; beacon.stationaryMs = (uint32_t)S.stationaryMin * 60000UL; beacon.moveM = S.moveMetres;
}

// ---- Phone setup: the pager opens its own WiFi hotspot and serves the setup page (Enter key). Off again when you leave. ----
static WebServer web(80);
static DNSServer dns;     // answers every name with the pager's own address, so phones show the "sign in to network" prompt and open the page
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

// Channel activity detection with a time limit. RadioLib's scanChannel() waits for the radio's "done" pin with no limit, and on this pager the
// main loop hung there until the watchdog reset it. A normal check takes a few LoRa symbols (about 33 ms each at SF12).
static const uint32_t CAD_TIMEOUT_MS = 1500;
static const int CAD_STUCK = -30000;                 // our own result: the check did not start or did not finish
static bool cadForceTimeout = false;                 // serial command PT: pretend the next check never finishes (tests the recovery)
static int scanChannelBounded() {
  bool forced = cadForceTimeout; cadForceTimeout = false;
  int st = radio->startChannelScan();
  if (st == RADIOLIB_ERR_NONE) {
    uint32_t t0 = millis();
    while (millis() - t0 < CAD_TIMEOUT_MS) {
      if (!forced && digitalRead(PIN_LORA_IRQ)) return radio->getChannelScanResult();
      yield();
    }
  }
  // stuck: note what the radio looked like (kept across restarts, printed by PW), then reset it
  plogRtc.cadTimeouts++; plogRtc.cadMs = millis(); plogRtc.cadStart = st;
  plogRtc.cadBusy = digitalRead(PIN_LORA_BUSY); plogRtc.cadIrq = radio->getIrqFlags();
  plogRtc.cadReinit = radioSetup() ? 1 : 0;
  rxFlag = false; rxApply();
  plog('c', plogRtc.cadReinit, (int)plogRtc.cadTimeouts);
  Serial.printf("channel check stuck%s: start %d, busy pin %d, irq flags 0x%04lx, radio reset %s\n", forced ? " (forced by PT)" : "",
                st, plogRtc.cadBusy, (unsigned long)plogRtc.cadIrq, plogRtc.cadReinit ? "ok" : "FAILED");
  return CAD_STUCK;
}

// Put one frame on the air after listening first (channel activity detection, up to 3 tries with a random wait).
// Returns 0 = sent, 1 = channel busy (nothing sent), 2 = the radio failed. Blocks about 3 s at SF12.
static int txFrame(const unsigned char *f, size_t len) {
  if (demoMode) return 1;                              // demo mode never transmits
  bool free = false;
  for (int tries = 0; tries < 3 && !free; tries++) {
    pstage(20); int c = scanChannelBounded(); pstage(21);
    if (c == CAD_STUCK) { txBack.failed(millis()); return 2; }     // nothing sent; the radio has been reset, the caller tries again later
    if (c == RADIOLIB_CHANNEL_FREE) free = true; else delay(300 + (esp_random() % 1200));
  }
  if (!free) { rxApply(); return 1; }
  // On battery the knob lines pick up transmitter noise and their pin-change interrupts can fire thousands of times a second, which trips the
  // interrupt watchdog (seen on this pager by the Meshtastic project). So the knob interrupts are off while we transmit.
  detachInterrupt(PIN_ROT_A); detachInterrupt(PIN_ROT_B);
  // The antenna icon turns red while we are on the air. transmit() blocks, so it is drawn straight onto the panel just before (a few ms, far
  // quicker than a full redraw), on the map only and never with the screen off; the next redraw puts the normal icon back.
  if (screenState != 2 && stView != 3 && !ui.active() && !mainMenu.on && !helpOn && !setupMode) { drawUiIcon(tft, UI_TX_NOW, tft.width() - ICON_COL_W / 2, 65); dirty = true; }
  pstage(22); int st = radio->transmit(f, len); pstage(23);
  kLast = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B); kAcc = 0; knobSteps = 0;
  attachInterrupt(PIN_ROT_A, knobIsr, CHANGE); attachInterrupt(PIN_ROT_B, knobIsr, CHANGE);
  rxFlag = false;                                   // the TX-done interrupt shares the receive pin
  rxApply();
  pstage(24); M.noteTx(millis());
  if (st == RADIOLIB_ERR_NONE) { txBack.sent(); return 0; }
  txBack.failed(millis()); return 2;
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
static bool sendBeacon(bool automatic = false) {
  pstage(19);
  if (demoMode) { say("Demo mode: nothing is sent"); return false; }
  if (!radioOk) { say("No radio"); return false; }
  if (strcmp(S.call, "N0CALL") == 0) { say("Set your call sign: press Enter"); haptic.play(H_ERROR); return false; }
  if (!gps.fix.valid) { blogBeacon('N', 0); say("No GPS fix: no beacon"); haptic.play(H_ERROR); return false; }
  unsigned char f[200];
  size_t len = aprsBuildBeacon(f, sizeof f, S.call, TOCALL, S.path, gps.fix.lat, gps.fix.lon, S.table, S.sym, S.comment);
  if (!len) { say("Beacon too long"); return false; }
  say("Sending beacon..."); if (!ui.active() && screenState != 2) render();   // never draw while the screen is off: the panel is asleep and its bus was just woken from light sleep
  int r = txFrame(f, len);
  blogBeacon('B', r);
  if (r == 1) { say("Channel busy: beacon skipped"); return false; }
  if (r != 0) { say("TX failed"); if (!automatic || txBack.buzz()) haptic.play(H_ERROR); return false; }   // you asked: always buzz; on its own: once
  txCount++; beacon.sent(millis(), gps.fix.lat, gps.fix.lon); lastBeaconUtc = utcNow();
  pstage(25); haptic.play(H_MESSAGE); pstage(26); say("Beacon sent");
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
  if (radioOk) rxApply();
  say("Power off did not work"); haptic.play(H_ERROR); dirty = true;
}
static void openMainMenu() {
  mainMenu.n = 0; mainMenu.sel = 0; mainMenu.on = true; strcpy(mainMenu.title, "Menu");
  mainMenu.info1[0] = mainMenu.info2[0] = mainMenu.info3[0] = 0;                // battery current from the fuel gauge, with words: negative = the battery is giving power
  { uint8_t b[2]; if (i2cRead(ADDR_BQ27220, 0x0C, b, 2) == 0) { int ma = (int16_t)(b[0] | (b[1] << 8));
      if (ma < 0) snprintf(mainMenu.info1, sizeof mainMenu.info1, "Battery, screen on:   using %d mA", -ma); else snprintf(mainMenu.info1, sizeof mainMenu.info1, "Battery, screen on:   charging %d mA", ma);
      if (!asleepHave) { strcpy(mainMenu.info2, "Battery, screen off:  no reading yet"); strcpy(mainMenu.info3, "Let the screen go off for a minute, then wake it."); }
      else if (asleepAvg < 0) snprintf(mainMenu.info2, sizeof mainMenu.info2, "Battery, screen off:  using %d mA (average)", -asleepAvg);
      else snprintf(mainMenu.info2, sizeof mainMenu.info2, "Battery, screen off:  USB was plugged in");
    } else { strcpy(mainMenu.info1, "Battery current: no reading from the gauge"); } }
  mmAdd("Messages", MA_MESSAGES); mmAdd("Phone setup", MA_SETUP); mmAdd("Beacon now", MA_BEACON);
  if (!chase.followingMe()) { char t[32]; snprintf(t, sizeof t, "Following %s: follow me", chase.call); mmAdd(t, MA_FOLLOW_ME); }
  else if (nOrd && selCall[0]) { char t[32]; snprintf(t, sizeof t, "Follow %s", selCall); mmAdd(t, MA_FOLLOW_STATION); }     // the station selected in the list
  mmAdd(chase.keepScreen ? "Screen is kept on: turn off" : "Keep screen on...", chase.keepScreen ? MA_KEEP_OFF : MA_KEEP_ASK);
  mmAdd(autoBeacon ? "Auto-beacon is ON: turn off" : "Auto-beacon is OFF: turn on", MA_AUTO);
  mmAdd("Help, restart history", MA_HELP); mmAdd("Power off", MA_POWEROFF_ASK); mmAdd("Close menu", MA_CLOSE);
  dirty = true;
}
static void mainMenuAct(MenuAct a) {
  mainMenu.on = false; dirty = true;
  switch (a) {
    case MA_MESSAGES: ui.open(); break;
    case MA_HELP: helpOn = true; helpPage = 0; break;
    case MA_SETUP: startSetup(); drawSetup(); break;
    case MA_BEACON: if (beacon.manualOk(millis())) sendBeacon(); else say("Wait: beacon just sent"); break;
    case MA_AUTO: autoBeacon = !autoBeacon; if (autoBeacon) beacon.start(millis()); say(autoBeacon ? "Auto beacon ON" : "Auto beacon off"); break;
    case MA_FOLLOW_ME: chaseFollow(chase, nullptr); say(gps.fix.valid ? "Following me" : "Following me: waiting for a GPS fix"); break;
    case MA_FOLLOW_STATION:
      chaseFollow(chase, selCall);
      for (auto &s : stations) if (s.used && !s.noPos && strcmp(s.call, chase.call) == 0) { chase.have = true; chase.lat = s.lat; chase.lon = s.lon; chase.heard = s.heard; }
      { static char note[40]; snprintf(note, sizeof note, "Following %s", chase.call); say(note); } break;
    case MA_KEEP_ASK:
      mainMenu.n = 0; mainMenu.sel = 0; mainMenu.on = true; strcpy(mainMenu.title, "Keep the screen on?");
      strcpy(mainMenu.info1, "It will not dim or switch off."); strcpy(mainMenu.info2, "This uses much more battery."); mainMenu.info3[0] = 0;
      mmAdd("No, cancel", MA_NO); mmAdd("Yes, keep it on", MA_KEEP_YES); break;          // No first: a stray press cannot switch it on
    case MA_KEEP_YES: chaseKeepScreen(chase, true); say("Screen stays on"); break;
    case MA_KEEP_OFF: chaseKeepScreen(chase, false); screenTouch(); say("Screen times out as usual"); break;
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
  if (chase.followingMe()) chase.paused = false; else chaseLook(chase, millis());
}

// Light sleep while the screen is off: the processor stops until the radio hears a packet (DIO1), a key or the knob moves, the GPS starts talking,
// or one second passes for the timers. Never with USB connected (the serial port would vanish) or in phone setup.
extern volatile int32_t knobSteps; extern volatile uint8_t kLast;
static void lightSleepOnce() {
  uint8_t st = 0; if (i2cRead(ADDR_BQ25896, 0x0B, &st, 1) != 0 || (st >> 5) != 0) { delay(10); return; }   // charger unreadable or power on VBUS: stay awake
  if (digitalRead(PIN_LORA_IRQ)) { rxFlag = true; delay(1); return; }                                  // a packet is already waiting
  pstage(1);
  detachInterrupt(PIN_ROT_A); detachInterrupt(PIN_ROT_B);                                // no knob handler while the lines are level wake sources: noise on them on battery made a storm of interrupts inside the sleep call
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  // The crash log (panic hook) showed the interrupt watchdog firing inside the GPIO interrupt routine right at wake-up: gpio_wakeup_enable() turns a pin into a LEVEL
  // interrupt, and a pin that still had its processor interrupt enabled (the radio's DIO1, the knob lines) then re-fires without end when the processor wakes, before
  // our code runs. Waking does not need the processor interrupt, so it is switched off for every wake pin first and switched on again after the restore below.
  for (int pin : {PIN_LORA_IRQ, PIN_KB_INT, PIN_ROT_PRESS, PIN_ROT_A, PIN_ROT_B, PIN_GPS_RX}) gpio_intr_disable((gpio_num_t)pin);
  gpio_wakeup_enable((gpio_num_t)PIN_LORA_IRQ, GPIO_INTR_HIGH_LEVEL);
  // Only in the GPS's long sleep is there anything to cut short; otherwise (walking: a report about twice a second) the once-a-second wake reads them.
  bool motionWake = S.gpsMotion && motionOk && aGps && gpsSleeping && gpsAd.still >= 2 && !digitalRead(PIN_MOTION_INT);      // a line already high is left to the polling
  if (motionWake) { gpio_intr_disable((gpio_num_t)PIN_MOTION_INT); gpio_wakeup_enable((gpio_num_t)PIN_MOTION_INT, GPIO_INTR_HIGH_LEVEL); }
  if (digitalRead(PIN_KB_INT)) gpio_wakeup_enable((gpio_num_t)PIN_KB_INT, GPIO_INTR_LOW_LEVEL);
  if (digitalRead(PIN_ROT_PRESS)) gpio_wakeup_enable((gpio_num_t)PIN_ROT_PRESS, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_ROT_A, digitalRead(PIN_ROT_A) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_ROT_B, digitalRead(PIN_ROT_B) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  // no wake on the GPS line: with the GPS talking, its level wake fired in a storm and the interrupt watchdog reset the chip (26 resets overnight, all inside the sleep call)
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(1000000ULL);
  Serial.flush();
  if (!plogSleepOpen) plog('s', pinBits());                                                  // one entry per stretch of sleeping; plain one-second timer wakes are only counted
  pstage(2); plogRtc.pins = pinBits();
  esp_light_sleep_start();
  pstage(3); plogRtc.beats++;
  lightSlept = true;
  { int cause = (int)esp_sleep_get_wakeup_cause(), pb = pinBits();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) plogTimerWakes++;
    else if ((pb & 1) == 0 && (pb & 4) && (pb & 8) && (pb & 16) && !(pb & 32)) plogRtc.gpsWakes++;      // only the GPS start bit: counted, not logged
    else if (motionWake && digitalRead(PIN_MOTION_INT) && !(pb & 1) && (pb & 4) && (pb & 8) && (pb & 16)) motionPinWakes++;      // counted, not logged
    else plog('w', cause, pb); }
  // gpio_wakeup_enable() had made these pins level-triggered interrupts: undo that at once, or a pin still at its wake level fires its interrupt forever (watchdog reset)
  for (int pin : {PIN_LORA_IRQ, PIN_KB_INT, PIN_ROT_PRESS, PIN_ROT_A, PIN_ROT_B, PIN_GPS_RX}) gpio_wakeup_disable((gpio_num_t)pin);
  if (motionWake) { gpio_wakeup_disable((gpio_num_t)PIN_MOTION_INT); gpio_set_intr_type((gpio_num_t)PIN_MOTION_INT, GPIO_INTR_DISABLE); }     // no processor interrupt on this pin
  gpio_set_intr_type((gpio_num_t)PIN_LORA_IRQ, GPIO_INTR_POSEDGE);                       // the radio library uses a rising edge
  gpio_intr_enable((gpio_num_t)PIN_LORA_IRQ);
  attachInterrupt(PIN_ROT_A, knobIsr, CHANGE); attachInterrupt(PIN_ROT_B, knobIsr, CHANGE);                                      // the knob uses CHANGE
  for (int pin : {PIN_KB_INT, PIN_ROT_PRESS, PIN_GPS_RX}) gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);                // these are polled
  pstage(4);
  if (digitalRead(PIN_LORA_IRQ)) rxFlag = true;                                              // the edge interrupt may not have run during sleep
  uint8_t ks = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B);
  if (ks != kLast) { kLast = ks; screenTouch(); }                                            // the knob moved: wake the screen, the turn itself is ignored
}
// Development test (serial command PX): which part of switching the SD card off upsets the radio on the shared SPI bus? Takes the steps one
// at a time and asks the radio to run a channel check after each (receive only, nothing is transmitted). Restores the card and radio.
static bool pxRadio(const char *what) {
  int okCount = 0, st = 0; uint32_t flags = 0; bool done = false;
  for (int k = 0; k < 3; k++) {
    st = radio->startChannelScan(); done = false;
    if (st == RADIOLIB_ERR_NONE) { uint32_t t0 = millis(); while (millis() - t0 < 1500) { if (digitalRead(PIN_LORA_IRQ)) { done = true; break; } delay(2); } }
    flags = radio->getIrqFlags();
    if (st == RADIOLIB_ERR_NONE && done && (flags & (RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED))) okCount++;
    delay(100); esp_task_wdt_reset();
  }
  Serial.printf("PX  %-52s radio %s (%d/3; start %d, done pin %d, flags 0x%04lx, busy %d)\n", what, okCount == 3 ? "OK  " : (okCount ? "FLAKY" : "FAIL"), okCount, st, done, (unsigned long)flags, digitalRead(PIN_LORA_BUSY));
  return okCount == 3;
}
static void pxPower(bool on) { xp.output(XP_SD_EN, on ? HIGH : LOW); delay(150); esp_task_wdt_reset(); }
static bool pxRestore() {                                    // card powered and mounted, radio set up again
  if (sdParkLines) { sdParkLines = false; busPark(false); }
  sdUnpowered = false; pinMode(PIN_SD_CS, OUTPUT); digitalWrite(PIN_SD_CS, HIGH); pxPower(true); delay(300); SD.end();
  bool card = SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000);
  bool rad = radioSetup(); rxFlag = false; esp_task_wdt_reset();
  Serial.printf("PX  -- restored: card mount %s, radio setup %s\n", card ? "ok" : "FAILED", rad ? "ok" : "FAILED");
  return card && rad;
}
// The fix under test: card off, CS high only during radio exchanges, after a pause. Many checks, with the line left floating for different
// lengths of time in between, then a radio reset (many exchanges back to back).
static void pxSettle(uint32_t us) {
  pxRestore();
  SD.end(); xp.output(XP_SD_EN, LOW); pinMode(PIN_SD_CS, INPUT); sdSettleUs = us; sdUnpowered = true;
  uint32_t t0 = millis(); while (millis() - t0 < 3000) { delay(20); esp_task_wdt_reset(); }                   // let the card's supply drain
  static const uint32_t GAP[4] = {0, 100, 1000, 5000}; static const int N[4] = {10, 10, 5, 3};
  int good = 0, all = 0; char line[96]; int at = 0;
  for (int g = 0; g < 4; g++) {
    int ok = 0;
    for (int k = 0; k < N[g]; k++) {
      uint32_t w = millis(); while (millis() - w < GAP[g]) { delay(10); esp_task_wdt_reset(); }
      int st = radio->startChannelScan(); bool done = false;
      if (st == RADIOLIB_ERR_NONE) { uint32_t c0 = millis(); while (millis() - c0 < 1500) { if (digitalRead(PIN_LORA_IRQ)) { done = true; break; } delay(2); } }
      uint32_t fl = radio->getIrqFlags();
      if (st == RADIOLIB_ERR_NONE && done && (fl & (RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED))) ok++;
      esp_task_wdt_reset();
    }
    at += snprintf(line + at, sizeof line - at, "  gap %lu ms: %d/%d", (unsigned long)GAP[g], ok, N[g]); good += ok; all += N[g];
  }
  Serial.printf("PX pause %5lu us:%s\n", (unsigned long)us, line);
  int resets = 0; for (int k = 0; k < 3; k++) { if (radioSetup()) resets++; esp_task_wdt_reset(); }
  Serial.printf("PX pause %5lu us: radio reset %d/3 -> %s\n", (unsigned long)us, resets, (good == all && resets == 3) ? "ALL PASSED" : "failed");
  sdSettleUs = SD_SETTLE_US;
}
static void pxRun() {
  screenTouch();
  Serial.println("PX start (2 to 4 minutes): card off, CS high only during exchanges, with a pause before each");
  static const uint32_t PAUSE_US[6] = {0, 20, 100, 500, 2000, 10000};
  for (int i = 0; i < 6; i++) pxSettle(PAUSE_US[i]);
  pxRestore();
  pxRadio("end: card powered and mounted");
  rxFlag = false; rxApply(); aSd = false; lastActivity = millis(); dirty = true;
  Serial.println("PX done");
}

#ifdef POWER_TEST
#include "powertest.h"
#endif

void loop() {
  esp_task_wdt_reset(); pstage(5);
  rstHistAlive();
#ifdef POWER_TEST
  powerTestTick();
#endif
  plogRtc.autoOn = autoBeacon ? 1 : 0; if (millis() > 1800000UL) plogRtc.autoRestores = 0;
  if (root == "") { delay(1000); return; }
  if (pwrSleep && screenState == 2 && !setupMode && !demoMode && radioOk && millis() - lastGpsByte > 400 && (gpsSleeping || !S.gpsSleep || !pwrGps)) lightSleepOnce();
  else delay(screenState ? 10 : 2);                  // let the processor idle between passes (the radio, GPS and knob are interrupt or buffer driven)

  // GPS characters
  while (Serial1.available()) {
    lastGpsByte = millis(); gpsWinBytes++;
    if (demoMode) { Serial1.read(); continue; }                // demo: the real GPS is ignored
    char gc = (char)Serial1.read(); unsigned long badBefore = gps.bad; bool gpsUpdate = gps.feed(gc);
    if (gc == '\n') { int k = getCpuFrequencyMhz() < 80; if (gps.bad != badBefore) nmeaBad[k]++; else nmeaOk[k]++; }
    if (!gpsUpdate) continue;
    if (gps.fix.valid) gpsEverFix = true;
    // only redraw when my dot would move at least a pixel at this zoom, or the fix appeared/vanished
    static bool lastValid = false; static double lastX = 0, lastY = 0;
    double gx = 0, gy = 0; if (gps.fix.valid) toPx(gps.fix.lat, gps.fix.lon, ZOOMS[zi], gx, gy);
    if (gps.fix.valid != lastValid || (gps.fix.valid && (fabs(gx - lastX) >= 1.0 || fabs(gy - lastY) >= 1.0))) {
      lastValid = gps.fix.valid; lastX = gx; lastY = gy; dirty = true;
    }
  }

  // Test hooks from the Mac (used to take screenshots): "S" screenshot, "M" open Messages, "G<lat>,<lon>,<zoom>\n" put the map there,
  // "V<0-3>\n" choose the L view, "C<call>\n" select a heard station, "X\n" back to the map.
  {
    static char cmd[40]; static int cmdN = -1;
    while (Serial.available()) {
      int c = Serial.read();
      if (cmdN >= 0) {
        if (c != '\n' && c != '\r') { if (cmdN < (int)sizeof cmd - 1) cmd[cmdN++] = (char)c; continue; }
        cmd[cmdN] = 0; cmdN = -1;
        if (cmd[0] == 'G') { chaseLook(chase, millis());           // a position set over USB counts as looking around
         
          double la, lo; int z;
          if (sscanf(cmd + 1, "%lf,%lf,%d", &la, &lo, &z) == 3 && fabs(la) < 85 && fabs(lo) <= 180) {
            for (int i = 0; i < NZ; i++) if (ZOOMS[i] == z) zi = i;
            clat = la; clon = lo; dirty = true; Serial.printf("map: %.4f %.4f z%d\n", la, lo, ZOOMS[zi]);
          }
        } else if (cmd[0] == 'V') { int v = atoi(cmd + 1); if (v >= 0 && v <= 3) { stView = v; if (stView != 0 && mode == M_LIST) mode = M_PANX; dirty = true; } }
        else if (cmd[0] == 'C') { strncpy(selCall, cmd + 1, 9); selCall[9] = 0; lastKnobAt = millis(); dirty = true; }
        else if (cmd[0] == 'X') { ui.close(); mainMenu.on = false; helpOn = false; dirty = true; }
        else if (cmd[0] == 'H') { helpOn = true; helpPage = constrain(atoi(cmd + 1), 0, 2); dirty = true; }
        else if (cmd[0] == 'D') { double la, lo; if (sscanf(cmd + 1, "%lf,%lf", &la, &lo) == 2) { loadDemo(la, lo); clat = la; clon = lo; dirty = true; Serial.println("demo mode on: simulated stations, nothing is transmitted until RST"); } }
        else if (cmd[0] == 'P') {
          if (cmd[1] == 'S') { pwrSd = cmd[2] == '1'; }
          else if (cmd[1] == 'D') { pwrPanel = cmd[2] == '1'; }
          else if (cmd[1] == 'G') { pwrGps = cmd[2] == '1'; powerApply(); }
          else if (cmd[1] == 'A') Serial.printf("screen-off current: %d samples, average %d mA, min %d, max %d (most negative = most drawn), %d samples skipped because USB was in\n", asleepN, asleepN ? (int)(asleepSum / asleepN) : 0, asleepMin, asleepMax, asleepUsbSkipped);
          else if (cmd[1] == 'N') {                                               // count what the GPS sends for 12 s, in 1 s steps
            uint32_t t0 = millis(); int sec[12] = {0};
            while (millis() - t0 < 12000) { while (Serial1.available()) { Serial1.read(); int k = (millis() - t0) / 1000; if (k < 12) sec[k]++; } delay(5); }
            Serial.print("GPS bytes per second:"); for (int i = 0; i < 12; i++) Serial.printf(" %d", sec[i]); Serial.println();
          }
          else if (cmd[1] == 'B') {                                               // screen repairs, tried one at a time while the screen is dark
            int m = atoi(cmd + 2); lastActivity = millis();                  // a repair counts as activity: the screen must not time out while we watch
            if (m == 0) { ledcSetup(7, 12000, 8); ledcAttachPin(PIN_DISP_BL, 7); tft.setBrightness(S.brightness); }
            else if (m == 1) { tft.wakeup(); delay(130); tft.setBrightness(S.brightness); }
            else if (m == 2) { tft.startWrite(); tft.writeCommand(0x29); tft.endWrite(); }
            else if (m == 3) { tft.init(); tft.setRotation(PAGER_ROTATION); tft.setBrightness(S.brightness); }
            dirty = true;
          }
          else if (cmd[1] == 'O') {                                               // PO<seconds>: screen-off time, saved (dim comes at half of it)
            int v = atoi(cmd + 2);
            if (v >= 0 && v <= 3600) { S.offSec = v; if (v && S.dimSec >= v) S.dimSec = v / 2; saveSettings(S); applySettings(); Serial.printf("screen off after %d s, dim after %d s (saved)\n", S.offSec, S.dimSec); }
          }
          else if (cmd[1] == 'W') {                                               // the event log, oldest first
            int n = plogN < 96 ? plogN : 96, first = plogN - n; uint32_t now = millis();
            Serial.printf("event log: %d events, %lu timer wakes this run, now %lu ms; stage %lu set at %lu ms, %lu sleeps, pins at last sleep %d\n", plogN, (unsigned long)plogTimerWakes, (unsigned long)now, (unsigned long)plogRtc.stage, (unsigned long)plogRtc.stageMs, (unsigned long)plogRtc.beats, plogRtc.pins);
            { int rn = plogRtc.rstN < 8 ? plogRtc.rstN : 8, rf = plogRtc.rstN - rn; Serial.printf("restarts (%d): ", plogRtc.rstN); for (int i = 0; i < rn; i++) { PRst &r = plogRtc.rst[(rf + i) % 8]; Serial.printf("[reason %d, stage %d, last stage at %lu ms] ", r.reason, r.stage, (unsigned long)r.upMs); } Serial.printf("\nGPS-only wakes %lu\n", (unsigned long)plogRtc.gpsWakes); 
            Serial.printf("panics recorded %lu; last: pc 0x%08lx, return 0x%08lx, sp 0x%08lx, core %lu, kind %lu (1 interrupt wdt, 2 task wdt, 3 abort, 4 fault) at %lu ms; code addresses on its stack:", (unsigned long)plogRtc.panics, (unsigned long)plogRtc.pPc, (unsigned long)plogRtc.pA0, (unsigned long)plogRtc.pSp, (unsigned long)plogRtc.pCore, (unsigned long)plogRtc.pExc, (unsigned long)plogRtc.pMs);
            for (int i = 0; i < 8; i++) if (plogRtc.pStack[i]) Serial.printf(" 0x%08lx", (unsigned long)plogRtc.pStack[i]);
            Serial.println();
            Serial.printf("stuck channel checks %lu; last at %lu ms: start %d, busy pin %d, irq flags 0x%04lx, radio reset ok %d\n", (unsigned long)plogRtc.cadTimeouts, (unsigned long)plogRtc.cadMs, plogRtc.cadStart, plogRtc.cadBusy, (unsigned long)plogRtc.cadIrq, plogRtc.cadReinit);
            Serial.printf("beacons: %lu sent, %lu busy, %lu failed, %lu held for no fix; GPS windows: %lu with a fix, %lu without\n", (unsigned long)plogRtc.bSent, (unsigned long)plogRtc.bBusy, (unsigned long)plogRtc.bFail, (unsigned long)plogRtc.bNoFix, (unsigned long)plogRtc.gFix, (unsigned long)plogRtc.gNoFix);
            blogPrint("beacon log", plogRtc.blog, BLOG_N, plogRtc.bN); blogPrint("GPS window log", plogRtc.glog, GLOG_N, plogRtc.gN);
#if GPS_HINT_CHECK
            gpsHintReport();
#endif
            Serial.printf("motion wake %s, sensor %s; woke the GPS %lu times; reports:", S.gpsMotion ? "on" : "off", motionOk ? "up" : "not started", (unsigned long)motionWakesTotal());
            Serial.printf(" (processor woken by the sensor %lu times)", (unsigned long)motionPinWakesTotal());
            for (int k = 0; k < MOT_KINDS; k++) Serial.printf(" %s %lu%s,", MOT_NAME[k], (unsigned long)motionCount[k], motionAvail[k] ? "" : " (n/a)");
            Serial.println();
            rstHistPrint();
            Serial.printf("slow processor setting %s; GPS lines at 80+ MHz: %lu good, %lu bad; below 80 MHz: %lu good, %lu bad\n", S.slow40 ? "on" : "off",
                          (unsigned long)nmeaOk[0], (unsigned long)nmeaBad[0], (unsigned long)nmeaOk[1], (unsigned long)nmeaBad[1]);
            Serial.printf("auto beacon %s, restored %d time(s) since the last 30 min stable run\n", autoBeacon ? "on" : "off", plogRtc.autoRestores); }
            for (int i = 0; i < n; i++) { PEv &e = plogBuf[(first + i) % 96]; Serial.printf("  %9lu ms  %c  %d %d\n", (unsigned long)e.ms, e.tag, e.a, e.b); }
          }
#ifdef POWER_TEST
          else if (cmd[1] == 'R') ptPrint();
#endif
          else if (cmd[1] == 'X') { if (radioOk) pxRun(); else Serial.println("PX: no radio"); }
          else if (cmd[1] == 'T') { cadForceTimeout = true; Serial.println("the next channel check will act stuck"); }
          else if (cmd[1] == 'L') { pwrSleep = cmd[2] == '1'; }
          else if (cmd[1] == 'A') {
            int n = atoi(cmd + 2); if (n < 1) n = 10; if (n > 3600) n = 3600;
            long sum = 0; int lo = 32767, hi = -32768, k = 0;
            for (int i = 0; i < n; i++) {
              delay(1000); esp_task_wdt_reset(); uint8_t b[2];
              if (i2cRead(ADDR_BQ27220, 0x14, b, 2) == 0) { int v = (int16_t)(b[0] | (b[1] << 8)); sum += v; k++; if (v < lo) lo = v; if (v > hi) hi = v; }
            }
            uint8_t vb[2]; int mv = i2cRead(ADDR_BQ27220, 0x08, vb, 2) == 0 ? (vb[0] | (vb[1] << 8)) : 0;
            if (k) Serial.printf("avg current: %d mA  (min %d, max %d, %d samples, battery %d mV)\n", (int)(sum / k), lo, hi, k, mv);
            else Serial.println("avg current: no answer");
          }
          Serial.printf("power: panel sleep %s, SD rail %s, GPS sleep %s, light sleep %s, screen %d, cpu %d MHz\n", pwrPanel ? "on" : "off", pwrSd ? "on" : "off", pwrGps ? "on" : "off", pwrSleep ? "on" : "off", screenState, getCpuFrequencyMhz());
        }
        else if (cmd[0] == 'R') { Serial.println("restarting"); delay(100); esp_restart(); }
        continue;
      }
      if (c == 'S') sendScreenshot();
      else if (c == 'B') { uint8_t b[2]; auto w = [&](uint8_t r, const char *n, bool sgn) { if (i2cRead(ADDR_BQ27220, r, b, 2) == 0) { int v = b[0] | (b[1] << 8); Serial.printf("gauge %s = %d\n", n, sgn ? (int)(int16_t)v : v); } else Serial.printf("gauge %s = no answer\n", n); };
                           w(0x08, "voltage mV", false); w(0x2C, "charge %", false); w(0x0C, "current 0x0C mA", true); w(0x14, "avg current 0x14 mA", true); w(0x0A, "flags", false); w(0x12, "full capacity mAh", false); w(0x10, "remaining mAh", false); w(0x16, "time to empty min", false); }
      else if (c == 'M' && !ui.active() && !setupMode) { ui.open(); Serial.println("ui: opened by serial"); }
      else if (c == 'G' || c == 'V' || c == 'C' || c == 'X' || c == 'D' || c == 'R' || c == 'P' || c == 'H') { cmd[0] = (char)c; cmdN = 1; }
    }
  }

  pstage(6);
  // received packets
  if (rxFlag && radioOk) {
    rxFlag = false;
    String f; int st = radio->readData(f);
    if (st == RADIOLIB_ERR_NONE) {
      rxIconUntil = millis() + RX_ICON_MS; if (!rxIconUntil) rxIconUntil = 1; dirty = true;     // any packet read, our own digipeated beacon too
      rxCount++;
      { char shown[200]; size_t n = std::min<size_t>(f.length(), sizeof shown - 1); for (size_t i = 0; i < n; i++) shown[i] = (f[i] >= 32 && f[i] < 127) ? f[i] : '.'; shown[n] = 0;
        Serial.printf("rx rssi=%.0f snr=%.1f (%u bytes): %s\n", radio->getRSSI(), radio->getSNR(), (unsigned)f.length(), shown); }
      {   // one of our own packets coming back through a digipeater: list the station that sent us this copy, and say so
        const char *t = f.c_str(); size_t tn = f.length(); if (tn > 3 && (uint8_t)t[0] == 0x3C) { t += 3; tn -= 3; }
        size_t cl = strlen(S.call);
        if (tn > cl && strncasecmp(t, S.call, cl) == 0 && t[cl] == '>') {
          char path[40], via[10], rep[10]; aprsPathInfo(t, tn, path, sizeof path, via, sizeof via);
          if (aprsRepeater(path, rep, sizeof rep) && strcasecmp(rep, S.call) != 0) {
            stationRepeatedUs(rep, radio->getRSSI(), radio->getSNR(), t, tn);
            if (stView == 3 && (lastKnobAt == 0 || millis() - lastKnobAt > FOLLOW_PAUSE_MS)) { strncpy(selCall, rep, 9); selCall[9] = 0; }
            if (!ui.active()) { static char note[64]; snprintf(note, sizeof note, "Repeated by %s  %.0f dBm  SNR %+.0f", rep, radio->getRSSI(), radio->getSNR()); say(note); }
            dirty = true;
          }
        }
      }
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
    rxApply();
  }

  if (setupMode) { setupTick(); return; }   // while the phone setup screen is open, nothing else runs (receiving still does)

  pstage(7);
  screenTick(); gpsPosSave(); gpsWakeCheck(); motionDuty(); gpsDuty(); chaseTick();
  { static uint32_t clkTickAt = 0; static int lastMin = -1, lastPct = -2; static bool lastChg = false;
    if (millis() - clkTickAt > 1000) {
      clkTickAt = millis(); clockTick();
      uint32_t u = utcNow(); int mn = u ? (int)(u / 60) : -1;
      if (mn != lastMin || batPct != lastPct || batCharging != lastChg) { lastMin = mn; lastPct = batPct; lastChg = batCharging; if (ui.active()) ui.dirty = true; else dirty = true; }
    } }

  pstage(8);
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
    else if (helpOn) { if (e & 0x80) { if ((e & 0x7F) == KEY_ENTER || (e & 0x7F) == KEY_BACKSPACE) helpOn = false; else helpPage = (helpPage + 1) % 3; dirty = true; } }
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
  if (pressed && !longDone && millis() - pressedAt >= 600) { longDone = true; if (helpOn) { helpOn = false; dirty = true; } else if (mainMenu.on) { mainMenu.on = false; dirty = true; haptic.play(H_CLICK); } else if (ui.active()) { ui.press(true); haptic.play(H_CLICK); if (!ui.active()) dirty = true; } else if (stView == 3) { stView = 0; haptic.play(H_CLICK); say(VIEW_NAME[0]); dirty = true; } else centerOnGps(); }
  if (!pressed && wasPressed && !longDone) { if (helpOn) { helpOn = false; dirty = true; } else if (mainMenu.on) { haptic.play(H_CLICK); mainMenuAct(mainMenu.act[mainMenu.sel]); } else if (ui.active()) { ui.press(false); haptic.play(H_TICK); if (!ui.active()) dirty = true; } else if (stView == 3) { haptic.play(H_CLICK); ui.open(); ui.openCompose(selCall); } else { if (chase.paused) chaseLook(chase, millis()); mode = nextMode(mode); knobIconUntil = millis() + KNOB_ICON_MS; if (!knobIconUntil) knobIconUntil = 1; haptic.play(H_TICK); dirty = true; } }
  wasPressed = pressed;

  int32_t d; noInterrupts(); d = knobSteps; knobSteps = 0; interrupts();
  if (d && screenTouch()) d = 0;
  if (d && helpOn) { helpPage = (helpPage + (d > 0 ? 1 : 2)) % 3; haptic.play(H_TICK); dirty = true; }
  else if (d && mainMenu.on) { mainMenu.sel = constrain(mainMenu.sel + (d > 0 ? -1 : 1), 0, mainMenu.n - 1); haptic.play(H_TICK); dirty = true; }
  else if (d && ui.active()) { for (int i = 0, n = abs(d); i < n; i++) ui.knob(d > 0 ? 1 : -1); haptic.play(H_TICK); }
  else if (d) {
    haptic.play(H_TICK);
    if (stView == 3 || mode != M_ZOOM) chaseLook(chase, millis());          // zooming keeps following
    if (stView == 3) {                               // full list: the knob moves the selection (up on the knob = up the list)
      lastKnobAt = millis();                         // and stops it following new packets for a while
      if (nOrd) {
        int i = constrain(selIndex() + (d > 0 ? -1 : 1), 0, nOrd - 1);
        const Station &st = stations[order[i]];
        strncpy(selCall, st.call, 9); selCall[9] = 0; if (!st.noPos) { clat = st.lat; clon = st.lon; }
      }
    } else if (mode == M_ZOOM) {
      int n = constrain(zi + (d > 0 ? 1 : -1), 0, NZ - 1);
      if (n != zi) zi = n;
    } else if (mode == M_LIST) {
      if (nOrd) {                                    // up on the knob = up the list; the map follows the selection
        int i = constrain(selIndex() + (d > 0 ? -1 : 1), 0, nOrd - 1);
        const Station &st = stations[order[i]];
        strncpy(selCall, st.call, 9); selCall[9] = 0; if (!st.noPos) { clat = st.lat; clon = st.lon; }
      }
    } else {
      int z = ZOOMS[zi]; double cx, cy; toPx(clat, clon, z, cx, cy);
      double step = 64.0 * d;
      if (mode == M_PANX) cx += step; else cy -= step;   // up on the knob = north
      fromPx(cx, cy, z, clat, clon);
    }
    dirty = true;
  }
  pstage(9);
  // auto beacon
  static uint32_t nextTry = 0;
  static bool noFixNoted = false, firstFixNoted = false;
  if (gps.fix.valid && !firstFixNoted && !demoMode) { firstFixNoted = true; blogBeacon('F', 0); }
  if (!autoBeacon || gps.fix.valid) noFixNoted = false;
  else if (!noFixNoted && beacon.due(millis(), true, gps.fix.lat, gps.fix.lon)) { noFixNoted = true; blogBeacon('N', 1); }   // would be due if there were a fix
  if (autoBeacon && (int32_t)(millis() - nextTry) >= 0 && txBack.allowed(millis()) && beacon.due(millis(), gps.fix.valid, gps.fix.lat, gps.fix.lon)) { if (!sendBeacon(true)) nextTry = millis() + 10000; }

  pstage(10);
  // messages and acks waiting to go out
  if (radioOk && M.callOk() && txBack.allowed(millis())) {            // while the radio is failing, queued messages wait without using up tries
    msg::TxItem it;
    if (M.nextTx(millis(), it)) {
      if (!ui.active()) { say(it.isAck ? "Sending ack..." : "Sending message..."); if (screenState != 2) render(); }
      int r = txEngineItem(it); M.txDone(r == 0, millis());
      if (r == 2) { if (txBack.buzz()) haptic.play(H_ERROR); } else if (r == 0 && !it.isAck) haptic.play(H_TICK);
      ui.dirty = true; dirty = true;
    }
  }
  // save the messages a few seconds after the last change (flash, not the SD card)
  static uint32_t chgAt = 0;
  if (M.changed) { if (!chgAt) chgAt = millis(); if (millis() - chgAt > 3000) { msgSave(); M.changed = false; chgAt = 0; } }

  pstage(11);
  // the messages screens replace the map while they are open
  if (ui.active()) {
    static uint32_t uiDrawn = 0;
    if (screenState != 2 && (ui.dirty || millis() - uiDrawn > 1000)) {
      uiDrawn = millis(); ui.draw(fbOk ? (lgfx::LovyanGFX &)fb : (lgfx::LovyanGFX &)tft); if (fbOk) fb.pushSprite(0, 0);
    }
    return;
  }

  // keep the "age" column current: refresh every 15 s while the list is showing
  if (knobIconUntil && (int32_t)(millis() - knobIconUntil) >= 0) { knobIconUntil = 0; dirty = true; }     // take the knob icon away again
  if (rxIconUntil && (int32_t)(millis() - rxIconUntil) >= 0) { rxIconUntil = 0; dirty = true; }     // antenna back to green or grey
  static uint32_t chaseDrawn = 0;
  if (!chase.followingMe() && millis() - chaseDrawn > 1000) { chaseDrawn = millis(); dirty = true; }
  static uint32_t lastAge = 0;
  if ((stView == 0 || stView == 3) && millis() - lastAge > 15000) { lastAge = millis(); dirty = true; }
  static uint32_t lastFade = 0;
  if (stView == 3 && millis() - lastHeardAt < FRESH_MS + 1500 && millis() - lastFade > 1000) { lastFade = millis(); dirty = true; }     // let the green highlight fade out
  pstage(12);
  // redraw at most every 300 ms (stations and GPS arrive in bursts)
  if (dirty && screenState != 2 && millis() - lastDraw > 300) { dirty = false; lastDraw = millis(); render(); }
  else if (notice && millis() > noticeUntil && !dirty) { notice = nullptr; dirty = true; }
}
