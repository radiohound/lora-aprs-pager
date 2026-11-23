// Milestone 1: board bring-up for the LilyGO T-LoRa Pager (433 MHz).
// Powers the peripherals, scans I2C, draws a test screen, probes which LoRa chip is
// fitted (SX1262 or LR1121), listens on 433.775 MHz (RX only, NEVER transmits), shows
// GPS NMEA status and the battery gauge. Everything is also printed on USB serial.
#include <Arduino.h>
#include <Wire.h>
#include "haptic.h"
#include "inputs.h"
#include "keymap.h"
#include <SPI.h>
#include <SD.h>
#include <RadioLib.h>
#include "pins.h"
#include "xl9555.h"
#include "display.h"

static XL9555 xp;
static Haptic haptic;
static Knob knob;
static Keys keys;
static int lastKey = 0; static bool lastKeyDown = false;
static uint8_t keyHist[10]; static uint8_t keyHistN = 0;   // last raw codes of key PRESSES, newest last
static PagerDisplay tft;

// ---- LoRa APRS parameters (CA2RXU / lora-aprs.org standard) ----
static const float  LORA_FREQ = 433.775;
static const float  LORA_BW   = 125.0;
static const uint8_t LORA_SF  = 12;
static const uint8_t LORA_CR  = 5;      // 4/5
static const uint8_t LORA_SYNC = 0x12;   // LoRa APRS uses the PRIVATE sync word 0x12 (CA2RXU/moricef firmware: RADIOLIB_SX126X_SYNC_WORD_PRIVATE), NOT LoRaWAN's 0x34
static const uint16_t LORA_PREAMBLE = 8;

static Module *mod = nullptr;
static SX1262 *sx = nullptr;
static LR1121 *lr = nullptr;
static const char *radioName = "none";
static volatile bool rxFlag = false;
static void IRAM_ATTR onRx() { rxFlag = true; }

// LR1121 RF switch (same table LilyGoLib and Meshtastic use for the LR1121 variant)
static const uint32_t lrDioPins[] = {RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};
static const Module::RfSwitchMode_t lrTable[] = {
  {LR11x0::MODE_STBY, {LOW, LOW}}, {LR11x0::MODE_RX, {LOW, HIGH}},
  {LR11x0::MODE_TX, {HIGH, LOW}},  {LR11x0::MODE_TX_HP, {HIGH, LOW}},
  {LR11x0::MODE_TX_HF, {LOW, LOW}}, {LR11x0::MODE_GNSS, {LOW, LOW}},
  {LR11x0::MODE_WIFI, {LOW, LOW}}, END_OF_MODE_TABLE};

static int line = 0;   // say() uses lines 0-14 (y up to 182) so the key history (y 194) and status (y 208) lines stay free
static void say(const char *fmt, ...) {
  char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  tft.setCursor(2, 2 + line * 12);
  tft.fillRect(0, 2 + line * 12, 480, 12, TFT_BLACK);
  tft.print(b);
  line = (line + 1) % 15;
}

static void i2cScan() {
  struct { uint8_t a; const char *n; } known[] = {
    {ADDR_XL9555, "XL9555 expander"}, {ADDR_TCA8418, "TCA8418 keyboard"}, {ADDR_BQ25896, "BQ25896 charger"},
    {ADDR_BQ27220, "BQ27220 gauge"}, {ADDR_DRV2605, "DRV2605 haptic"}, {ADDR_ES8311, "ES8311 codec"},
    {ADDR_PCF85063, "PCF85063 RTC"}};
  String found;
  for (auto &k : known) {
    Wire.beginTransmission(k.a);
    bool ok = Wire.endTransmission() == 0;
    Serial.printf("I2C 0x%02X %-18s %s\n", k.a, k.n, ok ? "found" : "MISSING");
    if (ok) found += String(k.a, HEX) + " ";
  }
  say("I2C found: %s", found.c_str());
}

static uint16_t gaugeWord(uint8_t cmd) {   // BQ27220 standard command, little endian
  Wire.beginTransmission(ADDR_BQ27220); Wire.write(cmd); Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ADDR_BQ27220, (uint8_t)2);
  uint16_t lo = Wire.read(), hi = Wire.read();
  return lo | (hi << 8);
}

static bool radioInit() {
  pinMode(PIN_LORA_CS, OUTPUT); digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_LORA_RST, OUTPUT); digitalWrite(PIN_LORA_RST, LOW); delay(10);
  digitalWrite(PIN_LORA_RST, HIGH); delay(50);
  mod = new Module(PIN_LORA_CS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, SPI);
  // Try SX1262 first (what LilyGO's docs list for the 433 board)
  sx = new SX1262(mod);
  int st = sx->begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, 10, LORA_PREAMBLE, 3.0);
  if (st == RADIOLIB_ERR_NONE) {
    sx->setDio2AsRfSwitch(true);
    sx->setPacketReceivedAction(onRx);
    sx->startReceive();
    radioName = "SX1262";
    return true;
  }
  say("SX1262 begin failed: %d, trying LR1121", st);
  delete sx; sx = nullptr; delete mod;
  digitalWrite(PIN_LORA_RST, LOW); delay(10); digitalWrite(PIN_LORA_RST, HIGH); delay(50);
  mod = new Module(PIN_LORA_CS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, SPI);
  lr = new LR1121(mod);
  st = lr->begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, 10, LORA_PREAMBLE, 3.0);
  if (st == RADIOLIB_ERR_NONE) {
    lr->setRfSwitchTable(lrDioPins, lrTable);
    lr->setPacketReceivedAction(onRx);
    lr->startReceive();
    radioName = "LR1121";
    return true;
  }
  say("LR1121 begin failed: %d", st);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== T-LoRa Pager bring-up (milestone 1) ===");

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  // 1. Expander first: it switches power to LoRa, GPS, keyboard, SD.
  bool xpOk = xp.begin(ADDR_XL9555);
  Serial.printf("XL9555: %s\n", xpOk ? "ok" : "NOT FOUND");
  if (xpOk) {
    const uint8_t on[] = {XP_LORA_EN, XP_GPS_EN, XP_GPS_RST, XP_KB_EN, XP_KB_RST, XP_SD_EN, XP_GPIO_EN};
    for (uint8_t p : on) { xp.output(p, HIGH); delay(2); }
    xp.input(XP_SD_PULLEN);
    // NFC power (XP_NFC_EN) stays off on purpose: no NFC use, saves power; its chip select is parked high below.
    // Haptic driver (XP_DRV_EN) is switched on by haptic.begin(). SD detect is only read, so it is left as the input it powers up as.
  }

  // 2. All shared-SPI chip selects high (and LoRa reset released) before anything talks on the bus.
  for (uint8_t p : {PIN_LORA_CS, PIN_LORA_RST, PIN_SD_CS, PIN_NFC_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  // 3. Display
  tft.init();
  tft.setRotation(PAGER_ROTATION);
  tft.setBrightness(160);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  // corner markers show whether the rotation/offset is right
  tft.setTextDatum(top_left);     tft.drawString("TL", 0, 0);
  tft.setTextDatum(top_right);    tft.drawString("TR", tft.width() - 1, 0);
  tft.setTextDatum(bottom_left);  tft.drawString("BL", 0, tft.height() - 1);
  tft.setTextDatum(bottom_right); tft.drawString("BR", tft.width() - 1, tft.height() - 1);
  tft.setTextDatum(top_left);
  tft.drawRect(0, 0, tft.width(), tft.height(), TFT_DARKGREY);
  line = 2;
  say("Display %dx%d rot %d", tft.width(), tft.height(), PAGER_ROTATION);
  i2cScan();

  // 4. Battery gauge (BQ27220 standard commands: 0x08 voltage mV, 0x2C state of charge %)
  say("Battery %u mV  %u %%", gaugeWord(0x08), gaugeWord(0x2C));

  // 4b. Haptic: one click at boot so you can feel that it works
  if (xpOk) { bool h = haptic.begin(xp); say("Haptic DRV2605: %s", h ? "ok" : "not found"); if (h) haptic.play(H_CLICK); }

  // 4c. Knob and keyboard
  knob.begin();
  say("Keyboard TCA8418: %s", keys.begin() ? "ok" : "not found");

  // 5. SD card (read only: just mount and report size)
  if (SD.begin(PIN_SD_CS, SPI, 4000000)) {
    static const char *types[] = {"none", "MMC", "SD", "SDHC", "?"};
    uint8_t t = SD.cardType();
    say("SD ok %s %llu MB", types[t > 4 ? 4 : t], SD.cardSize() / (1024 * 1024));
  } else say("SD fail: detect pin=%d (try FAT32 card)", xpOk ? xp.read(XP_SD_DET) : -1);

  // 6. GPS
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // 7. Radio
  if (radioInit()) say("Radio %s listening %.3f MHz SF%d", radioName, LORA_FREQ, LORA_SF);
}

static String nmea;
static uint32_t nmeaLines = 0, lastStat = 0;
static String lastGga;

static const char *keyName(int raw) {
  static char b[2] = {0, 0};
  KeyInfo k = keyLookup(raw);
  switch (k.mod) {
    case KM_ENTER: return "Enter"; case KM_ALT: return "Alt"; case KM_SHIFT: return "Shift";
    case KM_BACKSPACE: return "Bksp"; case KM_SPACE: return "Space"; default: break;
  }
  b[0] = k.ch ? k.ch : '?';
  return b;
}

static void statusLine() {
  static uint32_t t = 0;
  if (millis() - t < 100) return;
  t = millis();
  char b[100];
  snprintf(b, sizeof b, "knob %ld %s  key %d '%s' %s  gps %lu", (long)knob.pos, knob.pressed ? "PRESSED" : "-",
           lastKey, keyName(lastKey), lastKeyDown ? "down" : "up", (unsigned long)nmeaLines);
  tft.fillRect(0, 208, 480, 12, TFT_BLACK);
  tft.setCursor(2, 208);
  tft.print(b);
  // history of the last key presses (raw codes), for mapping the keyboard
  char h[100]; int n = snprintf(h, sizeof h, "keys:");
  for (uint8_t i = 0; i < keyHistN && n < 90; i++) n += snprintf(h + n, sizeof h - n, " %u", keyHist[i]);
  tft.fillRect(0, 194, 480, 12, TFT_BLACK);
  tft.setCursor(2, 194);
  tft.print(h);
}

void loop() {
  if (int d = knob.poll()) haptic.play(H_TICK);
  static bool wasPressed = false;
  if (knob.pressed != wasPressed) { wasPressed = knob.pressed; if (wasPressed) haptic.play(H_CLICK); }
  if (uint8_t e = keys.poll()) {
    lastKey = e & 0x7F; lastKeyDown = e & 0x80;
    if (lastKeyDown) {
      haptic.play(H_TICK);
      if (keyHistN == sizeof keyHist) { memmove(keyHist, keyHist + 1, sizeof keyHist - 1); keyHistN--; }
      keyHist[keyHistN++] = lastKey;
    }
  }
  statusLine();
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      nmeaLines++;
      if (nmea.startsWith("$GNGGA") || nmea.startsWith("$GPGGA")) lastGga = nmea;
      nmea = "";
    } else if (c != '\r' && nmea.length() < 120) nmea += c;
  }

  if (rxFlag) {
    rxFlag = false;
    String s; int st = sx ? sx->readData(s) : lr->readData(s);
    float rssi = sx ? sx->getRSSI() : lr->getRSSI();
    float snr  = sx ? sx->getSNR()  : lr->getSNR();
    if (st == RADIOLIB_ERR_NONE) {
      Serial.printf("RX rssi %.0f snr %.1f len %u: ", rssi, snr, (unsigned)s.length());
      for (char c : s) Serial.print(c >= 32 && c < 127 ? c : '.');
      Serial.println();
      say("RX %.0f dBm: %.60s", rssi, s.c_str());
    } else Serial.printf("RX error %d\n", st);
    if (sx) sx->startReceive(); else lr->startReceive();
  }

  if (millis() - lastStat > 5000) {
    lastStat = millis();
    Serial.printf("GPS lines=%lu  %s\n", (unsigned long)nmeaLines, lastGga.c_str());
    Serial.printf("Batt %u mV %u %%\n", gaugeWord(0x08), gaugeWord(0x2C));
  }
}
