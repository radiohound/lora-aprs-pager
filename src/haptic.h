// Haptic feedback through the DRV2605 (I2C 0x5A, enabled by expander bit XP_DRV_EN).
// Register use follows LilyGoLib's initDrv(): effect library 1, internal-trigger mode, one effect per click.
// Effect numbers are the DRV2605 built-in ones (1..117); the defaults below are guesses to be tuned by feel on the real device.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "pins.h"
#include "xl9555.h"
#include "buzzquiet.h"

enum HapticKind : uint8_t { H_TICK, H_CLICK, H_MESSAGE, H_ACK, H_ERROR, H_KINDS };

class Haptic {
public:
  bool enabled = true;     // user setting (to be saved in our own NVS namespace later)

  // Call after Wire.begin() and the expander is up. Returns false if the chip does not answer.
  bool begin(XL9555 &xp) {
    xp.output(XP_DRV_EN, HIGH);
    delay(2);
    Wire.beginTransmission(ADDR_DRV2605);
    if (Wire.endTransmission() != 0) { xp.output(XP_DRV_EN, LOW); return false; }
    _xp = &xp;
    wr(0x01, 0x00);        // mode: internal trigger, out of standby
    wr(0x03, 0x01);        // effect library 1
    _ok = true;
    return true;
  }

  bool ok() const { return _ok; }

  // True for a few seconds after a buzz: the motion sensor feels it (buzzquiet.h).
  bool shaking(uint32_t now) { return _quiet.quiet(now); }

  // Play one pattern. A tick is rate limited so a fast knob spin does not buzz constantly,
  // and nothing plays while the radio is sending (the motor draws a current burst).
  void play(HapticKind k, bool radioBusy = false) {
    if (!_ok || !enabled || radioBusy) return;
    uint32_t now = millis();
    if (k == H_TICK) {
      if (now - _lastTick < TICK_MIN_MS) return;
      _lastTick = now;
    }
    _quiet.buzzed(now);
    switch (k) {
      case H_TICK:    seq(EFFECT_TICK, 0, 0);                        break;
      case H_CLICK:   seq(EFFECT_CLICK, 0, 0);                       break;
      case H_MESSAGE: seq(EFFECT_BUZZ, EFFECT_BUZZ, 0);              break;  // double buzz
      case H_ACK:     seq(EFFECT_CLICK, 0, 0);                       break;
      case H_ERROR:   seq(EFFECT_LONG, 0, 0);                        break;
      default: break;
    }
  }

  // Switch the motor driver off (saves power); begin() turns it on again.
  void off() { if (_xp) _xp->output(XP_DRV_EN, LOW); _ok = false; }

private:
  static constexpr uint32_t TICK_MIN_MS = 40;
  static constexpr uint8_t EFFECT_TICK = 7;    // soft click
  static constexpr uint8_t EFFECT_CLICK = 1;   // strong click (LilyGO's default)
  static constexpr uint8_t EFFECT_BUZZ = 47;   // short buzz
  static constexpr uint8_t EFFECT_LONG = 16;   // long buzz
  XL9555 *_xp = nullptr;
  bool _ok = false;
  uint32_t _lastTick = 0;
  BuzzQuiet _quiet;

  void wr(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(ADDR_DRV2605); Wire.write(reg); Wire.write(v); Wire.endTransmission();
  }
  // Up to 3 effects in the waveform sequencer, 0 ends the list, then GO.
  void seq(uint8_t a, uint8_t b, uint8_t c) {
    wr(0x04, a); wr(0x05, b); wr(0x06, c); wr(0x07, 0);
    wr(0x0C, 0x01);
  }
};
