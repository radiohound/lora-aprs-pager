#include "../../src/clock.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  int64_t t = clockDays(2026, 11, 23) * 86400 + 22 * 3600 + 5 * 60;     // 2026-11-23 22:05 UTC, a Monday
  LocalTime u = clockBreak(t); char b[32]; clockFmt(b, sizeof b, u);
  expect(!strcmp(b, "Mon 23 Nov 22:05"), "UTC format");
  LocalTime p = clockLocal(t, -8, true); clockFmt(b, sizeof b, p);
  expect(!strcmp(b, "Mon 23 Nov 14:05"), "Pacific standard time in November");
  int64_t j = clockDays(2026, 7, 1) * 86400 + 20 * 3600;                // July: daylight time
  p = clockLocal(j, -8, true); expect(p.hour == 13 && p.day == 1, "Pacific daylight time in July");
  p = clockLocal(j, -8, false); expect(p.hour == 12, "DST off: standard time stays");
  // 2026: DST starts Sun 8 Mar 02:00 PST (10:00 UTC), ends Sun 1 Nov 02:00 PDT (09:00 UTC)
  int64_t s = clockDays(2026, 3, 8) * 86400 + 10 * 3600;
  expect(!clockUsDst(s - 1, -8) && clockUsDst(s, -8), "DST start instant");
  int64_t e = clockDays(2026, 11, 1) * 86400 + 9 * 3600;
  expect(clockUsDst(e - 1, -8) && !clockUsDst(e, -8), "DST end instant");
  LocalTime d = clockBreak(0); expect(d.year == 1970 && d.wday == 4, "epoch is a Thursday");
  LocalTime neg = clockLocal(3600, -8, false); expect(neg.year == 1969 && neg.month == 12 && neg.day == 31 && neg.hour == 17, "before 1970");
  { int64_t now = clockDays(2026, 11, 23) * 86400 + 22 * 3600 + 5 * 60;                   // 14:05 Pacific on 23 Nov
    char s[24]; clockStamp(s, sizeof s, now - 3600, now, -8, true); expect(!strcmp(s, "13:05"), "stamp today");
    clockStamp(s, sizeof s, now - 86400, now, -8, true); expect(!strcmp(s, "Nov 22 14:05"), "stamp yesterday has the date");
    clockStamp(s, sizeof s, now, 0, -8, true); expect(!strcmp(s, "Nov 23 14:05"), "stamp with no clock has the date");
    int64_t late = clockDays(2026, 11, 24) * 86400 + 5 * 3600;                           // 21:00 on the 23rd Pacific, 05:00 UTC on the 24th
    clockStamp(s, sizeof s, late, late + 60, -8, true); expect(!strcmp(s, "21:00"), "stamp uses the local date, not the UTC date"); }
  expect(clockToBcd(59) == 0x59 && clockFromBcd(0x23) == 23, "BCD");
  printf(fails ? "%d failed\n" : "clock: all passed\n", fails); return fails != 0;
}
