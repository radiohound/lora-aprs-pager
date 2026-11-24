// How long the GPS sleeps between fixes while the screen is off. Plain C++, tested in test/host/test_gpsduty.cpp.
// Short interval while the pager is moving; after two wake-ups in a row that found it in the same place, or found no fix at all (probably indoors
// and not going anywhere), the long interval. A fix that shows movement brings the short interval back at once.
#pragma once
#include <stdint.h>
#include "geo.h"

struct GpsAdaptive {
  int still = 0;                       // consecutive wake-ups that found us in the same place or found no fix
  double prevLat = 999, prevLon = 999; // the last fix a wake-up got (999 = none yet)
};
static const double GPS_STILL_M = 40.0;   // two fixes closer than this (GPS jitter included) count as standing still

inline void gpsAdaptiveReset(GpsAdaptive &a, bool haveFix, double lat, double lon) {   // the screen has just gone off
  a.still = 0; a.prevLat = haveFix ? lat : 999; a.prevLon = haveFix ? lon : 999;
}
// Call when a wake-up ends. Returns how long to sleep, in milliseconds.
inline uint32_t gpsAdaptiveNext(GpsAdaptive &a, bool gotFix, double lat, double lon, int moveSec, int stillMin) {
  if (gotFix) {
    if (a.prevLat < 900 && geoDistKm(a.prevLat, a.prevLon, lat, lon) * 1000.0 < GPS_STILL_M) a.still++; else a.still = 0;
    a.prevLat = lat; a.prevLon = lon;
  } else a.still++;
  return a.still >= 2 ? (uint32_t)stillMin * 60000UL : (uint32_t)moveSec * 1000UL;
}
