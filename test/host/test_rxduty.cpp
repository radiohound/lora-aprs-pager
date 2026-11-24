#include "../../src/rxduty.h"
#include <cstdio>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  const uint32_t TCXO = 5000;
  RxDutyPlan p12 = rxDutyPlan(12, 125.0f, RXD_SENDER_PREAMBLE, RXD_MIN_SYMBOLS, TCXO);
  expect(p12.usable, "SF12 / 125 kHz is usable");
  expect(p12.sleepUs == 32768UL * 2 && p12.wakeUs == 32768UL * 4, "SF12: sleeps 2 symbols (65.5 ms), awake 4 symbols (131 ms)");
  expect(p12.savingPct >= 25 && p12.savingPct <= 40, "SF12: about 30% of the time asleep after overhead");
  expect(rxDutyPlan(11, 125.0f, 8, 3, TCXO).usable, "SF11 is usable");
  expect(!rxDutyPlan(10, 125.0f, 8, 3, TCXO).usable, "SF10: too little left after the wake-up overhead");
  expect(!rxDutyPlan(9, 125.0f, 8, 3, TCXO).usable, "SF9 unused");
  expect(!rxDutyPlan(8, 125.0f, 8, 3, TCXO).usable, "SF8 unused");
  expect(!rxDutyPlan(7, 125.0f, 8, 3, TCXO).usable, "SF7 unused");
  expect(!rxDutyPlan(12, 125.0f, 8, 4, TCXO).usable, "RadioLib's own case: 4 or more minimum symbols leaves no time to sleep with an 8-symbol preamble");
  expect(!rxDutyPlan(12, 125.0f, 8, 8, TCXO).usable, "default minimum of 8: never sleeps");
  expect(rxDutyPlan(12, 125.0f, 12, 4, TCXO).usable, "a sender with a longer preamble (12) allows 4 minimum symbols");
  expect(!rxDutyPlan(12, 0.0f, 8, 3, TCXO).usable && !rxDutyPlan(13, 125.0f, 8, 3, TCXO).usable, "bad inputs refused");
  expect(rxDutyPlan(12, 250.0f, 8, 3, TCXO).sleepUs == 16384UL * 2, "doubling the bandwidth halves the symbol time");
  if (!fails) printf("rxduty: all ok\n");
  return fails ? 1 : 0;
}
