// LoRa APRS frame -> station position. Plain C++ (no Arduino), tested on the Mac in test/host/test_aprs.cpp.
// Frame on the air: 3C FF 01 + TNC2 text "SRC>DST,PATH:info". Handles uncompressed and compressed positions
// (! = / @ and objects ;). Mic-E and other formats are ignored for now.
#pragma once
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstddef>

struct AprsPos {
  char call[10];      // station or object name (max 9 chars)
  double lat, lon;
  char table, sym;    // APRS symbol table and code
  bool object;
};

namespace aprs_detail {
inline bool digitOrSpace(char c) { return (c >= '0' && c <= '9') || c == ' '; }
inline int dig(char c) { return c == ' ' ? 0 : c - '0'; }
// "DDMM.mmN" -> signed degrees; returns false if malformed
inline bool uncompressedLat(const char *p, double &lat) {
  for (int i : {0, 1, 2, 3, 5, 6}) if (!digitOrSpace(p[i])) return false;
  if (p[4] != '.' || (p[7] != 'N' && p[7] != 'S')) return false;
  lat = dig(p[0]) * 10 + dig(p[1]) + (dig(p[2]) * 10 + dig(p[3]) + (dig(p[5]) * 10 + dig(p[6])) / 100.0) / 60.0;
  if (p[7] == 'S') lat = -lat;
  return lat >= -90 && lat <= 90;
}
// "DDDMM.mmE"
inline bool uncompressedLon(const char *p, double &lon) {
  for (int i : {0, 1, 2, 3, 4, 6, 7}) if (!digitOrSpace(p[i])) return false;
  if (p[5] != '.' || (p[8] != 'E' && p[8] != 'W')) return false;
  lon = dig(p[0]) * 100 + dig(p[1]) * 10 + dig(p[2]) + (dig(p[3]) * 10 + dig(p[4]) + (dig(p[6]) * 10 + dig(p[7])) / 100.0) / 60.0;
  if (p[8] == 'W') lon = -lon;
  return lon >= -180 && lon <= 180;
}
inline bool b91(const char *p, long &v) {
  v = 0;
  for (int i = 0; i < 4; i++) { if (p[i] < 33 || p[i] > 124) return false; v = v * 91 + (p[i] - 33); }
  return true;
}
// position starting at p (info text), n = chars available
inline bool position(const char *p, size_t n, AprsPos &o) {
  if (n >= 19 && (p[0] >= '0' && p[0] <= '9')) {                 // uncompressed: lat8 table lon9 sym
    if (!uncompressedLat(p, o.lat) || !uncompressedLon(p + 9, o.lon)) return false;
    o.table = p[8]; o.sym = p[18]; return true;
  }
  if (n >= 10) {                                                   // compressed: table y4 x4 sym
    long y, x; if (!b91(p + 1, y) || !b91(p + 5, x)) return false;
    o.lat = 90.0 - y / 380926.0; o.lon = -180.0 + x / 190463.0;
    o.table = p[0]; o.sym = p[9]; return true;
  }
  return false;
}
}  // namespace aprs_detail

inline bool aprsParse(const unsigned char *frame, size_t len, AprsPos &out) {
  memset(&out, 0, sizeof out);
  if (len < 8) return false;
  size_t i = 0;
  if (frame[0] == 0x3C) { if (len < 4 || frame[1] != 0xFF || frame[2] != 0x01) return false; i = 3; }   // other 3C FF xx = not plain APRS
  const char *t = (const char *)frame + i; size_t n = len - i;
  const char *gt = (const char *)memchr(t, '>', n);
  const char *colon = (const char *)memchr(t, ':', n);
  if (!gt || !colon || gt > colon || gt == t) return false;
  size_t cl = gt - t; if (cl > 9) return false;
  memcpy(out.call, t, cl); out.call[cl] = 0;
  const char *info = colon + 1; size_t m = n - (info - t);
  if (m < 1) return false;
  switch (info[0]) {
    case '!': case '=': return aprs_detail::position(info + 1, m - 1, out);
    case '/': case '@': return m > 8 && aprs_detail::position(info + 8, m - 8, out);         // 7-char timestamp first
    case ';': {                                                                              // object: ;NAME_____*DDHHMMzPOS
      if (m < 19) return false;
      char nm[10]; memcpy(nm, info + 1, 9); nm[9] = 0;
      int e = 8; while (e >= 0 && nm[e] == ' ') nm[e--] = 0;
      if (info[10] != '*' && info[10] != '_') return false;
      memcpy(out.call, nm, 10); out.object = true;
      return aprs_detail::position(info + 18, m - 18, out);
    }
    default: return false;
  }
}

// ---- Building a position beacon ----
// Format a position as the 19 characters "DDMM.mmN/DDDMM.mmW$" (table between lat and lon, symbol code last).
inline void aprsFormatPosition(char *out, size_t n, double lat, double lon, char table, char sym) {
  char ns = lat >= 0 ? 'N' : 'S', ew = lon >= 0 ? 'E' : 'W';
  double a = fabs(lat), o = fabs(lon);
  long la = (long)(a * 6000.0 + 0.5), lo = (long)(o * 6000.0 + 0.5);          // hundredths of a minute
  snprintf(out, n, "%02ld%02ld.%02ld%c%c%03ld%02ld.%02ld%c%c", la / 6000, (la / 100) % 60, la % 100, ns, table,
           lo / 6000, (lo / 100) % 60, lo % 100, ew, sym);
}
// Whole LoRa APRS frame: 3C FF 01 + "CALL>TOCALL,PATH:!position comment". Returns its length, 0 if it does not fit.
inline size_t aprsBuildBeacon(unsigned char *out, size_t n, const char *call, const char *tocall, const char *path,
                              double lat, double lon, char table, char sym, const char *comment) {
  char pos[24]; aprsFormatPosition(pos, sizeof pos, lat, lon, table, sym);
  if (n < 4) return 0;
  out[0] = 0x3C; out[1] = 0xFF; out[2] = 0x01;
  int k = snprintf((char *)out + 3, n - 3, "%s>%s%s%s:!%s%s", call, tocall, path && *path ? "," : "", path ? path : "", pos, comment ? comment : "");
  return (k > 0 && (size_t)k < n - 3) ? (size_t)k + 3 : 0;
}
