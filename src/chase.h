// What the map follows, and keeping the screen on. Like a car navigator, the map follows my own position unless a station has been chosen
// instead. Keeping the screen on is a separate choice. Plain C++, tested in test/host/test_chase.cpp.
#pragma once
#include <stdint.h>

static const uint32_t CHASE_PAUSE_MS = 15000;      // after looking around (panning, scrolling the list) the map comes back after this long

struct Chase {
  char call[10] = "";                  // the station followed; empty = follow me (the default)
  bool keepScreen = false;             // the screen does not dim or switch off (off after every restart)
  bool paused = false; uint32_t lookAt = 0;                      // looking around, and when that last happened
  bool have = false; double lat = 0, lon = 0; uint32_t heard = 0;   // the followed station's last position (kept even if it leaves the list)
  int warned = 101;                    // lowest low-battery step already announced
  bool followingMe() const { return call[0] == 0; }
};
// Follow a station (nullptr or "" = follow me again). Ends any pause.
inline void chaseFollow(Chase &c, const char *call) {
  int i = 0; if (call) while (i < 9 && call[i]) { c.call[i] = call[i]; i++; }
  c.call[i] = 0; c.paused = false; c.have = false;
}
inline void chaseKeepScreen(Chase &c, bool on) { c.keepScreen = on; c.warned = 101; }
inline void chaseLook(Chase &c, uint32_t now) { c.paused = true; c.lookAt = now; }
// May the map move to the followed position now? Ends a pause that has run its time.
inline bool chaseFollowing(Chase &c, uint32_t now) {
  if (c.paused && (uint32_t)(now - c.lookAt) >= CHASE_PAUSE_MS) c.paused = false;
  return !c.paused;
}
// Low battery with the screen kept on: returns 20 or 10 once when the charge first falls to it, else 0. The screen is never switched off for this.
inline int chaseBatteryWarning(Chase &c, int pct, bool charging) {
  if (!c.keepScreen || pct < 0 || charging) return 0;
  int step = pct <= 10 ? 10 : (pct <= 20 ? 20 : 0);
  if (!step || step >= c.warned) return 0;
  c.warned = step; return step;
}
