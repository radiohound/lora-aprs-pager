#include "../../src/chase.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  Chase c;
  expect(c.followingMe() && !c.keepScreen && chaseFollowing(c, 0), "default: follow me, screen times out as usual");
  chaseFollow(c, "N0CALL-11"); expect(!c.followingMe() && strcmp(c.call, "N0CALL-11") == 0, "follow a station, nine characters kept");
  chaseLook(c, 5000); expect(!chaseFollowing(c, 5000) && !chaseFollowing(c, 19999), "paused for 15 s");
  expect(chaseFollowing(c, 20000), "then follows again");
  chaseLook(c, 30000); chaseLook(c, 40000); expect(!chaseFollowing(c, 54999) && chaseFollowing(c, 55000), "each look restarts the 15 s");
  chaseLook(c, 0xFFFFF000u); expect(!chaseFollowing(c, 100) && chaseFollowing(c, 20000), "works across the millisecond counter wrapping");
  chaseLook(c, 100); c.have = true; chaseFollow(c, nullptr); expect(c.followingMe() && !c.paused && !c.have, "back to me: no pause, old station forgotten");
  expect(chaseBatteryWarning(c, 5, false) == 0, "no battery warning unless the screen is kept on");
  chaseKeepScreen(c, true);
  chaseFollow(c, "N0CALL-9"); expect(c.keepScreen, "changing what is followed leaves the screen setting alone");
  expect(chaseBatteryWarning(c, 50, false) == 0, "no warning with charge");
  expect(chaseBatteryWarning(c, 20, false) == 20 && chaseBatteryWarning(c, 19, false) == 0, "20 % once");
  expect(chaseBatteryWarning(c, 10, false) == 10 && chaseBatteryWarning(c, 5, false) == 0 && chaseBatteryWarning(c, 18, false) == 0, "10 % once, no repeat");
  chaseKeepScreen(c, true); expect(chaseBatteryWarning(c, 8, true) == 0 && chaseBatteryWarning(c, -1, false) == 0 && chaseBatteryWarning(c, 8, false) == 10, "not while charging or unknown");
  chaseKeepScreen(c, false); expect(chaseBatteryWarning(c, 5, false) == 0, "off");
  if (!fails) printf("chase: all ok\n");
  return fails ? 1 : 0;
}
