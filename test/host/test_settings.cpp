// Host tests for src/settings_core.h.  Run:  c++ -std=c++17 -I src test/host/test_settings.cpp -o /tmp/test_settings && /tmp/test_settings
#include <cstdio>
#include <map>
#include "settings_core.h"
static int fails = 0;
static void expect(bool c, const char *w) { if (!c) { fails++; printf("FAIL: %s\n", w); } }
using M = std::map<std::string, std::string>;
static M good() { return {{"call","k6atv-6"},{"path","wide1-1"},{"table","/"},{"sym","["},{"comment","T-LoRa Pager"},{"txdbm","20"},{"moving","60"},{"stationary","10"},{"movem","150"},{"auto","0"},{"haptic","1"},{"bright","160"},{"tz","-8"},{"dst","1"},{"baticon","1"},{"dim","60"},{"off","300"},{"gpssl","1"},{"gpsmv","60"},{"gpsst","5"},{"gpsmo","0"},{"gpsbk","60"},{"rxd","0"},{"slow40","0"},{"gpsoff","0"}}; }
static bool parse(const M &m, PagerSettings &s, std::string &err) { return settingsParse([&](const char *k, std::string &v) { auto i = m.find(k); if (i == m.end()) return false; v = i->second; return true; }, s, err); }
int main() {
  PagerSettings s; std::string err;
  expect(parse(good(), s, err), "good form parses");
  expect(std::string(s.call) == "K6ATV-6" && std::string(s.path) == "WIDE1-1", "call and path are upper-cased");
  expect(s.table == '/' && s.sym == '[' && s.txDbm == 20 && s.movingSec == 60 && s.stationaryMin == 10 && s.moveMetres == 150 && !s.autoAtBoot && s.haptic && s.brightness == 160, "values");
  auto bad = [&](const char *field, const char *val, const char *what) { M m = good(); m[field] = val; PagerSettings t = s; t.txDbm = 7; std::string e; expect(!parse(m, t, e) && t.txDbm == 7 && !e.empty(), what); };
  bad("call", "", "empty call refused"); bad("call", "K6", "short call refused"); bad("call", "ABCDEFG-1", "long call refused");
  bad("call", "KKKKK", "call without digit refused"); bad("call", "12345", "call without letter refused"); bad("call", "K6ATV-16", "SSID 16 refused");
  bad("call", "K6ATV-", "empty SSID refused"); bad("call", "K6 ATV", "space in call refused"); bad("call", "K6ATV-1x", "non-numeric SSID refused");
  bad("path", "WIDE1-1,WIDE2-1,WIDE3-1,WIDE4-1", "four hops refused"); bad("path", "wide1/1", "slash in path refused"); bad("path", "WIDE1-1,", "trailing comma refused");
  bad("path", "VERYLONGHOPNAME", "long hop refused");
  bad("table", "", "empty table refused"); bad("table", "ab", "two-char table refused"); bad("table", "!", "bad table refused"); bad("sym", " ", "space symbol refused"); bad("sym", "\x7f", "DEL symbol refused");
  bad("comment", "has|bar", "pipe refused"); bad("comment", "has~tilde", "tilde refused"); bad("comment", "line\nbreak", "newline refused");
  bad("comment", "0123456789012345678901234567890123456789012345", "long comment refused");
  bad("txdbm", "23", "23 dBm refused"); bad("txdbm", "1", "1 dBm refused"); bad("txdbm", "abc", "non-number refused"); bad("txdbm", "20x", "trailing junk refused"); bad("txdbm", "", "empty number refused");
  bad("moving", "29", "moving too fast refused"); bad("moving", "3601", "moving too slow refused"); bad("stationary", "0", "0 min refused"); bad("stationary", "121", "121 min refused");
  bad("gpssl", "2", "GPS sleep 2 refused"); bad("gpsmv", "14", "GPS move wake 14 s refused"); bad("gpsmv", "601", "GPS move wake 601 s refused");
  bad("gpsst", "0", "GPS still wake 0 min refused"); bad("gpsst", "61", "GPS still wake 61 min refused"); bad("gpsmv", "x", "GPS move wake non-number refused");
  bad("movem", "19", "19 m refused"); bad("auto", "2", "auto 2 refused"); bad("bright", "9", "dark refused"); bad("bright", "256", "256 refused"); bad("tz", "15", "tz 15 refused"); { M m = good(); m["call"] = "W1AW-01"; std::string e; PagerSettings t; expect(!parse(m, t, e), "SSID with a leading zero refused"); } bad("tz", "-13", "tz -13 refused"); bad("dst", "2", "dst 2 refused"); bad("baticon", "2", "baticon 2 refused"); bad("dim", "-1", "dim -1 refused"); bad("off", "3601", "off 3601 refused");
  { expect(s.gpsSleep && s.gpsMoveSec == 60 && s.gpsStillMin == 5, "GPS sleep values parsed");
    M m = good(); m["gpssl"] = "0"; m["gpsmv"] = "15"; m["gpsst"] = "60"; PagerSettings t; std::string e; expect(parse(m, t, e) && !t.gpsSleep && t.gpsMoveSec == 15 && t.gpsStillMin == 60, "GPS sleep edge values accepted");
    std::string j = settingsToJson(t); expect(j.find("\"gpssl\":0") != std::string::npos && j.find("\"gpsmv\":15") != std::string::npos && j.find("\"gpsst\":60") != std::string::npos, "JSON carries the GPS sleep settings");
    { M m2 = good(); m2["gpsmo"] = "1"; m2["gpsbk"] = "240"; PagerSettings u; std::string e2; expect(parse(m2, u, e2) && u.gpsMotion && u.gpsBackstopMin == 240, "motion wake settings parsed");
      std::string j2 = settingsToJson(u); expect(j2.find("\"gpsmo\":1") != std::string::npos && j2.find("\"gpsbk\":240") != std::string::npos, "JSON carries the motion wake settings");
      M m3 = good(); m3["rxd"] = "1"; PagerSettings r; std::string e3; expect(parse(m3, r, e3) && r.rxDuty && settingsToJson(r).find("\"rxd\":1") != std::string::npos, "burst receive setting parsed and in the JSON");
      PagerSettings d3; expect(!d3.rxDuty, "default: burst receive off");
      PagerSettings d2; expect(!d2.gpsMotion && d2.gpsBackstopMin == 60, "defaults: motion wake off, 60 min backstop"); }
    PagerSettings d; expect(d.gpsSleep && d.gpsMoveSec == 60 && d.gpsStillMin == 5, "defaults: GPS sleeps, 60 s moving, 5 min still"); }
  { // a pager saved by the first release has none of the later settings: they must read as defaults and everything else must load
    M old = good(); for (const char *k : {"tz", "dst", "baticon", "dim", "off", "gpssl", "gpsmv", "gpsst", "gpsmo", "gpsbk", "rxd", "slow40", "gpsoff"}) old.erase(k);
    PagerSettings t; std::string e;
    bool ok = settingsParse([&](const char *k, std::string &v) { auto i = old.find(k); if (i != old.end()) { v = i->second; return true; } return settingsLaterDefault(k, v); }, t, e);
    expect(ok && std::string(t.call) == "K6ATV-6" && t.gpsSleep && t.gpsMoveSec == 60 && t.gpsStillMin == 5 && t.offSec == 300 && t.dimSec == 60 && t.utcOffset == -8 && t.usDst && t.batIcon, "settings saved by an older version load with the defaults for the new ones");
    std::string dummy; expect(!settingsLaterDefault("call", dummy) && !settingsLaterDefault("bright", dummy), "no silent default for the original fields"); }
  { M m = good(); m.erase("bright"); std::string e; PagerSettings t; expect(!parse(m, t, e) && e.find("bright") != std::string::npos, "missing field named in the error"); }
  { M m = good(); m["table"] = "L"; m["sym"] = "&"; PagerSettings t; std::string e; expect(parse(m, t, e) && t.table == 'L' && t.sym == '&', "overlay symbol L& accepted"); }
  { M m = good(); m["table"] = "\\"; m["sym"] = "&"; PagerSettings t; std::string e; expect(parse(m, t, e) && t.table == '\\', "alternate table accepted"); }
  { M m = good(); m["comment"] = ""; m["path"] = ""; PagerSettings t; std::string e; expect(parse(m, t, e) && t.comment[0] == 0 && t.path[0] == 0, "empty comment and path are fine"); }
  { M m = good(); m["comment"] = "  spaced  "; PagerSettings t; std::string e; expect(parse(m, t, e) && std::string(t.comment) == "spaced", "comment is trimmed"); }
  // JSON round trip including characters that need escaping
  { PagerSettings t; strcpy(t.comment, "say \"hi\" \\o/"); std::string j = settingsToJson(t);
    expect(j.find("\"comment\":\"say \\\"hi\\\" \\\\o/\"") != std::string::npos, "JSON escapes quotes and backslashes");
    t.table = '\\'; j = settingsToJson(t); expect(j.find("\"table\":\"\\\\\"") != std::string::npos, "JSON escapes a backslash table"); }
  expect(std::string(PagerSettings().call) == "N0CALL", "default call is N0CALL");
  { M m4 = good(); m4["slow40"] = "1"; PagerSettings u4; std::string e4; expect(parse(m4, u4, e4) && u4.slow40, "slow processor setting parsed");
    std::string j4 = settingsToJson(u4); expect(j4.find("\"slow40\":1,") != std::string::npos, "JSON carries it");
    PagerSettings d4; expect(!d4.slow40, "slow processor is off by default"); }
  { M m5 = good(); m5["gpsoff"] = "1"; PagerSettings u5; std::string e5; expect(parse(m5, u5, e5) && u5.gpsPowerOff, "GPS power off setting parsed");
    std::string j5 = settingsToJson(u5); expect(j5.find("\"gpsoff\":1}") != std::string::npos, "JSON carries it, and is not cut off");
    PagerSettings d5; expect(!d5.gpsPowerOff, "GPS power off is off by default"); }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
