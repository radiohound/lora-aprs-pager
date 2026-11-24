// Receive duty cycle for the SX1262 while the screen is off: the radio sleeps and wakes in short bursts to catch the start (preamble) of a packet.
// Plain C++, tested in test/host/test_rxduty.cpp. The numbers mirror RadioLib's calculateRxDutyCycle() (from Semtech app note AN1200.48), but with the
// "minimum symbols" set by us: LoRa APRS stations send an 8-symbol preamble, and RadioLib's default of 8 leaves no time to sleep at all.
// Only worth it when the preamble lasts long enough: at low spreading factors the radio's wake-up time (TCXO start + about 1 ms) eats the saving.
#pragma once
#include <stdint.h>

static const int RXD_SENDER_PREAMBLE = 8;    // symbols: what LoRa APRS stations send
static const int RXD_MIN_SYMBOLS = 3;        // the radio must see this many preamble symbols while awake to lock on (fewer = more sleep, less certain)
static const int RXD_MIN_SAVING_PCT = 25;    // do nothing unless at least this share of the time is asleep after the wake-up overhead

struct RxDutyPlan { bool usable; uint32_t sleepUs, wakeUs; int savingPct; };

inline RxDutyPlan rxDutyPlan(int sf, float bwKHz, int senderPre, int minSym, uint32_t tcxoDelayUs) {
  RxDutyPlan p = {false, 0, 0, 0};
  if (sf < 5 || sf > 12 || bwKHz <= 0 || senderPre <= 0 || minSym <= 0 || 2 * minSym > senderPre) return p;
  uint32_t sym = (uint32_t)(((uint32_t)(10 * 1000) << sf) / (10 * bwKHz));                 // symbol length, microseconds (same integer maths as RadioLib)
  uint32_t sleepSyms = (uint32_t)(senderPre - 2 * minSym);
  p.sleepUs = sym * sleepSyms;
  int64_t a = ((int64_t)sym * (senderPre + 1) - ((int64_t)p.sleepUs - 1000)) / 2;           // RadioLib's two conditions for the awake period
  int64_t b = (int64_t)sym * (minSym + 1);
  p.wakeUs = (uint32_t)(a > b ? a : b);
  uint32_t trans = tcxoDelayUs + 1000;                                                      // radio start-up each cycle: not asleep, not receiving either
  if (p.sleepUs <= trans + 16) return p;                                                    // RadioLib itself falls back to continuous receive here
  p.savingPct = (int)(((int64_t)(p.sleepUs - trans) * 100) / ((int64_t)p.sleepUs + p.wakeUs));
  p.usable = p.savingPct >= RXD_MIN_SAVING_PCT;
  return p;
}
