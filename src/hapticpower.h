// When the haptic driver (DRV2605) is powered. Enabled all the time it cost 0.47 mA of sleep current (leak test), so it is enabled only
// around a buzz: on just before one, off HAPTIC_HOLD_MS after the latest buzz started. That is longer than the longest effect used (about
// a second, not measured), and keeps it on between the clicks of a turning knob. Plain C++, tested in test/host/test_hapticpower.cpp.
#pragma once
#include <stdint.h>

static const uint32_t HAPTIC_HOLD_MS = 2000;

struct HapticPower {
  bool on = false; uint32_t last = 0;
  // A buzz is about to start. Returns true if the chip is off: enable it and set it up before playing.
  bool wake(uint32_t now) { bool was = on; on = true; last = now; return !was; }
  // Call often. Returns true once, when it is time to disable the chip.
  bool idle(uint32_t now) { if (!on || (uint32_t)(now - last) < HAPTIC_HOLD_MS) return false; on = false; return true; }
};
