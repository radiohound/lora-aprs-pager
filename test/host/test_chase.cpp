#include "../../src/chase.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  Chase c;
  expect(!c.on && !chaseFollowing(c, 0), "off at start");
  chaseLook(c, 100); expect(!c.paused, "looking around does nothing when off");
  chaseStart(c, nullptr); expect(c.on && c.followingMe() && chaseFollowing(c, 1000), "follow me");
  chaseStart(c, "N0CALL-11"); expect(!c.followingMe() && strcmp(c.call, "N0CALL-11") == 0, "follow a station, nine characters kept");
  chaseLook(c, 5000); expect(!chaseFollowing(c, 5000) && !chaseFollowing(c, 19999), "paused for 15 s");
  expect(chaseFollowing(c, 20000), "then follows again");
  chaseLook(c, 30000); chaseLook(c, 40000); expect(!chaseFollowing(c, 54999) && chaseFollowing(c, 55000), "each look restarts the 15 s");
  chaseLook(c, 0xFFFFF000u); expect(!chaseFollowing(c, 100) && chaseFollowing(c, 20000), "works across the millisecond counter wrapping");
  expect(chaseBatteryWarning(c, 50, false) == 0, "no warning with charge");
  expect(chaseBatteryWarning(c, 20, false) == 20 && chaseBatteryWarning(c, 19, false) == 0, "20 % once");
  expect(chaseBatteryWarning(c, 10, false) == 10 && chaseBatteryWarning(c, 5, false) == 0 && chaseBatteryWarning(c, 18, false) == 0, "10 % once, no repeat");
  chaseStart(c, nullptr); expect(chaseBatteryWarning(c, 8, true) == 0 && chaseBatteryWarning(c, -1, false) == 0 && chaseBatteryWarning(c, 8, false) == 10, "not while charging or unknown");
  chaseStop(c); expect(!c.on && chaseBatteryWarning(c, 5, false) == 0, "off");
  if (!fails) printf("chase: all ok\n");
  return fails ? 1 : 0;
}
