// Wall clock helpers: UTC seconds -> local date and time (fixed UTC offset plus optional US daylight saving), and BCD for the RTC chip.
// Plain C++ (no Arduino), tested in test/host/test_clock.cpp.
#pragma once
#include <cstdint>
#include <cstdio>

struct LocalTime { int year, month, day, hour, min, sec, wday; };   // wday 0 = Sunday

// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
inline void clockCivil(int64_t z, int &y, int &m, int &d) {
  z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; y = (int)(yoe + era * 400);
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1); m = (int)(mp < 10 ? mp + 3 : mp - 9); if (m <= 2) y++;
}
inline int64_t clockDays(int y, int m, int d) {
  y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
inline LocalTime clockBreak(int64_t t) {
  LocalTime r; int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400); int64_t s = t - days * 86400;
  clockCivil(days, r.year, r.month, r.day); r.hour = (int)(s / 3600); r.min = (int)(s % 3600 / 60); r.sec = (int)(s % 60);
  r.wday = (int)(((days % 7) + 11) % 7);       // 1970-01-01 was a Thursday
  return r;
}
// US daylight saving: from the second Sunday of March 02:00 local standard time to the first Sunday of November 02:00 local daylight time
inline bool clockUsDst(int64_t utc, int stdOffsetHours) {
  int y = clockBreak(utc + (int64_t)stdOffsetHours * 3600).year;
  int64_t m1 = clockDays(y, 3, 1), w = ((m1 % 7) + 11) % 7; int64_t sun2 = m1 + (7 - w) % 7 + 7;          // second Sunday of March
  int64_t n1 = clockDays(y, 11, 1); w = ((n1 % 7) + 11) % 7; int64_t sun1 = n1 + (7 - w) % 7;              // first Sunday of November
  int64_t start = sun2 * 86400 + 2 * 3600 - (int64_t)stdOffsetHours * 3600;
  int64_t end = sun1 * 86400 + 2 * 3600 - (int64_t)(stdOffsetHours + 1) * 3600;
  return utc >= start && utc < end;
}
inline LocalTime clockLocal(int64_t utc, int stdOffsetHours, bool usDst) {
  int off = stdOffsetHours + ((usDst && clockUsDst(utc, stdOffsetHours)) ? 1 : 0);
  return clockBreak(utc + (int64_t)off * 3600);
}
// "Mon 23 Nov 14:05"
inline void clockFmt(char *out, size_t n, const LocalTime &t) {
  static const char *D[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(out, n, "%s %d %s %02d:%02d", D[t.wday % 7], t.day, M[(t.month - 1 + 12) % 12], t.hour, t.min);
}
// Time of an event for the screen: "09:14" when it was today (local date), otherwise "Nov 22 09:14". now = 0 (clock not set): always with the date.
inline void clockStamp(char *out, size_t n, int64_t epoch, int64_t now, int stdOffsetHours, bool usDst) {
  static const char *M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  LocalTime e = clockLocal(epoch, stdOffsetHours, usDst);
  if (now > 0) { LocalTime t = clockLocal(now, stdOffsetHours, usDst); if (t.year == e.year && t.month == e.month && t.day == e.day) { snprintf(out, n, "%02d:%02d", e.hour, e.min); return; } }
  snprintf(out, n, "%s %d %02d:%02d", M[(e.month - 1 + 12) % 12], e.day, e.hour, e.min);
}
inline uint8_t clockToBcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
inline int clockFromBcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
