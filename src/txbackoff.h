// When the radio keeps failing to send: wait longer between automatic tries, and buzz only for the first failure of a run.
// A busy channel is not a failure. Plain C++, tested in test/host/test_txbackoff.cpp.
#pragma once
#include <stdint.h>

struct TxBackoff {
  int fails = 0;                       // radio failures in a row
  uint32_t failedAt = 0;
  void failed(uint32_t now) { if (fails < 1000) fails++; failedAt = now; }
  void sent() { fails = 0; }
  // 10, 20, 40, 80 s, then every 2 minutes
  uint32_t waitMs() const { if (!fails) return 0; return fails > 4 ? 120000UL : 10000UL << (fails - 1); }
  bool allowed(uint32_t now) const { return !fails || (uint32_t)(now - failedAt) >= waitMs(); }
  bool buzz() const { return fails == 1; }
};
