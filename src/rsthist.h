// A restart history kept in flash, so it survives the RST button and a flat battery (the logs in RTC memory do not). One record per start:
// when, why, and how long the run before it lasted. Plain C++, tested in test/host/test_rsthist.cpp.
#pragma once
#include <stdint.h>
#include <string.h>

static const int RSTH_N = 10;
static const uint16_t RSTH_VER = 1;
struct RstRec { uint32_t utc; uint16_t ranMin; uint8_t reason, stage; };     // utc 0 = clock not set; ranMin: the run before, minutes (at least this)
struct RstHist { uint16_t ver, starts, crashes; uint8_t count, next; RstRec r[RSTH_N]; };

// ESP32 reset reasons. 4 crash, 5 interrupt watchdog, 6 task watchdog, 7 other watchdog; 9 is a supply dip (not counted as a crash).
inline bool rstIsCrash(int reason) { return reason >= 4 && reason <= 7; }
inline const char *rstName(int reason) {
  switch (reason) {
    case 1: return "power on / RST button";          // this chip reports both the same way
    case 2: return "reset pin";
    case 3: return "restart command";
    case 4: return "CRASH";
    case 5: return "CRASH (interrupt watchdog)";
    case 6: return "CRASH (stuck: task watchdog)";
    case 7: return "CRASH (watchdog)";
    case 8: return "woke from deep sleep";
    case 9: return "BROWNOUT (supply dipped)";
    default: return "unknown";
  }
}
inline void rstClear(RstHist &h) { memset(&h, 0, sizeof h); h.ver = RSTH_VER; }
inline bool rstValid(const RstHist &h) { return h.ver == RSTH_VER && h.count <= RSTH_N && h.next < RSTH_N; }
inline void rstAdd(RstHist &h, uint32_t utc, int reason, int stage, uint32_t ranMin) {
  if (!rstValid(h)) rstClear(h);
  RstRec &e = h.r[h.next]; e.utc = utc; e.reason = (uint8_t)reason; e.stage = (uint8_t)stage; e.ranMin = ranMin > 65535 ? 65535 : (uint16_t)ranMin;
  h.next = (h.next + 1) % RSTH_N; if (h.count < RSTH_N) h.count++;
  if (h.starts < 65535) h.starts++;
  if (rstIsCrash(reason) && h.crashes < 65535) h.crashes++;
}
// i = 0 is the newest; i < h.count
inline const RstRec &rstGet(const RstHist &h, int i) { return h.r[(h.next + RSTH_N - 1 - i) % RSTH_N]; }
