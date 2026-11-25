// How long the GPS sleeps between fixes while the screen is off. Plain C++, tested in test/host/test_gpsduty.cpp.
// Short interval while the pager is moving; after two wake-ups in a row that found it in the same place, or found no fix at all (probably indoors
// and not going anywhere), the long interval. A fix that shows movement brings the short interval back at once.
#pragma once
#include <stdint.h>
#include "geo.h"

struct GpsAdaptive {
  int still = 0;                       // consecutive wake-ups that found us in the same place or found no fix
  double prevLat = 999, prevLon = 999; // the last fix a wake-up got (999 = none yet)
  bool motionAware = false;            // the motion sensor is running: standing still can sleep much longer, a movement wakes the GPS at once
  bool motionWakeSeen = false; uint32_t lastMotionWakeMs = 0;   // when a movement last woke the GPS (rate limit)
};
static const uint32_t GPS_MOTION_GAP_MS = 60000;   // a movement wakes the GPS at most once a minute (vibration, a pager carried in a bag)
static const double GPS_STILL_M = 40.0;   // two fixes closer than this (GPS jitter included) count as standing still

inline void gpsAdaptiveReset(GpsAdaptive &a, bool haveFix, double lat, double lon) {   // the screen has just gone off
  a.still = 0; a.prevLat = haveFix ? lat : 999; a.prevLon = haveFix ? lon : 999;
}
// How long one wake-up may search before giving up, and how long to sleep after a miss. `misses` = wake-ups in a row without a fresh fix.
// Until the first fix since start-up the GPS gets a long search: a first fix can take minutes, and cutting it off early can stop it ever arriving.
// The limits still end, so a pager indoors does not search all the time.
static const uint32_t GPS_FIRST_LIMIT_MS = 300000;                                     // no fix since start-up
static const uint32_t GPS_LIMIT_MS = 30000, GPS_LIMIT_1_MS = 90000, GPS_LIMIT_2_MS = 180000;   // had a fix: 0, 1, 2 or more misses
inline uint32_t gpsAwakeLimit(bool everFix, int misses) {
  if (!everFix) return GPS_FIRST_LIMIT_MS;
  return misses <= 0 ? GPS_LIMIT_MS : (misses == 1 ? GPS_LIMIT_1_MS : GPS_LIMIT_2_MS);
}
// `misses` includes the one that has just ended. Two quick retries, then 5 minutes (or the long interval if that is longer);
// with a fix behind us, five misses in a row stretch it to 15 minutes.
inline uint32_t gpsMissSleep(bool everFix, int misses, int stillMin) {
  if (misses < 3) return 60000UL;
  uint32_t mins = stillMin > 5 ? stillMin : 5;
  if (everFix && misses >= 5 && mins < 15) mins = 15;
  return mins * 60000UL;
}

// Call when a wake-up ends. Returns how long to sleep, in milliseconds.
// stillMin is the long interval; with a motion sensor it is the backstop check (nothing moved, but keep the GPS data fresh), so it can be long.
inline uint32_t gpsAdaptiveNext(GpsAdaptive &a, bool gotFix, double lat, double lon, int moveSec, int stillMin) {
  if (gotFix) {
    if (a.prevLat < 900 && geoDistKm(a.prevLat, a.prevLon, lat, lon) * 1000.0 < GPS_STILL_M) a.still++; else a.still = 0;
    a.prevLat = lat; a.prevLon = lon;
  } else a.still++;
  return a.still >= 2 ? (uint32_t)stillMin * 60000UL : (uint32_t)moveSec * 1000UL;
}

// The motion sensor reported movement (tilt or significant motion). Returns true if the GPS should wake now: it was in the long sleep and the
// last movement wake was at least GPS_MOTION_GAP_MS ago. Either way the pager counts as moving, so the short interval applies again.
inline bool gpsAdaptiveOnMotion(GpsAdaptive &a, uint32_t nowMs) {
  if (!a.motionAware) return false;
  bool wasLong = a.still >= 2;
  a.still = 0;
  if (!wasLong) return false;
  if (a.motionWakeSeen && (uint32_t)(nowMs - a.lastMotionWakeMs) < GPS_MOTION_GAP_MS) { a.still = 2; return false; }   // too soon: stay in the long sleep
  a.motionWakeSeen = true; a.lastMotionWakeMs = nowMs;
  return true;
}
