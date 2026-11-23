// Host tests for src/aprs.h and src/nmea.h.  Run:
//   c++ -std=c++17 -I src test/host/test_aprs.cpp -o /tmp/test_aprs && /tmp/test_aprs
#include <cstdio>
#include <string>
#include "aprs.h"
#include "nmea.h"
#include "beacon.h"

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

  // ---- beacon packet builder: what we send must parse back to what we meant ----
  {
    unsigned char f[128];
    size_t len = aprsBuildBeacon(f, sizeof f, "K6ATV-6", "APZPGR", "WIDE1-1", 37.3417, -121.905, '/', '[', "T-LoRa Pager");
    expect(len > 0 && f[0] == 0x3C && f[1] == 0xFF && f[2] == 0x01, "beacon header");
    std::string t((char *)f + 3, len - 3);
    expect(t == "K6ATV-6>APZPGR,WIDE1-1:!3720.50N/12154.30W[T-LoRa Pager", "beacon text exact");
    AprsPos b; expect(aprsParse(f, len, b) && std::string(b.call) == "K6ATV-6" && near(b.lat, 37.3417, 2e-4) && near(b.lon, -121.905, 2e-4) && b.table == '/' && b.sym == '[', "beacon parses back");
    // southern/eastern and a minute that rounds up to 60
    char pos[24]; aprsFormatPosition(pos, sizeof pos, -33.99999, 151.0, '\\', '&');
    expect(std::string(pos) == "3400.00S\\15100.00E&", "rounding carries into the degrees");
    aprsFormatPosition(pos, sizeof pos, 0.0, 0.0, '/', '>'); expect(std::string(pos) == "0000.00N/00000.00E>", "zero");
    char tiny[8]; expect(aprsBuildBeacon((unsigned char *)tiny, sizeof tiny, "K6ATV-6", "APZPGR", "", 1, 1, '/', '[', "") == 0, "too small a buffer is refused");
    size_t l2 = aprsBuildBeacon(f, sizeof f, "K6ATV-6", "APZPGR", "", 37.0, -121.0, '/', '[', "x");
    expect(std::string((char *)f + 3, l2 - 3).rfind("K6ATV-6>APZPGR:!", 0) == 0, "no path: no comma");
  }
  // ---- beacon timing ----
  {
    BeaconScheduler s; s.start(0);
    expect(!s.due(14000, true, 37, -121), "not before the first delay");
    expect(!s.due(20000, false, 37, -121), "never without a fix");
    expect(s.due(15000, true, 37, -121), "first beacon after the delay");
    s.sent(15000, 37.0, -121.0);
    expect(!s.due(15000 + 20000, true, 37.01, -121), "not inside the minimum gap, even when moved");
    expect(!s.due(15000 + 61000, true, 37.0, -121.0), "stationary: wait the long interval");
    expect(s.due(15000 + 61000, true, 37.01, -121.0), "moved ~1 km: beacon after a minute");
    expect(!s.due(15000 + 61000, true, 37.0005, -121.0), "moved ~55 m: not enough");
    expect(s.due(15000 + 600000, true, 37.0, -121.0), "stationary beacon after 10 minutes");
    expect(s.manualOk(15000 + 5000) && !s.manualOk(15000 + 4000), "manual beacon needs 5 s since the last");
  }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
