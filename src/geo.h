// Distance, bearing and short text formats for the station list. Plain C++, tested in test/host/test_geo.cpp.
#pragma once
#include <cmath>
#include <cstdio>

inline double geoRad(double d) { return d * M_PI / 180.0; }

// Great-circle distance in km (haversine, mean Earth radius).
inline double geoDistKm(double lat1, double lon1, double lat2, double lon2) {
  double dla = geoRad(lat2 - lat1), dlo = geoRad(lon2 - lon1);
  double a = sin(dla / 2) * sin(dla / 2) + cos(geoRad(lat1)) * cos(geoRad(lat2)) * sin(dlo / 2) * sin(dlo / 2);
  return 6371.0088 * 2 * atan2(sqrt(a), sqrt(1 - a));
}
// Initial bearing from point 1 to point 2, 0..360 degrees (0 = north, 90 = east).
inline double geoBearing(double lat1, double lon1, double lat2, double lon2) {
  double dlo = geoRad(lon2 - lon1), p1 = geoRad(lat1), p2 = geoRad(lat2);
  double y = sin(dlo) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dlo);
  double b = atan2(y, x) * 180.0 / M_PI;
  return b < 0 ? b + 360.0 : b;
}
inline const char *geoCompass(double deg) {
  static const char *n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return n[(int)floor((deg + 22.5) / 45.0) % 8];
}
// "850m", "7.5km", "68km"
inline void geoFmtDist(char *b, size_t n, double km) {
  if (km < 1.0) snprintf(b, n, "%dm", (int)(km * 1000.0 + 0.5));
  else if (km < 10.0) snprintf(b, n, "%.1fkm", km);
  else snprintf(b, n, "%dkm", (int)(km + 0.5));
}
// age in seconds -> "45s", "3m", "2h", "5d"
inline void geoFmtAge(char *b, size_t n, unsigned long s) {
  if (s < 60) snprintf(b, n, "%lus", s);
  else if (s < 3600) snprintf(b, n, "%lum", s / 60);
  else if (s < 86400) snprintf(b, n, "%luh", s / 3600);
  else snprintf(b, n, "%lud", s / 86400);
}
