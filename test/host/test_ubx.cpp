#include "../../src/ubx.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  uint8_t f[80];
  // UBX-MON-VER poll, a message every u-blox document shows: B5 62 0A 04 00 00 0E 34
  size_t n = ubxFrame(0x0A, 0x04, nullptr, 0, f);
  expect(n == 8 && f[6] == 0x0E && f[7] == 0x34, "MON-VER poll checksum");
  // backup request: PMREQ, 16-byte payload, duration 0, flags = backup, wake on UART RX
  n = ubxBackup(f);
  expect(n == 24 && f[2] == 0x02 && f[3] == 0x41 && f[4] == 16 && f[5] == 0, "PMREQ header");
  expect(f[6] == 0 && f[10] == 0 && f[11] == 0 && f[12] == 0 && f[13] == 0, "PMREQ version and duration 0");
  expect(f[14] == 0x02 && f[15] == 0 && f[16] == 0 && f[17] == 0, "PMREQ backup flag");
  expect(f[18] == 0x08 && f[19] == 0 && f[20] == 0 && f[21] == 0, "PMREQ wake on UART RX");
  { uint8_t a = 0, b = 0; for (size_t i = 2; i < n - 2; i++) { a += f[i]; b += a; } expect(f[n - 2] == a && f[n - 1] == b, "PMREQ checksum"); }
  // VALSET framing, RAM layer only (kept for later configuration writes)
  { const uint32_t k[1] = {0x30210001}, v[1] = {1000}; const uint8_t z[1] = {2}; n = ubxValset(k, v, z, 1, f);
    expect(n == 8 + 4 + 6 && f[3] == 0x8A && f[6] == 0 && f[7] == 1, "VALSET header, RAM layer");
    expect(f[10] == 0x01 && f[11] == 0x00 && f[12] == 0x21 && f[13] == 0x30 && f[14] == 0xE8 && f[15] == 0x03, "VALSET key and value little-endian"); }
  // position hint: 37.5 N, 121.25 W
  n = ubxMgaPos(375000000, -1212500000, 10000000, f);
  expect(n == 28 && f[2] == 0x13 && f[3] == 0x40 && f[4] == 20 && f[6] == 0x01, "POS_LLH header");
  int32_t la = (int32_t)(f[10] | f[11] << 8 | f[12] << 16 | (uint32_t)f[13] << 24), lo = (int32_t)(f[14] | f[15] << 8 | f[16] << 16 | (uint32_t)f[17] << 24);
  expect(la == 375000000 && lo == -1212500000, "POS_LLH signed latitude and longitude");
  uint32_t acc = f[22] | f[23] << 8 | f[24] << 16 | (uint32_t)f[25] << 24; expect(acc == 10000000, "POS_LLH accuracy");
  // time hint: 2026-11-23 22:05:09 UTC
  uint32_t t = (uint32_t)(clockDays(2026, 11, 23) * 86400 + 22 * 3600 + 5 * 60 + 9);
  n = ubxMgaTime(t, 5, f);
  expect(n == 32 && f[4] == 24 && f[6] == 0x10 && f[9] == 18, "TIME_UTC header and leap seconds");
  expect((f[10] | f[11] << 8) == 2026 && f[12] == 11 && f[13] == 23 && f[14] == 22 && f[15] == 5 && f[16] == 9, "TIME_UTC date and time");
  expect((f[22] | f[23] << 8) == 5, "TIME_UTC accuracy");
  // acknowledgments
  n = ubxAckAidingOn(f);
  expect(n == 17 && f[3] == 0x8A && f[10] == 0x25 && f[11] == 0x00 && f[12] == 0x11 && f[13] == 0x10 && f[14] == 1, "ACKAIDING on: key and value");
  auto scan = [](UbxScan &s, const uint8_t *d, size_t len) { int hits = 0; for (size_t i = 0; i < len; i++) if (s.feed(d[i])) hits++; return hits; };
  { UbxScan s; uint8_t r[40]; const uint8_t ack[8] = {1, 0, 0, 0x40, 0x10, 0, 0, 18};
    const char *nmea = "$GNGGA,,,,,,0,00,99.99,,,,,,*56\r\n"; expect(scan(s, (const uint8_t *)nmea, strlen(nmea)) == 0, "NMEA text is no frame");
    size_t m = ubxFrame(0x13, 0x60, ack, 8, r); expect(scan(s, r, m) == 1, "MGA-ACK found after NMEA text");
    expect(ubxIsMgaAck(s, 0x10) && !ubxIsMgaAck(s, 0x01) && s.pl[0] == 1 && s.pl[2] == 0, "it answers the time hint, not the position hint");
    r[m - 1] ^= 1; expect(scan(s, r, m) == 0, "bad checksum rejected");
    const uint8_t cfg[2] = {0x06, 0x8A}; m = ubxFrame(0x05, 0x01, cfg, 2, r); expect(scan(s, r, m) == 1 && ubxValsetAnswer(s) == 1, "VALSET acknowledged");
    m = ubxFrame(0x05, 0x00, cfg, 2, r); expect(scan(s, r, m) == 1 && ubxValsetAnswer(s) == 0, "VALSET refused"); }
  if (!fails) printf("ubx: all ok\n");
  return fails ? 1 : 0;
}
