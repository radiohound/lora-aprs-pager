// MEASUREMENT BUILD ONLY (env:leaktest, -DLEAK_TEST). Where does the sleep current go? Radio and GPS asleep, processor in light sleep, one
// change at a time, compared in a power analyser's recording (tools/analyze_leaktest.py). Runs once, 60 s after start-up, ONLY without USB
// power. After the end marker it deep-sleeps for 30 s (a reference with the processor's pins released), wakes, and says it has finished.
//
// ROUND 2: the other circuits. (Round 1, the SD card and shared SPI lines, is in git history, commit 1e025d6. Result: the card powered and
// idle costs about 0.4 mA; with it off, floating or lowering its lines changes nothing; SCK+MOSI high with it off costs 16 mA.)
// From LilyGO's schematic (V1.0 25-06-13):
//  - NFC chip (sheet 4): its supply is switched by RF_EN (off by default), but this firmware drives its chip-select (IO39) high from start-up,
//    into an unpowered chip.
//  - Audio codec ES8311 (sheet 5): on the always-on 3.3 V; this firmware never talks to it. Sleep sequence: LilyGoLib es8311_suspend().
//  - Haptic DRV2605 (sheet 3): on the always-on 3.3 V; enabled (expander M_EN) at start-up and left so.
//  - Speaker amplifier (sheet 5): control pulled down (off) by a resistor. Keyboard light (IO46) drives a transistor. Screen backlight IO42.
//  - GPS (sheet 5): switched supply; normally kept powered, in backup. Charger BQ25896: may keep its converter running.
//  - The knob (sheet 5) has 10 k pull-ups on its three contacts: about 0.33 mA per closed contact, which software cannot change.
//
// Every step shows its own number with the backlight: a long flash counts 5, a short flash counts 1.
#pragma once
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_sleep.h>

enum { LTA_NONE, LTA_NFC_FLOAT, LTA_NFC_LOW, LTA_NFC_ON, LTA_HAPTIC_OFF, LTA_KBLED_LOW, LTA_KB_OFF, LTA_BL_LOW, LTA_AMP_LOW, LTA_HDR_LOW, LTA_GPS_OFF,
       LTA_MOTION_OFF, LTA_CHG_ADC_OFF, LTA_CODEC_SLEEP, LTA_COMBINED };
struct LtStep { const char *name; uint8_t action; };
// tools/analyze_leaktest.py reads this table from this file
static const LtStep LT_PLAN[] = {
  {"BASE (as screen off today)",                       LTA_NONE},
  {"NFC chip-select floating",                         LTA_NFC_FLOAT},
  {"NFC chip-select driven low",                       LTA_NFC_LOW},
  {"NFC powered, chip-select high",                    LTA_NFC_ON},
  {"BASE repeat",                                      LTA_NONE},
  {"haptic driver disabled",                           LTA_HAPTIC_OFF},
  {"keyboard light pin held low",                      LTA_KBLED_LOW},
  {"keyboard power off, its lines released",           LTA_KB_OFF},
  {"screen backlight pin held low",                    LTA_BL_LOW},
  {"speaker amplifier control held low",               LTA_AMP_LOW},
  {"expansion header control held low",                LTA_HDR_LOW},
  {"BASE repeat",                                      LTA_NONE},
  {"GPS power off, its lines released",                LTA_GPS_OFF},
  {"BASE repeat (GPS powered, back in backup)",        LTA_NONE},
  {"motion sensor detectors off",                      LTA_MOTION_OFF},
  {"charger converter off",                            LTA_CHG_ADC_OFF},
  {"BASE repeat",                                      LTA_NONE},
  {"audio chip put to sleep (stays so)",               LTA_CODEC_SLEEP},
  {"BASE after audio chip sleep",                      LTA_NONE},
  {"combined: audio asleep, haptic off, NFC CS float", LTA_COMBINED},
};
static const int LT_N = sizeof LT_PLAN / sizeof LT_PLAN[0];
static const uint32_t LT_STEP_S = 28;                    // each step, including its number flashes and any set-up at the start
#define LT_NOTE 64

static uint32_t ltBlRoute = 0;
static void ltPinHold(int pin, int how) {                 // 0 float, 1 low, 2 high (weak drive)
  gpio_num_t p = (gpio_num_t)pin;
  esp_rom_gpio_connect_out_signal(p, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_pull_mode(p, GPIO_FLOATING);
  if (how == 0) gpio_set_direction(p, GPIO_MODE_INPUT);
  else { gpio_set_level(p, how == 2); gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT); gpio_set_drive_capability(p, GPIO_DRIVE_CAP_0); }
}
static bool ltI2cWrite(uint8_t addr, uint8_t reg, uint8_t v) { Wire.beginTransmission(addr); Wire.write(reg); Wire.write(v); return Wire.endTransmission() == 0; }
// LilyGoLib bsp_codec/device/es8311/es8311.c, es8311_suspend(), same registers in the same order. Returns how many writes were not answered.
static int ltCodecSleep() {
  static const uint8_t SEQ[][2] = {{0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02}, {0x14, 0x00}, {0x0D, 0xFA}, {0x15, 0x00}, {0x02, 0x10},
                                   {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00}, {0x45, 0x00}, {0x0D, 0xFC}, {0x02, 0x00}};
  int bad = 0; for (auto &w : SEQ) if (!ltI2cWrite(ADDR_ES8311, w[0], w[1])) bad++;
  return bad;
}
// Apply (on = true) or undo one change. `note` receives what could be checked.
static void ltDo(uint8_t a, bool on, char *note) {
  switch (a) {
    case LTA_NFC_FLOAT: if (on) ltPinHold(PIN_NFC_CS, 0); else ltPinHold(PIN_NFC_CS, 2); break;
    case LTA_NFC_LOW:   if (on) ltPinHold(PIN_NFC_CS, 1); else ltPinHold(PIN_NFC_CS, 2); break;
    case LTA_NFC_ON:    if (on) xp.output(XP_NFC_EN, HIGH); else xp.input(XP_NFC_EN); break;
    case LTA_HAPTIC_OFF: xp.output(XP_DRV_EN, on ? LOW : HIGH); break;
    case LTA_KBLED_LOW: if (on) ltPinHold(PIN_KB_BL, 1); else ltPinHold(PIN_KB_BL, 0); break;
    case LTA_KB_OFF:
      if (on) { ltPinHold(PIN_KB_BL, 1); ltPinHold(PIN_KB_INT, 0); xp.output(XP_KB_RST, LOW); xp.output(XP_KB_EN, LOW); }
      else { xp.output(XP_KB_EN, HIGH); delay(5); xp.output(XP_KB_RST, HIGH); ltPinHold(PIN_KB_BL, 0); gpio_set_direction((gpio_num_t)PIN_KB_INT, GPIO_MODE_INPUT); }
      break;
    case LTA_BL_LOW:    if (on) { ltBlRoute = GPIO.func_out_sel_cfg[PIN_DISP_BL].val; ltPinHold(PIN_DISP_BL, 1); } else GPIO.func_out_sel_cfg[PIN_DISP_BL].val = ltBlRoute; break;
    case LTA_AMP_LOW:   if (on) xp.output(XP_AMP_EN, LOW); else xp.input(XP_AMP_EN); break;
    case LTA_HDR_LOW:   if (on) xp.output(XP_GPIO_EN, LOW); else xp.input(XP_GPIO_EN); break;
    case LTA_GPS_OFF:
      if (on) { ltPinHold(PIN_GPS_RX, 0); ltPinHold(PIN_GPS_TX, 0); ltPinHold(PIN_GPS_PPS, 0); xp.output(XP_GPS_RST, LOW); xp.output(XP_GPS_EN, LOW); }
      else { xp.output(XP_GPS_EN, HIGH); delay(5); xp.output(XP_GPS_RST, HIGH); delay(1200); gpsWake(); delay(300); gpsBackup(false); }   // powered again, then back to sleep
      break;
    case LTA_MOTION_OFF:
      if (!motionOk) { if (note) strlcpy(note, "motion sensor not running: nothing changed", LT_NOTE); break; }
      for (int k = 0; k < MOT_KINDS; k++) if (motionAvail[k]) motionChip.configure(MOT_ID[k], on ? 0 : 1, 0);
      break;
    case LTA_CHG_ADC_OFF: {
      static int was = -1;
      if (on) { was = bqRead(0x02); if (was >= 0) { bqWrite(0x02, (uint8_t)(was & ~0xC0)); if (note) snprintf(note, LT_NOTE, "charger reg 02 was 0x%02x, now 0x%02x", was, bqRead(0x02)); } else if (note) strlcpy(note, "charger did not answer", LT_NOTE); }
      else if (was >= 0) bqWrite(0x02, (uint8_t)was);
    } break;
    case LTA_CODEC_SLEEP: if (on) { int bad = ltCodecSleep(); if (note) snprintf(note, LT_NOTE, "audio chip: %d of 15 writes not answered", bad); } break;     // stays asleep
    case LTA_COMBINED: if (on) { ltCodecSleep(); xp.output(XP_DRV_EN, LOW); ltPinHold(PIN_NFC_CS, 0); } else { xp.output(XP_DRV_EN, HIGH); ltPinHold(PIN_NFC_CS, 2); } break;
    default: break;
  }
}
static void ltFlash(uint32_t onMs) { tft.setBrightness(255); for (uint32_t k = 0; k < onMs / 10; k++) delay(10); tft.setBrightness(0); delay(200); esp_task_wdt_reset(); }
static void ltNumber(int n) { for (int i = 0; i < n / 5; i++) ltFlash(600); for (int i = 0; i < n % 5; i++) ltFlash(200); }
static void ltMarker() { for (int i = 0; i < 3; i++) { tft.setBrightness(255); for (int k = 0; k < 100; k++) delay(10); tft.setBrightness(0); for (int k = 0; k < 100; k++) delay(10); esp_task_wdt_reset(); } }
static char ltNotes[LT_N][LT_NOTE];

static void ltRun() {
  lastActivity = millis() - 3600000UL; screenState = 2; powerApply(); tft.setBrightness(0);      // screen-off state: panel asleep, SD card off
  if (!gpsSleeping) gpsBackup(false);
  radio->sleep(); rxFlag = false;
  setCpuFrequencyMhz(80);
  for (int pin : {PIN_NFC_CS, PIN_KB_BL, PIN_KB_INT, PIN_DISP_BL, PIN_GPS_RX, PIN_GPS_TX, PIN_GPS_PPS}) gpio_sleep_sel_dis((gpio_num_t)pin);
  memset(ltNotes, 0, sizeof ltNotes);
  ltMarker();
  uint32_t t0 = millis(); uint8_t active = LTA_NONE;
  for (int i = 0; i < LT_N; i++) {
    uint32_t until = t0 + (uint32_t)(i + 1) * LT_STEP_S * 1000UL;
    ltNumber(i + 1);                                                            // "this is step i+1" (first, so it is always on time)
    if (active != LTA_NONE) ltDo(active, false, nullptr);                         // undo the previous change, then make this one
    esp_task_wdt_reset();
    active = LT_PLAN[i].action;
    if (active != LTA_NONE) ltDo(active, true, ltNotes[i]);
    while ((int32_t)(millis() - until) < 0) { esp_task_wdt_reset(); lightSleepOnce(); rxFlag = false; }
  }
  if (active != LTA_NONE) ltDo(active, false, nullptr);
  ltMarker();
  { Preferences p; p.begin("pgr-leak", false); p.putBytes("lv2", ltNotes, sizeof ltNotes); p.putUChar("done2", LT_N); p.end(); }
  radio->sleep();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
  esp_deep_sleep_start();                                                       // reference; wakes as a restart
}
static void ltPrint() {
  static char n[LT_N][LT_NOTE]; Preferences p; p.begin("pgr-leak", true); bool have = p.getBytes("lv2", n, sizeof n) == sizeof n; p.end();
  if (!have) { Serial.println("leak test: no results saved"); return; }
  for (int i = 0; i < LT_N; i++) Serial.printf("  step %2d  %-52s %s\n", i + 1, LT_PLAN[i].name, n[i]);
}
static void leakTestTick() {
  static bool done = false;
  if (done || millis() < 60000UL) return;
  static uint32_t next = 0; if (millis() < next) return; next = millis() + 2000;
  if (usbPresent() || !radioOk) return;
  Preferences p; p.begin("pgr-leak", true); bool ran = p.getUChar("done2", 0) != 0; p.end();
  done = true;
  if (ran && (int)esp_reset_reason() == ESP_RST_DEEPSLEEP) { screenTouch(); say("Leak test finished. Stop the recording."); return; }
  ltRun();
}
