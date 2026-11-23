// Minimal NMEA GGA parser for the GPS fix. Plain C++, tested in test/host/test_nmea.cpp.
#pragma once
#include <cstring>
#include <cstdlib>
#include <cstdio>

struct NmeaFix { bool valid = false; double lat = 0, lon = 0; int sats = 0; };

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
  static double coord(const char *f, const char *hemi, bool lon) {
    double v = atof(f); int dd = (int)(v / 100.0); double r = dd + (v - dd * 100.0) / 60.0;
    return (hemi[0] == 'S' || hemi[0] == 'W') ? -r : r;
  }
  bool parse(char *s) {
    if (!checksumOk(s)) return false;
    if (strncmp(s + 3, "GGA", 3) != 0) return false;              // $GNGGA / $GPGGA
    char *f[15]; int k = 0; char *p = s;
    while (k < 15) { f[k++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
    if (k < 8) return false;
    int q = atoi(f[6]); fix.sats = atoi(f[7]);
    if (q == 0 || !f[2][0] || !f[4][0]) { fix.valid = false; return true; }
    fix.lat = coord(f[2], f[3], false); fix.lon = coord(f[4], f[5], true); fix.valid = true;
    return true;
  }
};
