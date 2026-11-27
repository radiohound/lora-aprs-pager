#include <stdio.h>
#include "hapticpower.h"
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { fails++; printf("FAIL: %s\n", what); } }
int main() {
  HapticPower p;
  expect(!p.idle(100000), "nothing to switch off before the first use");
  expect(p.wake(1000), "off: it must be set up before the buzz");
  expect(!p.idle(1000) && !p.idle(2999), "stays on while the buzz plays");
  expect(p.idle(3000), "off 2 s after the buzz started");
  expect(!p.idle(9000), "switched off only once");
  expect(p.wake(10000) && !p.wake(10100) && !p.wake(11900), "knob clicks: set up once, then it stays on");
  expect(!p.idle(13000) && p.idle(13900), "the time counts from the latest buzz");
  p.wake(0xFFFFFF00u);
  expect(!p.idle(0x00000200u) && p.idle(0x00001000u), "the millisecond counter rolling over");
  printf(fails ? "%d FAILED\n" : "all haptic power tests passed\n", fails); return fails ? 1 : 0;
}
