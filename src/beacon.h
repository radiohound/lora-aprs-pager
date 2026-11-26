// When to send a position beacon. Plain C++, tested in test/host/test_aprs.cpp.
// Rules: needs a GPS fix; the first beacon comes FIRST_DELAY_MS after auto-beaconing is switched on (or the fix appears);
// then every movingMs while you have moved at least moveM metres since the last one, else every stationaryMs;
// never closer together than MIN_GAP_MS. A manual beacon only has to respect MANUAL_GAP_MS.
#pragma once
#include <cstdint>
#include <cmath>

class BeaconScheduler {
public:
  static constexpr uint32_t FIRST_DELAY_MS = 15000, MIN_GAP_MS = 30000, MANUAL_GAP_MS = 5000;
  uint32_t movingMs = 60000, stationaryMs = 600000;   // set from the settings
  double moveM = 200.0;

  void start(uint32_t now) { _armedAt = now; _sent = false; }
  void sent(uint32_t now, double lat, double lon) { _sent = true; _last = now; _lat = lat; _lon = lon; }
  bool manualOk(uint32_t now) const { return !_sent || (uint32_t)(now - _last) >= MANUAL_GAP_MS; }
  bool due(uint32_t now, bool fix, double lat, double lon) const {
    if (!fix) return false;
    if (!_sent) return (uint32_t)(now - _armedAt) >= FIRST_DELAY_MS;
    uint32_t since = now - _last;
    if (since < MIN_GAP_MS) return false;
    if (since >= stationaryMs) return true;
    return since >= movingMs && metres(lat, lon) >= moveM;
  }
  uint32_t lastSentAgeMs(uint32_t now) const { return _sent ? now - _last : 0xFFFFFFFFu; }
  bool hasSent() const { return _sent; }
private:
  uint32_t _armedAt = 0, _last = 0; bool _sent = false; double _lat = 0, _lon = 0;
  double metres(double lat, double lon) const {       // flat-earth distance, fine for a few hundred metres
    double dy = (lat - _lat) * 111320.0, dx = (lon - _lon) * 111320.0 * cos(_lat * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
  }
};
