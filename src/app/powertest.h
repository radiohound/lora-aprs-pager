// MEASUREMENT BUILD ONLY (env:powertest, -DPOWER_TEST). Steps through power states on a fixed timetable so that a power analyser's recording
// can be cut into steps afterwards (tools/analyze_powertest.py). Runs once, 60 s after start-up, and ONLY with no USB power. It transmits three
// short status packets under the saved call sign (one at each processor speed). Results are kept in flash; serial command PR prints them.
#pragma once

struct PtStep { const char *name; uint16_t secs; uint8_t mhz; uint8_t kind; };    // kind: 0 light sleep, 1 awake, 2 awake and one transmission
enum { PT_SLEEP, PT_AWAKE, PT_TX };
static const uint8_t PT_GPS = 1, PT_RADIO_OFF = 2;
// KEEP IN STEP WITH tools/analyze_powertest.py
static const struct { PtStep s; uint8_t flags; } PT_PLAN[] = {
  {{"sleep, radio receiving (normal)",   30, 80, PT_SLEEP}, 0},
  {{"sleep, radio asleep",               30, 80, PT_SLEEP}, PT_RADIO_OFF},
  {{"sleep, radio receiving (repeat)",   20, 80, PT_SLEEP}, 0},
  {{"awake 80 MHz, GPS asleep",          20, 80, PT_AWAKE}, 0},
  {{"awake 40 MHz, GPS asleep",          20, 40, PT_AWAKE}, 0},
  {{"awake 20 MHz, GPS asleep",          20, 20, PT_AWAKE}, 0},
  {{"GPS warm-up (not compared)",        20, 80, PT_AWAKE}, PT_GPS},
  {{"GPS on, 80 MHz",                    30, 80, PT_AWAKE}, PT_GPS},
  {{"GPS on, 40 MHz",                    30, 40, PT_AWAKE}, PT_GPS},
  {{"GPS on, 20 MHz",                    30, 20, PT_AWAKE}, PT_GPS},
  {{"GPS on, 80 MHz (repeat)",           30, 80, PT_AWAKE}, PT_GPS},
  {{"transmit at 80 MHz",                20, 80, PT_TX},    0},
  {{"transmit at 40 MHz",                20, 40, PT_TX},    0},
  {{"transmit at 20 MHz",                20, 20, PT_TX},    0},
  {{"sleep, radio receiving (end)",      30, 80, PT_SLEEP}, 0},
};
static const int PT_N = sizeof PT_PLAN / sizeof PT_PLAN[0];
struct PtRes { uint32_t gpsBytes; uint16_t gpsOk, gpsBad, rxOk, rxBad; int16_t txResult; uint16_t txMs; };
struct PtStore { uint16_t ver, done; PtRes r[PT_N]; };
static PtStore ptStore;

static void ptPrint() {
  Preferences p; p.begin("pgr-ptest", true); bool have = p.getBytes("res", &ptStore, sizeof ptStore) == sizeof ptStore && ptStore.ver == 1; p.end();
  if (!have) { Serial.println("power test: no results saved"); return; }
  Serial.printf("power test results (%d steps done of %d)\n", ptStore.done, PT_N);
  for (int i = 0; i < ptStore.done && i < PT_N; i++) {
    const PtRes &r = ptStore.r[i];
    Serial.printf("  %-34s GPS %lu bytes, lines ok %u bad %u | radio packets ok %u bad %u", PT_PLAN[i].s.name, (unsigned long)r.gpsBytes, r.gpsOk, r.gpsBad, r.rxOk, r.rxBad);
    if (PT_PLAN[i].s.kind == PT_TX) Serial.printf(" | tx %s in %u ms", r.txResult == 0 ? "sent" : (r.txResult == 1 ? "channel busy" : (r.txResult == 2 ? "FAILED" : "skipped")), r.txMs);
    Serial.println();
  }
}
// Marker: backlight full for 1 s, off for 1 s, three times: an unmistakable shape in the recording.
static void ptMarker() {
  setCpuFrequencyMhz(80);
  for (int i = 0; i < 3; i++) {
    tft.setBrightness(255); for (int k = 0; k < 100; k++) { delay(10); } esp_task_wdt_reset();
    tft.setBrightness(0);   for (int k = 0; k < 100; k++) { delay(10); } esp_task_wdt_reset();
  }
}
static void ptRun() {
  memset(&ptStore, 0, sizeof ptStore); ptStore.ver = 1;
  lastActivity = millis() - 3600000UL; screenState = 2; powerApply(); tft.setBrightness(0);     // as with the screen off: panel asleep, SD card off
  if (!gpsSleeping) gpsBackup(false);
  setCpuFrequencyMhz(80);
  ptMarker();
  uint32_t t0 = millis(), end = 0; bool gpsOn = false, radioOff = false; static char line[128]; int ln = 0;
  for (int i = 0; i < PT_N; i++) {
    const PtStep &st = PT_PLAN[i].s; uint8_t fl = PT_PLAN[i].flags; PtRes &r = ptStore.r[i];
    uint32_t begin = t0 + end; end += st.secs * 1000UL; uint32_t until = t0 + end;                // fixed timetable: an overrun does not shift later steps
    setCpuFrequencyMhz(80);
    bool wantGps = fl & PT_GPS, wantOff = fl & PT_RADIO_OFF;
    if (wantGps != gpsOn) { if (wantGps) gpsWake(); else gpsBackup(false); gpsOn = wantGps; ln = 0; }
    if (wantOff != radioOff) { if (wantOff) radio->sleep(); else { radio->standby(); rxApply(); } radioOff = wantOff; rxFlag = false; }
    setCpuFrequencyMhz(st.mhz); if (gpsOn) Serial1.updateBaudRate(GPS_BAUD);
    bool sent = false;
    while ((int32_t)(millis() - until) < 0) {
      esp_task_wdt_reset();
      if (st.kind == PT_SLEEP) lightSleepOnce();
      else delay(2);                                                                              // what the normal loop does with the screen off
      if (gpsOn) {
        gpsWakeCheck();
        while (Serial1.available()) {
          char c = (char)Serial1.read(); r.gpsBytes++; gpsWinBytes++; gps.feed(c);
          if (c == '\n') { line[ln] = 0; if (ln > 6 && line[0] == '$') { if (NmeaParser::checksumOk(line)) r.gpsOk++; else r.gpsBad++; } else if (ln > 6) r.gpsBad++; ln = 0; }
          else if (c != '\r' && ln < (int)sizeof line - 1) line[ln++] = c;
        }
      }
      if (rxFlag && !radioOff) { rxFlag = false; String f; int s = radio->readData(f); if (s == RADIOLIB_ERR_NONE) r.rxOk++; else r.rxBad++; rxApply(); }
      if (st.kind == PT_TX && !sent && millis() - begin > 6000) {
        sent = true; r.txResult = -1;
        if (strcmp(S.call, "N0CALL") != 0) {
          char info[64]; snprintf(info, sizeof info, ">pager power test, CPU %u MHz", st.mhz);
          std::string t = msg::tnc2(S.call, TOCALL, S.path, info);
          unsigned char fr[200]; fr[0] = 0x3C; fr[1] = 0xFF; fr[2] = 0x01; memcpy(fr + 3, t.data(), t.size());
          uint32_t a = millis(); r.txResult = (int16_t)txFrame(fr, t.size() + 3); r.txMs = (uint16_t)std::min<uint32_t>(millis() - a, 65535);
        }
      }
    }
    ptStore.done = i + 1;
  }
  setCpuFrequencyMhz(80);
  if (radioOff) { radio->standby(); rxApply(); }
  ptMarker();
  { Preferences p; p.begin("pgr-ptest", false); p.putBytes("res", &ptStore, sizeof ptStore); p.end(); }
  setCpuFrequencyMhz(240); screenTouch(); say("Power test finished. Stop the recording."); render();
}
static void powerTestTick() {                                // every pass of the normal loop
  static bool done = false;
  if (done || millis() < 60000UL) return;
  static uint32_t next = 0; if (millis() < next) return; next = millis() + 2000;
  if (usbPresent() || !radioOk) return;                      // never with USB power: plug in afterwards to read the results with PR
  if (!gps.fix.valid && millis() < 180000UL) return;         // give the GPS up to 3 minutes for a first fix, so the GPS steps are alike
  done = true; ptRun();
}
