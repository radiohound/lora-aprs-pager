// Host tests for src/aprs.h and src/nmea.h.  Run:
//   c++ -std=c++17 -I src test/host/test_aprs.cpp -o /tmp/test_aprs && /tmp/test_aprs
#include <cstdio>
#include <string>
#include "aprs.h"
#include "nmea.h"

static int fails = 0;
static void expect(bool c, const char *what) { if (!c) { fails++; printf("FAIL: %s\n", what); } }
static bool near(double a, double b, double e = 1e-3) { return fabs(a - b) < e; }
static bool parse(const std::string &s, AprsPos &o, bool header = true) {
  std::string f = header ? std::string("\x3c\xff\x01") + s : s;
  return aprsParse((const unsigned char *)f.data(), f.size(), o);
}
static std::string nmeaLine(const std::string &body) {
  unsigned x = 0; for (char c : body) x ^= (unsigned char)c;
  char b[16]; snprintf(b, sizeof b, "*%02X\r\n", x); return "$" + body + b;
}

int main() {
  AprsPos p;
  // uncompressed position, no timestamp
  expect(parse("K6ATV-10>APLPIK,WIDE1-1:!3720.50N/12154.30W#hill", p), "uncompressed parses");
  expect(std::string(p.call) == "K6ATV-10", "call");
  expect(near(p.lat, 37 + 20.5 / 60) && near(p.lon, -(121 + 54.3 / 60)), "uncompressed coordinates");
  expect(p.table == '/' && p.sym == '#', "symbol");
  // '=' (messaging capable), and '@' with timestamp
  expect(parse("K6ATV-4>APLRT1:=3720.50N/12154.30W>", p) && p.sym == '>', "= type");
  expect(parse("K6ATV-4>APLRT1:@092345z3720.50N/12154.30W>x", p) && near(p.lat, 37.3417), "@ with timestamp");
  // southern and eastern hemisphere, spaces for ambiguity
  expect(parse("VK2AB>APLRT1:!3350.  S/15112.00E>", p) && near(p.lat, -(33 + 50.0 / 60)) && near(p.lon, 151 + 12.0 / 60), "S/E and ambiguity");
  // compressed position (APRS 1.0 spec example: 49.5N 72.75W)
  expect(parse("N0CALL>APRS:!/5L!!<*e7>7P[", p) && near(p.lat, 49.5, 0.01) && near(p.lon, -72.75, 0.01), "compressed");
  expect(p.table == '/' && p.sym == '>', "compressed symbol");
  // object
  expect(parse("K6ATV-3>APLPIK:;MEETHERE *092345z3720.50N/12154.30W+", p) && p.object && std::string(p.call) == "MEETHERE", "object");
  // rejects
  expect(!parse("K6ATV-4>APLRT1::K6ATV-3  :hello{1", p), "message is not a position");
  expect(!parse("K6ATV-4>APLRT1:>status text", p), "status is not a position");
  expect(!parse("K6ATV-4>APLRT1:!9999.99N/18099.99W>", p), "out of range lat");
  expect(!parse("garbage", p), "garbage");
  expect(!parse("K6ATV-4>APLRT1:!3720.50N/12154.30W>", p, true) == false, "sanity");
  { std::string f = std::string("\x3c\xff\x03") + "K6ATV-4>APLRT1:!3720.50N/12154.30W>"; expect(!aprsParse((const unsigned char *)f.data(), f.size(), p), "LACF frame ignored"); }
  expect(parse("K6ATV-4>APLRT1:!3720.50N/12154.30W>", p, false), "no header also accepted");

  // NMEA
  NmeaParser n; bool upd = false;
  for (char c : nmeaLine("GNGGA,123519.00,3720.5000,N,12154.3000,W,1,09,0.9,50.0,M,-30.0,M,,")) upd |= n.feed(c);
  expect(upd && n.fix.valid && n.fix.sats == 9, "GGA fix");
  expect(near(n.fix.lat, 37 + 20.5 / 60, 1e-4) && near(n.fix.lon, -(121 + 54.3 / 60), 1e-4), "GGA coordinates");
  for (char c : nmeaLine("GNGGA,123520.00,,,,,0,00,99.9,,,,,,")) n.feed(c);
  expect(!n.fix.valid, "no-fix GGA clears the fix");
  NmeaParser bad; std::string l = nmeaLine("GNGGA,123519.00,3720.5000,N,12154.3000,W,1,09,0.9,50.0,M,-30.0,M,,"); l[10] = '9';
  for (char c : l) bad.feed(c);
  expect(!bad.fix.valid, "bad checksum rejected");
  expect(n.lines == 2, "line count");

  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
