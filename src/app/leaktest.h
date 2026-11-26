// MEASUREMENT BUILD ONLY (env:leaktest, -DLEAK_TEST). Where does the sleep current go? First round: the SD card and the SPI lines it shares
// with the radio and the display. Radio and GPS asleep, processor in light sleep, one change at a time, compared in a power analyser's
// recording (tools/analyze_leaktest.py). Runs once, 60 s after start-up, ONLY without USB power, then restarts the pager.
// From LilyGO's schematic (V1.0 25-06-13, sheet 2): 10 k pull-ups on MOSI (R6), SCK (R5) and MISO (R7) go to SPI_PULLUP_EN (expander P13;
// R61 to the card's supply is not fitted); the card's chip-select has no pull-up.
// Safety: MISO is never driven from here (the card, the radio or the display may drive it); other lines are driven at the weakest strength.
//
// Every step shows its own number with the backlight: a long flash counts 5, a short flash counts 1.
#pragma once
#include <driver/gpio.h>
#include <esp_rom_gpio.h>

// how a line is held: KEEP = as the normal firmware leaves it with the screen off, FLOAT = input, DOWN = input with pull-down, LO/HI = driven
enum { L_KEEP, L_FLOAT, L_DOWN, L_LO, L_HI };
enum { PU_INPUT, PU_LO, PU_HI };                         // SPI_PULLUP_EN: input (what the firmware does), driven low, driven high
struct LtStep { const char *name; uint8_t card, cs, sck, mosi, miso, pu; };                    // card: 1 = powered
// tools/analyze_leaktest.py reads this table from this file
static const LtStep LT_PLAN[] = {
  {"BASE (as screen off today)",                 0, L_KEEP,  L_KEEP,  L_KEEP,  L_KEEP,  PU_INPUT},
  {"card POWERED, CS high",                      1, L_HI,    L_KEEP,  L_KEEP,  L_KEEP,  PU_INPUT},
  {"card POWERED, CS float",                     1, L_FLOAT, L_KEEP,  L_KEEP,  L_KEEP,  PU_INPUT},
  {"BASE repeat",                                0, L_KEEP,  L_KEEP,  L_KEEP,  L_KEEP,  PU_INPUT},
  {"pull-ups driven LOW",                        0, L_KEEP,  L_KEEP,  L_KEEP,  L_KEEP,  PU_LO},
  {"pull-ups driven HIGH",                       0, L_KEEP,  L_KEEP,  L_KEEP,  L_KEEP,  PU_HI},
  {"BASE repeat",                                0, L_KEEP,  L_KEEP,  L_KEEP,  L_KEEP,  PU_INPUT},
  {"SCK MOSI driven low",                        0, L_KEEP,  L_LO,    L_LO,    L_KEEP,  PU_INPUT},
  {"SCK MOSI float",                             0, L_KEEP,  L_FLOAT, L_FLOAT, L_KEEP,  PU_INPUT},
  {"SCK MOSI low, MISO pull-down",               0, L_KEEP,  L_LO,    L_LO,    L_DOWN,  PU_INPUT},
  {"SCK MOSI low, MISO pull-down, pull-ups LOW", 0, L_KEEP,  L_LO,    L_LO,    L_DOWN,  PU_LO},
  {"all float, pull-ups LOW",                    0, L_FLOAT, L_FLOAT, L_FLOAT, L_FLOAT, PU_LO},
  {"all low incl CS, MISO pull-down",            0, L_LO,    L_LO,    L_LO,    L_DOWN,  PU_LO},
  {"SCK MOSI driven HIGH (card off)",            0, L_KEEP,  L_HI,    L_HI,    L_KEEP,  PU_INPUT},
  {"CS driven HIGH (card off)",                  0, L_HI,    L_LO,    L_LO,    L_DOWN,  PU_INPUT},
  {"card POWERED, CS high, SCK MOSI low",        1, L_HI,    L_LO,    L_LO,    L_KEEP,  PU_INPUT},
  {"card POWERED, CS high, pull-ups HIGH",       1, L_HI,    L_KEEP,  L_KEEP,  L_KEEP,  PU_HI},
};
static const int LT_N = sizeof LT_PLAN / sizeof LT_PLAN[0];
static const uint32_t LT_STEP_S = 25;                    // each step, including its number flashes at the start

struct LtLine { int pin; uint32_t route; };
static LtLine ltLines[4] = {{PIN_SD_CS, 0}, {PIN_SCK, 0}, {PIN_MOSI, 0}, {PIN_MISO, 0}};
static void ltHold(int i, uint8_t how) {
  gpio_num_t p = (gpio_num_t)ltLines[i].pin;
  if (how == L_KEEP) return;
  if (i == 3 && (how == L_LO || how == L_HI)) return;                          // never drive MISO
  esp_rom_gpio_connect_out_signal(p, SIG_GPIO_OUT_IDX, false, false);         // take the pin away from the SPI peripheral
  gpio_set_pull_mode(p, how == L_DOWN ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
  if (how == L_LO || how == L_HI) { gpio_set_level(p, how == L_HI); gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT); gpio_set_drive_capability(p, GPIO_DRIVE_CAP_0); }
  else gpio_set_direction(p, GPIO_MODE_INPUT);
}
// Back to how the normal firmware has the lines with the screen off: SCK and MOSI on the SPI peripheral, MISO an input, CS floating.
static void ltNormalLines() {
  for (int i = 1; i <= 2; i++) { gpio_num_t p = (gpio_num_t)ltLines[i].pin; gpio_set_pull_mode(p, GPIO_FLOATING); gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT); GPIO.func_out_sel_cfg[p].val = ltLines[i].route; }
  gpio_set_pull_mode((gpio_num_t)PIN_MISO, GPIO_FLOATING); gpio_set_direction((gpio_num_t)PIN_MISO, GPIO_MODE_INPUT);
  pinMode(PIN_SD_CS, INPUT);
}
static void ltFlash(uint32_t onMs) {
  tft.setBrightness(255); for (uint32_t k = 0; k < onMs / 10; k++) delay(10);
  tft.setBrightness(0); delay(200); esp_task_wdt_reset();
}
static void ltNumber(int n) { for (int i = 0; i < n / 5; i++) ltFlash(600); for (int i = 0; i < n % 5; i++) ltFlash(200); }
static void ltMarker() { for (int i = 0; i < 3; i++) { tft.setBrightness(255); for (int k = 0; k < 100; k++) delay(10); tft.setBrightness(0); for (int k = 0; k < 100; k++) delay(10); esp_task_wdt_reset(); } }

static void ltRun() {
  lastActivity = millis() - 3600000UL; screenState = 2; powerApply(); tft.setBrightness(0);      // screen-off state: panel asleep, SD card off
  if (!gpsSleeping) gpsBackup(false);
  radio->sleep(); rxFlag = false;
  setCpuFrequencyMhz(80);
  for (auto &l : ltLines) { gpio_sleep_sel_dis((gpio_num_t)l.pin); l.route = GPIO.func_out_sel_cfg[l.pin].val; }   // light sleep must leave these pins as set
  ltMarker();
  uint32_t t0 = millis();
  static char levels[LT_N][64]; bool cardStarted[LT_N] = {false};
  for (int i = 0; i < LT_N; i++) {
    const LtStep &s = LT_PLAN[i]; uint32_t until = t0 + (uint32_t)(i + 1) * LT_STEP_S * 1000UL;
    // always from the same starting point: lines normal, card off
    ltNormalLines(); xp.input(XP_SD_PULLEN); xp.output(XP_SD_EN, LOW); delay(20);
    if (s.card) {                                        // powered the way a running pager has it: started, then closed, so it sits idle
      pinMode(PIN_SD_CS, OUTPUT); digitalWrite(PIN_SD_CS, HIGH); xp.output(XP_SD_EN, HIGH); delay(150);
      bool ok = SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000); if (ok) SD.end();
      cardStarted[i] = ok; pinMode(PIN_SD_CS, INPUT); esp_task_wdt_reset();
    }
    if (s.pu == PU_LO) xp.output(XP_SD_PULLEN, LOW); else if (s.pu == PU_HI) xp.output(XP_SD_PULLEN, HIGH);
    ltHold(0, s.cs); ltHold(1, s.sck); ltHold(2, s.mosi); ltHold(3, s.miso);
    ltNumber(i + 1);                                                            // "this is step i+1"
    delay(100);
    snprintf(levels[i], sizeof levels[i], "CS%d SCK%d MOSI%d MISO%d PU%d%s", gpio_get_level((gpio_num_t)PIN_SD_CS), gpio_get_level((gpio_num_t)PIN_SCK),
             gpio_get_level((gpio_num_t)PIN_MOSI), gpio_get_level((gpio_num_t)PIN_MISO), xp.read(XP_SD_PULLEN), s.card ? (cardStarted[i] ? " card started ok" : " CARD DID NOT START") : "");
    while ((int32_t)(millis() - until) < 0) { esp_task_wdt_reset(); lightSleepOnce(); rxFlag = false; }
  }
  ltNormalLines(); xp.input(XP_SD_PULLEN); xp.output(XP_SD_EN, LOW);
  ltMarker();
  { Preferences p; p.begin("pgr-leak", false); p.putBytes("lv", levels, sizeof levels); p.putUChar("done", LT_N); p.end(); }
  delay(500); esp_restart();                                                    // everything back to normal
}
static void ltPrint() {
  static char levels[LT_N][64]; Preferences p; p.begin("pgr-leak", true); bool have = p.getBytes("lv", levels, sizeof levels) == sizeof levels; p.end();
  if (!have) { Serial.println("leak test: no results saved"); return; }
  for (int i = 0; i < LT_N; i++) Serial.printf("  step %2d  %-46s read back: %s\n", i + 1, LT_PLAN[i].name, levels[i]);
}
static void leakTestTick() {
  static bool done = false;
  if (done || millis() < 60000UL) return;
  static uint32_t next = 0; if (millis() < next) return; next = millis() + 2000;
  if (usbPresent() || !radioOk) return;
  Preferences p; p.begin("pgr-leak", true); bool ran = p.getUChar("done", 0) != 0; p.end();
  done = true;
  if (ran && (int)esp_reset_reason() == ESP_RST_SW) { say("Leak test finished. Stop the recording."); screenTouch(); return; }     // the restart at its end
  ltRun();
}
