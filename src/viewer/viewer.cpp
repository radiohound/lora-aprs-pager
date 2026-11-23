// Tile viewer: draws the T-Deck card's raster tiles (/LoRa_Tracker/Maps/<REGION>/<z>/<x>/<y>.jpg|png) placed by x/y.
// Read-only on the SD card. Knob only: turn = zoom or pan, press = switch what the knob does (ZOOM -> PAN W-E -> PAN N-S).
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
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

// ---- Stations heard (positions only for now) ----
struct Station { char call[10]; double lat, lon; char table, sym; uint32_t heard; float rssi; bool used; };
static Station stations[48];
static int stationCount() { int n = 0; for (auto &s : stations) if (s.used) n++; return n; }
static void stationUpdate(const AprsPos &p, float rssi) {
  Station *slot = nullptr, *oldest = nullptr;
  for (auto &s : stations) {
    if (s.used && strcmp(s.call, p.call) == 0) { slot = &s; break; }
    if (!s.used && !slot) slot = &s;
    if (s.used && (!oldest || s.heard < oldest->heard)) oldest = &s;
  }
  if (!slot) slot = oldest;
  strncpy(slot->call, p.call, 9); slot->call[9] = 0;
  slot->lat = p.lat; slot->lon = p.lon; slot->table = p.table; slot->sym = p.sym;
  slot->heard = millis(); slot->rssi = rssi; slot->used = true;
}

// ---- GPS ----
static NmeaParser gps;
static bool dirty = false;
static uint32_t lastDraw = 0;
static const char *notice = nullptr; static uint32_t noticeUntil = 0;
static void say(const char *m) { notice = m; noticeUntil = millis() + 2500; dirty = true; }

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
// Station view, stepped by the L key: 0 = list + markers, 1 = markers only (full-width map), 2 = clean map (no stations)
static int stView = 0;
static bool sortByDistance = true;          // S key: nearest first, or most recently heard first
static char selCall[10] = "";               // selected station (kept by call sign so it survives re-sorting)
static const int LIST_W = 130;
static const char *VIEW_NAME[] = {"stations: list + map", "stations: map only", "stations: hidden"};
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

// ---- APRS symbols: 24x24 icons (the size the T-Deck uses) baked into the firmware (src/symbols24.bin, made by tools/make_symbols.py from the aprs.fi set) ----
static const int ICON = 24;
extern const uint8_t symbolsBin[] asm("_binary_src_symbols24_bin_start");
// Draw the symbol centred on (cx, cy), alpha-blended over what is already in the frame buffer. Table '/' = primary, '\\' = alternate,
// and an overlay character (0-9, A-Z, or a-j in compressed packets for 0-9) = alternate symbol with that letter drawn on top (e.g. I& = iGate).
// Returns false when the code has no icon or the icon would run off the screen.
static void blendRec(uint16_t *px, const uint8_t *rec) {
  const int n = ICON * ICON;
  for (int k = 0; k < n; k++) {
    uint8_t a = rec[n * 2 + k];
    if (!a) continue;
    uint16_t fg = rec[k * 2] | (rec[k * 2 + 1] << 8), bg = px[k];
    if (a == 255) { px[k] = fg; continue; }
    int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
    int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
    int b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
    px[k] = (r << 11) | (g << 5) | b;
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
    D.setCursor(2, dy + 26); D.printf("rssi %.0f  heard %s", st.rssi, as);
    D.setCursor(2, dy + 38); D.printf("%.4f %.4f", st.lat, st.lon);
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
  // station markers: yellow dot + call sign label (not in the clean-map view)
  D.setTextSize(1);
  if (stView != 2) for (auto &st : stations) {
    if (!st.used) continue;
    double sx, sy; toPx(st.lat, st.lon, z, sx, sy);
    int x = (int)(sx - ox), y = (int)(sy - oy);
    if (x < x0 - 60 || x > W + 60 || y < -10 || y > H + 10) continue;
    bool icon = drawSymbol(D, x, y, st.table, st.sym);
    if (!icon) { D.fillCircle(x, y, 4, TFT_BLACK); D.fillCircle(x, y, 3, TFT_YELLOW); }      // no icon for this code: plain dot
    D.setTextColor(TFT_BLACK, TFT_YELLOW); D.setCursor(x + (icon ? 14 : 6), y - 4); D.print(st.call);
    if (strcmp(st.call, selCall) == 0) D.drawCircle(x, y, icon ? 15 : 8, TFT_WHITE);
  }
  // my own GPS position: blue dot with a white ring
  if (gps.fix.valid) {
    double sx, sy; toPx(gps.fix.lat, gps.fix.lon, z, sx, sy);
    int x = (int)(sx - ox), y = (int)(sy - oy);
    if (x > x0 - 8 && x < W + 8 && y > -8 && y < H + 8) { D.fillCircle(x, y, 6, TFT_WHITE); D.fillCircle(x, y, 4, TFT_BLUE); }
  }
  // crosshair at the map centre
  D.drawFastHLine(xc - 6, H / 2, 13, TFT_RED); D.drawFastVLine(xc, H / 2 - 6, 13, TFT_RED);
  if (stView == 0) drawListPanel(D, H);
  // status at the top of the map, position at the bottom
  D.setTextColor(TFT_WHITE, TFT_BLACK); D.setTextSize(1);
  D.setCursor(x0 + 2, 2);
  if (notice && millis() < noticeUntil) D.printf(" %s                         ", notice);
  else D.printf(" z%d %s  %lums  GPS %s ", z, MODE_NAME[mode], (unsigned long)tTiles, gps.fix.valid ? "fix" : "none");
  D.setCursor(x0 + 2, H - 10);
  D.printf(" %.4f %.4f ", clat, clon);
  if (fbOk) fb.pushSprite(0, 0);
}

void setup() {
  Serial.begin(115200); delay(500);
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  bool xpOk = xp.begin(ADDR_XL9555);
  if (xpOk) {
    const uint8_t on[] = {XP_LORA_EN, XP_GPS_EN, XP_GPS_RST, XP_KB_EN, XP_KB_RST, XP_SD_EN, XP_GPIO_EN};
    for (uint8_t p : on) { xp.output(p, HIGH); delay(2); }
    xp.input(XP_SD_PULLEN);
  }
  for (uint8_t p : {PIN_LORA_CS, PIN_LORA_RST, PIN_SD_CS, PIN_NFC_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  tft.init(); tft.setRotation(PAGER_ROTATION); tft.setBrightness(160); tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.setTextSize(1);
  if (xpOk) haptic.begin(xp);
  fb.setPsram(true); fb.setColorDepth(16); fbOk = fb.createSprite(tft.width(), tft.height()) != nullptr;

  buf = (uint8_t *)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
  bool mounted = SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000);
  if (!buf || !mounted) { tft.setCursor(2, 2); tft.printf("viewer: buffer %s, SD %s", buf ? "ok" : "FAIL", mounted ? "ok" : "FAIL"); return; }

  const char *cands[] = {"/LoRa_Tracker/Maps", "/maps"};
  String base;
  for (auto c : cands) if (SD.exists(c)) { base = c; break; }
  if (base == "") { tft.setCursor(2, 2); tft.print("viewer: no /LoRa_Tracker/Maps on the card"); return; }
  File r = SD.open(base); String region;
  while (true) { File e = r.openNextFile(); if (!e) break; if (e.isDirectory()) { region = e.name(); e.close(); break; } e.close(); }
  r.close();
  if (region == "") { tft.setCursor(2, 2); tft.print("viewer: no region folder"); return; }
  root = base + "/" + region;

  // GPS on Serial1 (38400 baud), keyboard, and the radio (receive only)
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  keys.begin();
  digitalWrite(PIN_LORA_RST, LOW); delay(10); digitalWrite(PIN_LORA_RST, HIGH); delay(50);
  radio = new SX1262(new Module(PIN_LORA_CS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, SPI));
  if (radio->begin(433.775, 125.0, 12, 5, 0x12, 10, 8, 3.0) == RADIOLIB_ERR_NONE) {
    radio->setDio2AsRfSwitch(true); radio->setPacketReceivedAction(onRx); radio->startReceive(); radioOk = true;
  }

  pinMode(PIN_ROT_A, INPUT_PULLUP); pinMode(PIN_ROT_B, INPUT_PULLUP); pinMode(PIN_ROT_PRESS, INPUT_PULLUP);
  kLast = (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B);
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

static void centerOnGps() {
  if (!gps.fix.valid) { haptic.play(H_ERROR); say("No GPS fix yet"); return; }
  clat = gps.fix.lat; clon = gps.fix.lon; haptic.play(H_CLICK); dirty = true;
}

void loop() {
  if (root == "") { delay(1000); return; }

  // GPS characters
  while (Serial1.available()) {
    if (!gps.feed((char)Serial1.read())) continue;
    // only redraw when my dot would move at least a pixel at this zoom, or the fix appeared/vanished
    static bool lastValid = false; static double lastX = 0, lastY = 0;
    double gx = 0, gy = 0; if (gps.fix.valid) toPx(gps.fix.lat, gps.fix.lon, ZOOMS[zi], gx, gy);
    if (gps.fix.valid != lastValid || (gps.fix.valid && (fabs(gx - lastX) >= 1.0 || fabs(gy - lastY) >= 1.0))) {
      lastValid = gps.fix.valid; lastX = gx; lastY = gy; dirty = true;
    }
  }

  // 'S' from the Mac = send a screenshot
  while (Serial.available()) if (Serial.read() == 'S') sendScreenshot();

  // received packets
  if (rxFlag && radioOk) {
    rxFlag = false;
    String f; int st = radio->readData(f);
    if (st == RADIOLIB_ERR_NONE) {
      rxCount++;
      AprsPos pos;
      if (aprsParse((const unsigned char *)f.c_str(), f.length(), pos)) { stationUpdate(pos, radio->getRSSI()); dirty = true; haptic.play(H_ACK); }
    }
    radio->startReceive();
  }

  // keyboard: C centres on my GPS position, L steps the station view (list + map / map only / hidden), S changes the list order
  if (uint8_t e = keys.poll()) {
    if (e & 0x80) {
      char ch = keyLookup(e & 0x7F).ch;
      if (ch == 'p') sendScreenshot();
      else if (ch == 'c') centerOnGps();
      else if (ch == 'l') { stView = (stView + 1) % 3; if (stView != 0 && mode == M_LIST) mode = M_PANX; haptic.play(H_CLICK); say(VIEW_NAME[stView]); }
      else if (ch == 's') { sortByDistance = !sortByDistance; haptic.play(H_TICK); say(sortByDistance ? "list: nearest first" : "list: most recent first"); }
    }
  }

  // knob press: short = next knob mode, long (>= 600 ms) = centre on my GPS position
  static bool wasPressed = false; static uint32_t pressedAt = 0; static bool longDone = false;
  bool pressed = digitalRead(PIN_ROT_PRESS) == LOW;
  if (pressed && !wasPressed) { pressedAt = millis(); longDone = false; }
  if (pressed && !longDone && millis() - pressedAt >= 600) { longDone = true; centerOnGps(); }
  if (!pressed && wasPressed && !longDone) { mode = nextMode(mode); haptic.play(H_TICK); dirty = true; }
  wasPressed = pressed;

  int32_t d; noInterrupts(); d = knobSteps; knobSteps = 0; interrupts();
  if (d) {
    haptic.play(H_TICK);
    if (mode == M_ZOOM) {
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
  // keep the "age" column current: refresh every 15 s while the list is showing
  static uint32_t lastAge = 0;
  if (stView == 0 && millis() - lastAge > 15000) { lastAge = millis(); dirty = true; }
  // redraw at most every 300 ms (stations and GPS arrive in bursts)
  if (dirty && millis() - lastDraw > 300) { dirty = false; lastDraw = millis(); render(); }
  else if (notice && millis() > noticeUntil && !dirty) { notice = nullptr; dirty = true; }
}
