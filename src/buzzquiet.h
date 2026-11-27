// The pager's own buzz shakes the motion sensor: it reports steps about a quarter of a second after the buzz after a beacon, which woke the
// GPS for nothing (34 of 35 motion wakes in one day's log of a pager lying still). So motion reports are ignored from the start of any buzz
// until BUZZ_QUIET_MS later: the longest effect (about a second, not measured) and about two seconds for late reports.
// A real movement that starts inside that time is noticed by the sensor's next report. Plain C++, tested in test/host/test_buzzquiet.cpp.
#pragma once
#include <stdint.h>

static const uint32_t BUZZ_QUIET_MS = 3000;

struct BuzzQuiet {
  bool any = false; uint32_t at = 0;       // when the last buzz started
  void buzzed(uint32_t now) { any = true; at = now; }
  bool quiet(uint32_t now) {               // true = do not believe motion reports now
    if (any && (uint32_t)(now - at) >= BUZZ_QUIET_MS) any = false;       // over (also keeps the millis() roll-over harmless)
    return any;
  }
};
