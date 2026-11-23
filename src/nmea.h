// Minimal NMEA GGA parser for the GPS fix. Plain C++, tested in test/host/test_nmea.cpp.
#pragma once
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

// Seconds since 1970-01-01 UTC for a calendar date and time (days-from-civil algorithm).
inline uint32_t nmeaEpoch(int y, int mo, int d, int h, int mi, int sec) {
  y -= mo <= 2; int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + sec);
}
struct NmeaFix { bool valid = false; double lat = 0, lon = 0; int sats = 0; uint32_t epoch = 0; };   // epoch: UTC from the last valid RMC, 0 = unknown

class NmeaParser {
public:
  NmeaFix fix;
  unsigned long lines = 0;      // complete lines seen (alive check)
  // Feed one character; returns true when a position update was just parsed.
  bool feed(char c) {
    if (c == '\r') return false;
    if (c != '\n') { if (_n < sizeof _l - 1) _l[_n++] = c; return false; }
    _l[_n] = 0; _n = 0; lines++;
    return parse(_l);
  }
  static bool checksumOk(const char *s) {
    if (s[0] != '$') return false;
    unsigned x = 0; const char *p = s + 1;
    while (*p && *p != '*') x ^= (unsigned char)*p++;
    if (*p != '*') return false;
    return (unsigned)strtoul(p + 1, nullptr, 16) == x;
  }
private:
  char _l[128]; size_t _n = 0;
  static double coord(const char *f, const char *hemi) {
    double v = atof(f); int dd = (int)(v / 100.0); double r = dd + (v - dd * 100.0) / 60.0;
    return (hemi[0] == 'S' || hemi[0] == 'W') ? -r : r;
  }
  bool parseRmc(char *s) {                                          // $GNRMC,hhmmss.ss,A,lat,N,lon,E,spd,cog,ddmmyy,...
    char *f[12]; int k = 0; char *p = s;
    while (k < 12) { f[k++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
    if (k < 10 || f[2][0] != 'A' || strlen(f[1]) < 6 || strlen(f[9]) < 6) return false;
    int hh = (f[1][0] - '0') * 10 + f[1][1] - '0', mi = (f[1][2] - '0') * 10 + f[1][3] - '0', ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
    int dd = (f[9][0] - '0') * 10 + f[9][1] - '0', mo = (f[9][2] - '0') * 10 + f[9][3] - '0', yy = (f[9][4] - '0') * 10 + f[9][5] - '0';
    if (hh > 23 || mi > 59 || ss > 60 || dd < 1 || dd > 31 || mo < 1 || mo > 12) return false;
    fix.epoch = nmeaEpoch(2000 + yy, mo, dd, hh, mi, ss); return false;      // a time update is not a position update
  }
  bool parse(char *s) {
    if (!checksumOk(s)) return false;
    if (strncmp(s + 3, "RMC", 3) == 0) return parseRmc(s);          // date and time
    if (strncmp(s + 3, "GGA", 3) != 0) return false;              // $GNGGA / $GPGGA
    char *f[15]; int k = 0; char *p = s;
    while (k < 15) { f[k++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
    if (k < 8) return false;
    int q = atoi(f[6]); fix.sats = atoi(f[7]);
    if (q == 0 || !f[2][0] || !f[4][0]) { fix.valid = false; return true; }
    fix.lat = coord(f[2], f[3]); fix.lon = coord(f[4], f[5]); fix.valid = true;
    return true;
  }
};
