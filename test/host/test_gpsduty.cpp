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
  // motion sensor
  gpsAdaptiveReset(a, true, lat, lon); a.motionAware = false; a.motionWakeSeen = false;
  gpsAdaptiveNext(a, true, lat, lon, MV, ST); gpsAdaptiveNext(a, true, lat, lon, MV, ST);   // now in the long sleep
  expect(!gpsAdaptiveOnMotion(a, 1000), "without a motion sensor a movement does nothing");
  a.motionAware = true;
  expect(gpsAdaptiveOnMotion(a, 1000), "movement during the long sleep wakes the GPS now");
  expect(a.still == 0, "and the short interval applies again");
  expect(!gpsAdaptiveOnMotion(a, 2000), "movement while already in the short interval: nothing to wake");
  gpsAdaptiveNext(a, true, lat, lon, MV, ST); expect(gpsAdaptiveNext(a, true, lat, lon, MV, ST) == LONG, "two still wake-ups later: long again");
  expect(!gpsAdaptiveOnMotion(a, 30000), "a second movement wake within a minute is held back (vibration)");
  expect(a.still >= 2, "and the long sleep goes on");
  expect(gpsAdaptiveOnMotion(a, 61001 + 1000), "a minute later it wakes again");
  // search limits and retry pauses
  expect(gpsAwakeLimit(false, 0) == 300000UL && gpsAwakeLimit(false, 9) == 300000UL, "no fix since start-up: 5 minute searches");
  expect(gpsAwakeLimit(true, 0) == 30000UL, "had a fix: 30 s");
  expect(gpsAwakeLimit(true, 1) == 90000UL, "after one miss: 90 s");
  expect(gpsAwakeLimit(true, 2) == 180000UL && gpsAwakeLimit(true, 8) == 180000UL, "after two or more: 3 minutes");
  expect(gpsMissSleep(false, 1, 5) == 60000UL && gpsMissSleep(false, 2, 5) == 60000UL, "two quick retries");
  expect(gpsMissSleep(false, 3, 5) == 300000UL && gpsMissSleep(false, 20, 5) == 300000UL, "never had a fix: then every 5 minutes");
  expect(gpsMissSleep(true, 4, 5) == 300000UL, "had a fix: 5 minutes");
  expect(gpsMissSleep(true, 5, 5) == 900000UL, "five misses: 15 minutes");
  expect(gpsMissSleep(true, 5, 30) == 1800000UL, "a longer still setting is respected");
  if (!fails) printf("gpsduty: all ok\n");
  return fails ? 1 : 0;
}
