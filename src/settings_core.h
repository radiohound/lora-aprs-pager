// Pager settings: the values, the validation rules, and the JSON for the phone page. Plain C++ (no Arduino), tested in
// test/host/test_settings.cpp. Everything a phone sends is checked here before it is used or saved.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

struct PagerSettings {
  char call[10] = "N0CALL";        // N0CALL = not set yet: the pager will not transmit until you set a real call sign
  char path[21] = "WIDE1-1";
  char table = '/', sym = '[';     // APRS symbol: person
  char comment[44] = "T-LoRa Pager";
  int txDbm = 20;
  int movingSec = 60;              // beacon at most this often while moving
  int stationaryMin = 10;          // beacon this often while standing still
  int moveMetres = 150;            // how far counts as "moved"
  bool autoAtBoot = false;         // start auto-beaconing at power-on
  bool haptic = true;
  int brightness = 160;
  int utcOffset = -8;              // hours from UTC for the clock on the screen (Pacific standard time)
  bool usDst = true;               // add one hour in US daylight saving time
  int dimSec = 60;                 // screen dims after this many seconds without a key or knob (0 = never)
  int offSec = 300;                // backlight off after this many seconds (0 = never); any key or knob wakes it at once, there is no lock screen
  bool batIcon = true;             // battery shape in the top right corner
  bool gpsSleep = true;            // while the screen is off the GPS sleeps and wakes now and then for a fix (off: it stays on, which costs battery)
  int gpsMoveSec = 60;             // GPS wake interval while the pager is moving, seconds
  int gpsStillMin = 5;             // GPS wake interval once the pager has stayed put, minutes (without the motion sensor)
  bool gpsMotion = false;          // the motion sensor (Bosch BHI260AP) wakes the GPS when the pager moves; off until the sensor wake is proven on the pager
  int gpsBackstopMin = 60;         // with the motion sensor: how often the GPS wakes anyway while nothing moves (keeps its satellite data fresh), minutes
};

namespace settings_detail {
inline bool toInt(const std::string &s, int lo, int hi, int &out) {
  if (s.empty()) return false;
  char *e = nullptr; long v = strtol(s.c_str(), &e, 10);
  if (*e || v < lo || v > hi) return false;
  out = (int)v; return true;
}
inline std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::string upper(std::string s) { for (auto &c : s) c = toupper((unsigned char)c); return s; }
}  // namespace settings_detail

// "W1AW-7": 3-6 letters/digits with at least one of each, then an optional -0..15
inline bool settingsValidCall(const std::string &c) {
  size_t dash = c.find('-'); std::string base = c.substr(0, dash);
  if (base.size() < 3 || base.size() > 6) return false;
  bool l = false, d = false;
  for (char ch : base) { if (isalpha((unsigned char)ch) && isupper((unsigned char)ch)) l = true; else if (isdigit((unsigned char)ch)) d = true; else return false; }
  if (!l || !d) return false;
  if (dash == std::string::npos) return true;
  std::string sd = c.substr(dash + 1);
  int ssid; return settings_detail::toInt(sd, 0, 15, ssid) && sd.size() <= 2 && !(sd.size() == 2 && sd[0] == '0');     // -0 to -15, no leading zero
}
// "WIDE1-1" or "WIDE1-1,WIDE2-1" or empty: up to 3 hops of A-Z, 0-9 and a dash
inline bool settingsValidPath(const std::string &p) {
  if (p.size() > 20) return false;
  if (p.empty()) return true;
  int hops = 1;
  size_t run = 0;
  for (char ch : p) {
    if (ch == ',') { if (!run) return false; hops++; run = 0; continue; }
    if (!(isupper((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '-')) return false;
    if (++run > 9) return false;
  }
  return run > 0 && hops <= 3;
}
inline bool settingsValidComment(const std::string &c) {
  if (c.size() > 43) return false;
  for (unsigned char ch : c) if (ch < 32 || ch > 126 || ch == '|' || ch == '~') return false;   // | and ~ are reserved in APRS comments
  return true;
}
inline bool settingsValidSymbol(char table, char code) {
  bool t = table == '/' || table == '\\' || (table >= '0' && table <= '9') || (table >= 'A' && table <= 'Z');
  return t && code >= 33 && code <= 126;
}

// Settings added after the first release. A pager saved by an older version has no such keys; they must read as these defaults, because if a
// field is missing the whole load fails and the pager silently falls back to factory settings (the call sign would be lost).
inline bool settingsLaterDefault(const char *k, std::string &v) {
  struct D { const char *k, *v; };
  static const D defs[] = {{"tz", "-8"}, {"dst", "1"}, {"baticon", "1"}, {"dim", "60"}, {"off", "300"}, {"gpssl", "1"}, {"gpsmv", "60"}, {"gpsst", "5"}, {"gpsmo", "0"}, {"gpsbk", "60"}};
  for (const D &d : defs) if (!strcmp(k, d.k)) { v = d.v; return true; }
  return false;
}

// Read all fields through get(name, value). Returns true and fills `out` only if EVERY field is valid; otherwise `err` says
// which one is wrong and `out` is left untouched. Fields: call path table sym comment txdbm moving stationary movem auto haptic bright tz dst baticon dim off gpssl gpsmv gpsst gpsmo gpsbk
using SettingsGet = std::function<bool(const char *, std::string &)>;
inline bool settingsParse(const SettingsGet &get, PagerSettings &out, std::string &err) {
  using namespace settings_detail;
  PagerSettings n; std::string v;
  auto need = [&](const char *name) { if (!get(name, v)) { err = std::string("Missing field: ") + name; return false; } return true; };
  if (!need("call")) return false;
  { std::string c = upper(trim(v)); if (!settingsValidCall(c)) { err = "Call sign: use letters and digits like W1AW-7 (SSID 0-15)"; return false; } strcpy(n.call, c.c_str()); }
  if (!need("path")) return false;
  { std::string p = upper(trim(v)); if (!settingsValidPath(p)) { err = "Path: up to 3 hops like WIDE1-1 or WIDE1-1,WIDE2-1"; return false; } strcpy(n.path, p.c_str()); }
  if (!need("table")) return false;
  if (v.size() != 1) { err = "Symbol table: one character"; return false; }
  n.table = v[0];
  if (!need("sym")) return false;
  if (v.size() != 1) { err = "Symbol: one character"; return false; }
  n.sym = v[0];
  if (!settingsValidSymbol(n.table, n.sym)) { err = "Symbol: table must be / or \\ or an overlay 0-9 / A-Z, and the symbol a printable character"; return false; }
  if (!need("comment")) return false;
  { std::string c = trim(v); if (!settingsValidComment(c)) { err = "Comment: up to 43 plain characters, no | or ~"; return false; } strcpy(n.comment, c.c_str()); }
  auto num = [&](const char *name, int lo, int hi, int &dst, const char *msg) {
    if (!need(name)) return false;
    if (!toInt(v, lo, hi, dst)) { err = msg; return false; }
    return true;
  };
  int b = 0;
  if (!num("txdbm", 2, 22, n.txDbm, "TX power: 2 to 22 dBm")) return false;
  if (!num("moving", 30, 3600, n.movingSec, "Moving interval: 30 to 3600 seconds")) return false;
  if (!num("stationary", 1, 120, n.stationaryMin, "Standing-still interval: 1 to 120 minutes")) return false;
  if (!num("movem", 20, 5000, n.moveMetres, "Distance: 20 to 5000 metres")) return false;
  if (!num("auto", 0, 1, b, "Auto-beacon at start: 0 or 1")) return false;
  n.autoAtBoot = b;
  if (!num("haptic", 0, 1, b, "Haptics: 0 or 1")) return false;
  n.haptic = b;
  if (!num("bright", 10, 255, n.brightness, "Brightness: 10 to 255")) return false;
  if (!num("tz", -12, 14, n.utcOffset, "Time zone: -12 to 14 hours from UTC")) return false;
  if (!num("dst", 0, 1, b, "Daylight saving: 0 or 1")) return false;
  n.usDst = b;
  if (!num("dim", 0, 3600, n.dimSec, "Dim after: 0 to 3600 seconds")) return false;
  if (!num("off", 0, 3600, n.offSec, "Screen off after: 0 to 3600 seconds")) return false;
  if (!num("baticon", 0, 1, b, "Battery icon: 0 or 1")) return false;
  n.batIcon = b;
  if (!num("gpssl", 0, 1, b, "GPS sleep: 0 or 1")) return false;
  n.gpsSleep = b;
  if (!num("gpsmv", 15, 600, n.gpsMoveSec, "GPS wake while moving: 15 to 600 seconds")) return false;
  if (!num("gpsst", 1, 60, n.gpsStillMin, "GPS wake while still: 1 to 60 minutes")) return false;
  if (!num("gpsmo", 0, 1, b, "GPS wake on motion: 0 or 1")) return false;
  n.gpsMotion = b;
  if (!num("gpsbk", 10, 240, n.gpsBackstopMin, "GPS backstop wake: 10 to 240 minutes")) return false;
  out = n; return true;
}

inline std::string settingsJsonEscape(const char *s) {
  std::string o;
  for (; *s; s++) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; }
  return o;
}
inline std::string settingsToJson(const PagerSettings &s) {
  char b[640];
  std::string cm = settingsJsonEscape(s.comment), pa = settingsJsonEscape(s.path);
  char t[3] = {s.table, 0, 0}; std::string tb = settingsJsonEscape(t); char y[2] = {s.sym, 0}; std::string sy = settingsJsonEscape(y);
  snprintf(b, sizeof b, "{\"call\":\"%s\",\"path\":\"%s\",\"table\":\"%s\",\"sym\":\"%s\",\"comment\":\"%s\",\"txdbm\":%d,\"moving\":%d,"
           "\"stationary\":%d,\"movem\":%d,\"auto\":%d,\"haptic\":%d,\"bright\":%d,\"tz\":%d,\"dst\":%d,\"baticon\":%d,\"dim\":%d,\"off\":%d,\"gpssl\":%d,\"gpsmv\":%d,\"gpsst\":%d,\"gpsmo\":%d,\"gpsbk\":%d}",
           s.call, pa.c_str(), tb.c_str(), sy.c_str(), cm.c_str(), s.txDbm, s.movingSec, s.stationaryMin, s.moveMetres,
           s.autoAtBoot ? 1 : 0, s.haptic ? 1 : 0, s.brightness, s.utcOffset, s.usDst ? 1 : 0, s.batIcon ? 1 : 0, s.dimSec, s.offSec, s.gpsSleep ? 1 : 0, s.gpsMoveSec, s.gpsStillMin, s.gpsMotion ? 1 : 0, s.gpsBackstopMin);
  return b;
}
