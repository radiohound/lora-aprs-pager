#include "../../src/gpsduty.h"
#include <cstdio>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  const int MV = 60, ST = 5; const uint32_t SHORT = 60000, LONG = 300000;
  GpsAdaptive a; double lat = 37.5, lon = -121.9;
  gpsAdaptiveReset(a, true, lat, lon);
  expect(gpsAdaptiveNext(a, true, lat, lon, MV, ST) == SHORT, "first still fix: still short (needs two)");
  expect(gpsAdaptiveNext(a, true, lat + 0.0001, lon, MV, ST) == LONG, "second fix about 11 m away: standing still, long interval");   // 0.0001 deg = 11 m
  expect(gpsAdaptiveNext(a, true, lat + 0.0001, lon, MV, ST) == LONG, "stays long while still");
  expect(gpsAdaptiveNext(a, true, lat + 0.01, lon, MV, ST) == SHORT, "a move of about 1.1 km brings the short interval back at once");
  expect(gpsAdaptiveNext(a, true, lat + 0.02, lon, MV, ST) == SHORT, "still moving: short");
  // no fix (indoors): counts as still
  gpsAdaptiveReset(a, false, 0, 0);
  expect(gpsAdaptiveNext(a, false, 0, 0, MV, ST) == SHORT, "one miss: short");
  expect(gpsAdaptiveNext(a, false, 0, 0, MV, ST) == LONG, "two misses in a row: long");
  expect(gpsAdaptiveNext(a, true, lat, lon, MV, ST) == SHORT, "a fix after misses has no previous position: treated as movement");
  // jitter: 30 m is still still, 60 m is a move
  gpsAdaptiveReset(a, true, lat, lon); gpsAdaptiveNext(a, true, lat + 0.0002, lon, MV, ST);   // 22 m
  expect(gpsAdaptiveNext(a, true, lat + 0.0004, lon, MV, ST) == LONG, "22 m steps are jitter, not movement");
  gpsAdaptiveReset(a, true, lat, lon);
  expect(gpsAdaptiveNext(a, true, lat + 0.0006, lon, MV, ST) == SHORT, "66 m is movement");
  // settings are used
  gpsAdaptiveReset(a, true, lat, lon); gpsAdaptiveNext(a, true, lat, lon, 30, 15);
  expect(gpsAdaptiveNext(a, true, lat, lon, 30, 15) == 15 * 60000UL, "still interval follows the setting");
  gpsAdaptiveReset(a, true, lat, lon);
  expect(gpsAdaptiveNext(a, true, lat + 0.01, lon, 30, 15) == 30000UL, "moving interval follows the setting");
  if (!fails) printf("gpsduty: all ok\n");
  return fails ? 1 : 0;
}
