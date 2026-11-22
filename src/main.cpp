// Milestone 1: board bring-up for the LilyGO T-LoRa Pager (433 MHz).
// Powers the peripherals, scans I2C, draws a test screen, probes which LoRa chip is
// fitted (SX1262 or LR1121), listens on 433.775 MHz (RX only, NEVER transmits), shows
// GPS NMEA status and the battery gauge. Everything is also printed on USB serial.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <RadioLib.h>
#include "pins.h"
#include "xl9555.h"
#include "display.h"

static XL9555 xp;
static PagerDisplay tft;

// ---- LoRa APRS parameters (CA2RXU / lora-aprs.org standard) ----
static const float  LORA_FREQ = 433.775;
static const float  LORA_BW   = 125.0;
static const uint8_t LORA_SF  = 12;
static const uint8_t LORA_CR  = 5;      // 4/5
static const uint8_t LORA_SYNC = 0x34;
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

static int line = 0;
static void say(const char *fmt, ...) {
  char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  tft.setCursor(2, 2 + line * 12);
  tft.fillRect(0, 2 + line * 12, 480, 12, TFT_BLACK);
  tft.print(b);
  line = (line + 1) % 17;
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
    xp.input(XP_SD_DET);
  }

  // 2. All shared-SPI chip selects high before anything talks on the bus.
  for (uint8_t p : {PIN_LORA_CS, PIN_SD_CS, PIN_NFC_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
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

  // 5. SD card (read only: just mount and report size)
  if (SD.begin(PIN_SD_CS, SPI, 4000000)) say("SD ok %llu MB", SD.cardSize() / (1024 * 1024));
  else say("SD: no card or mount failed (detect pin=%d)", xpOk ? xp.read(XP_SD_DET) : -1);

  // 6. GPS
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // 7. Radio
  if (radioInit()) say("Radio %s listening %.3f MHz SF%d", radioName, LORA_FREQ, LORA_SF);
}

static String nmea;
static uint32_t nmeaLines = 0, lastStat = 0;
static String lastGga;

void loop() {
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
