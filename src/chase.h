// Chase mode: the screen stays on and the map follows my own position or a chosen station. Plain C++, tested in test/host/test_chase.cpp.
#pragma once
#include <stdint.h>

static const uint32_t CHASE_PAUSE_MS = 15000;      // after looking around (panning, scrolling the list) the map comes back after this long

struct Chase {
  bool on = false;
  char call[10] = "";                  // the station followed; empty = follow me
  bool paused = false; uint32_t lookAt = 0;                      // looking around, and when that last happened
  bool have = false; double lat = 0, lon = 0; uint32_t heard = 0;   // the followed station's last position (kept even if it leaves the list)
  int warned = 101;                    // lowest low-battery step already announced
  bool followingMe() const { return call[0] == 0; }
};
inline void chaseStart(Chase &c, const char *call) {
  c = Chase(); c.on = true;
  if (call) { int i = 0; while (i < 9 && call[i]) { c.call[i] = call[i]; i++; } c.call[i] = 0; }
}
inline void chaseStop(Chase &c) { c = Chase(); }
inline void chaseLook(Chase &c, uint32_t now) { if (c.on) { c.paused = true; c.lookAt = now; } }
// May the map move to the followed position now? Ends a pause that has run its time.
inline bool chaseFollowing(Chase &c, uint32_t now) {
  if (!c.on) return false;
  if (c.paused && (uint32_t)(now - c.lookAt) >= CHASE_PAUSE_MS) c.paused = false;
  return !c.paused;
}
// Low battery while chasing: returns 20 or 10 once when the charge first falls to it, else 0. The screen is never switched off for this.
inline int chaseBatteryWarning(Chase &c, int pct, bool charging) {
  if (!c.on || pct < 0 || charging) return 0;
  int step = pct <= 10 ? 10 : (pct <= 20 ? 20 : 0);
  if (!step || step >= c.warned) return 0;
  c.warned = step; return step;
}
