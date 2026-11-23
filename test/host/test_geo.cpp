// Host tests for src/geo.h.  Run:  c++ -std=c++17 -I src test/host/test_geo.cpp -o /tmp/test_geo && /tmp/test_geo
#include <cstdio>
#include <cstring>
#include "geo.h"
static int fails = 0;
static void expect(bool c, const char *w) { if (!c) { fails++; printf("FAIL: %s\n", w); } }
static bool near(double a, double b, double e) { return fabs(a - b) < e; }
int main() {
  // one degree of longitude on the equator = 111.195 km with R = 6371.0088
  expect(near(geoDistKm(0, 0, 0, 1), 111.195, 0.01), "1 deg lon on equator");
  expect(near(geoDistKm(0, 0, 1, 0), 111.195, 0.01), "1 deg lat");
  expect(near(geoDistKm(37.3, -121.9, 37.3, -121.9), 0, 1e-9), "same point");
  expect(near(geoDistKm(10, 20, -30, 140), geoDistKm(-30, 140, 10, 20), 1e-9), "symmetric");
  expect(near(geoBearing(0, 0, 1, 0), 0, 1e-6), "north");
  expect(near(geoBearing(0, 0, 0, 1), 90, 1e-6), "east");
  expect(near(geoBearing(1, 0, 0, 0), 180, 1e-6), "south");
  expect(near(geoBearing(0, 1, 0, 0), 270, 1e-6), "west");
  // San Francisco -> San Jose: about 68 km to the south-east
  double d = geoDistKm(37.7749, -122.4194, 37.3382, -121.8863), b = geoBearing(37.7749, -122.4194, 37.3382, -121.8863);
  expect(d > 65 && d < 71, "SF-SJ distance is about 68 km");
  expect(strcmp(geoCompass(b), "SE") == 0, "SF-SJ is south-east");
  expect(strcmp(geoCompass(0), "N") == 0 && strcmp(geoCompass(359), "N") == 0 && strcmp(geoCompass(22.4), "N") == 0, "N sector");
  expect(strcmp(geoCompass(22.6), "NE") == 0 && strcmp(geoCompass(180), "S") == 0 && strcmp(geoCompass(315), "NW") == 0, "other sectors");
  char b2[16];
  geoFmtDist(b2, sizeof b2, 0.85); expect(strcmp(b2, "850m") == 0, "850 m");
  geoFmtDist(b2, sizeof b2, 7.46); expect(strcmp(b2, "7.5km") == 0, "7.5 km");
  geoFmtDist(b2, sizeof b2, 67.6); expect(strcmp(b2, "68km") == 0, "68 km");
  geoFmtAge(b2, sizeof b2, 45); expect(strcmp(b2, "45s") == 0, "45 s");
  geoFmtAge(b2, sizeof b2, 180); expect(strcmp(b2, "3m") == 0, "3 m");
  geoFmtAge(b2, sizeof b2, 7300); expect(strcmp(b2, "2h") == 0, "2 h");
  geoFmtAge(b2, sizeof b2, 200000); expect(strcmp(b2, "2d") == 0, "2 d");
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
