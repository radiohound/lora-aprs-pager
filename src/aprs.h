// LoRa APRS frame -> station position. Plain C++ (no Arduino), tested on the Mac in test/host/test_aprs.cpp.
// Frame on the air: 3C FF 01 + TNC2 text "SRC>DST,PATH:info". Handles uncompressed and compressed positions
// (! = / @ and objects ;) and Mic-E (` and '). Other formats are ignored.
#pragma once
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstddef>

struct AprsPos {
  char call[10];      // station or object name (max 9 chars)
  double lat, lon;
  char table, sym;    // APRS symbol table and code
  bool object;
  char comment[44];   // free text after the position (made printable, cut to 43 characters); may start with a course/speed or altitude extension
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
inline void setComment(AprsPos &o, const char *c, size_t n) {
  size_t k = 0;
  for (size_t i = 0; i < n && k < sizeof o.comment - 1; i++) o.comment[k++] = (c[i] >= 32 && c[i] < 127) ? c[i] : '.';
  o.comment[k] = 0;
}
// position starting at p (info text), n = chars available
inline bool position(const char *p, size_t n, AprsPos &o) {
  if (n >= 19 && (p[0] >= '0' && p[0] <= '9')) {                 // uncompressed: lat8 table lon9 sym
    if (!uncompressedLat(p, o.lat) || !uncompressedLon(p + 9, o.lon)) return false;
    o.table = p[8]; o.sym = p[18]; setComment(o, p + 19, n - 19); return true;
  }
  if (n >= 10) {                                                   // compressed: table y4 x4 sym
    long y, x; if (!b91(p + 1, y) || !b91(p + 5, x)) return false;
    o.lat = 90.0 - y / 380926.0; o.lon = -180.0 + x / 190463.0;
    o.table = p[0]; o.sym = p[9]; setComment(o, p + (n >= 13 ? 13 : 10), n >= 13 ? n - 13 : n - 10); return true;
  }
  return false;
}
// Mic-E: the latitude is hidden in the six destination characters, the longitude and speed/course are bytes in the info field.
// Same rules as Dire Wolf's decoder. dest = the destination call (at least 6 characters), info = text after the colon (info[0] is ` or ').
inline int miceDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'J') return c - 'A';
  if (c >= 'P' && c <= 'Y') return c - 'P';
  return 0;                                                       // K, L, Z = space (position ambiguity): counts as 0
}
inline bool miceDestChar(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'L') || (c >= 'P' && c <= 'Z'); }
inline bool miceHigh(char c) { return c >= 'P' && c <= 'Z'; }
inline bool micE(const char *dest, size_t dl, const unsigned char *info, size_t m, AprsPos &o) {
  if (dl < 6 || m < 9) return false;
  for (int i = 0; i < 6; i++) if (!miceDestChar(dest[i])) return false;
  double lat = miceDigit(dest[0]) * 10 + miceDigit(dest[1]) + (miceDigit(dest[2]) * 1000 + miceDigit(dest[3]) * 100 + miceDigit(dest[4]) * 10 + miceDigit(dest[5])) / 6000.0;
  int amb = 0; for (int i = 5; i >= 2 && (dest[i] == 'K' || dest[i] == 'L' || dest[i] == 'Z'); i--) amb++;   // blanked digits: the position is only known to a box
  double halfBox = amb >= 4 ? 0.5 : (amb ? 0.05 * pow(10.0, amb - 1) / 60.0 : 0);                                                 // half its size in degrees: report the middle of the box
  lat += halfBox;
  if (!miceHigh(dest[3])) lat = -lat;                              // 0-9 and A-L: south; P-Z: north
  int d = info[1] - 28; if (miceHigh(dest[4])) d += 100;             // longitude offset
  if (d >= 190) d -= 190; else if (d >= 180) d -= 80;
  int mi = info[2] - 28; if (mi >= 60) mi -= 60;
  int hu = info[3] - 28;
  if (info[1] < 28 || info[2] < 28 || info[3] < 28 || d < 0 || d > 179 || mi < 0 || mi > 59 || hu < 0 || hu > 99) return false;
  double lonMin = mi + hu / 100.0;
  if (amb) { double u = amb >= 4 ? 60.0 : 0.1 * pow(10.0, amb - 1); lonMin = floor(lonMin / u + 1e-9) * u + u / 2; }   // the longitude is blurred the same way: cut to the box, then its middle
  double lon = d + lonMin / 60.0;
  if (miceHigh(dest[5])) lon = -lon;                               // 0-9 and A-L: east; P-Z: west
  if (lat < -90 || lat > 90) return false;
  o.lat = lat; o.lon = lon; o.sym = (char)info[7]; o.table = (char)info[8];
  if (o.table != '/' && o.table != '\\' && !(o.table >= 'A' && o.table <= 'Z') && !(o.table >= '0' && o.table <= '9')) o.table = '/';
  int sp = (info[4] - 28) * 10 + (info[5] - 28) / 10; if (sp >= 800) sp -= 800;      // knots
  int co = ((info[5] - 28) % 10) * 100 + (info[6] - 28); if (co >= 400) co -= 400;   // degrees, 0 = unknown
  char com[64]; size_t k = 0;
  if (sp > 0 || co > 0) k = snprintf(com, sizeof com, "%03d/%03d", co == 360 ? 0 : co, sp);   // course/speed like other APRS comments
  const char *c = (const char *)info + 9; size_t cn = m - 9;
  while (cn && (c[cn - 1] == '\r' || c[cn - 1] == '\n')) cn--;
  char lead = 0;
  if (cn && (*c == ' ' || *c == '>' || *c == ']' || *c == '`' || *c == '\'')) { lead = *c; c++; cn--; }   // the radio-type character
  if (cn >= 4 && c[3] == '}') { c += 4; cn -= 4; }                                                          // altitude: three base-91 characters and }
  if (cn >= 2 && c[cn - 2] == '_') cn -= 2;                                                                 // Yaesu type suffix
  else if (cn >= 1 && (lead == '>' || lead == ']') && (c[cn - 1] == '=' || c[cn - 1] == '^')) cn--;       // Kenwood type suffix
  char all[64]; size_t al = 0; memcpy(all, com, k); al = k;
  if (k && cn && c[0] != ' ') all[al++] = ' ';                                                              // keep "151/000" apart from the text
  if (al + cn >= sizeof all) cn = sizeof all - 1 - al;
  if (al + cn >= sizeof o.comment) cn = sizeof o.comment - 1 - al;
  memcpy(all + al, c, cn); setComment(o, all, al + cn);
  return true;
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
    case '`': case '\'': {                                                                    // Mic-E: the position is in the destination field
      const char *d = gt + 1; size_t dl = 0; while (d + dl < colon && d[dl] != ',') dl++;
      return aprs_detail::micE(d, dl, (const unsigned char *)info, m, out);
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

// ---- How a packet reached us ----
// From the frame text (after the 3C FF 01 header) "SRC>DST,HOP1,HOP2*:info": path = what follows the destination ("" if none), via = the LAST hop
// marked with "*" (the digipeater that repeated it, without the star), "" if no hop has the star (heard directly, or the path is not used up yet).
inline void aprsPathInfo(const char *t, size_t n, char *path, size_t pathCap, char *via, size_t viaCap) {
  path[0] = 0; via[0] = 0;
  const char *colon = (const char *)memchr(t, ':', n); if (!colon) return;
  const char *comma = (const char *)memchr(t, ',', colon - t); if (!comma) return;
  size_t pl = colon - comma - 1; if (pl >= pathCap) pl = pathCap - 1;
  memcpy(path, comma + 1, pl); path[pl] = 0;
  const char *hopStart = path;
  for (const char *c = path;; c++) {
    if (*c == ',' || *c == 0) {
      size_t hl = c - hopStart;
      if (hl > 1 && hopStart[hl - 1] == '*') { size_t k = std::min(hl - 1, viaCap - 1); memcpy(via, hopStart, k); via[k] = 0; }
      if (*c == 0) break;
      hopStart = c + 1;
    }
  }
}
