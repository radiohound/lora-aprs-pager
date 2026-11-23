// Knob (quadrature on GPIO 40/41, press on GPIO 7) and the TCA8418 keyboard (I2C 0x34), polled.
// Bring-up only: it reports raw knob steps and raw key codes so we can see what the hardware sends.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "pins.h"

class Knob {
public:
  int32_t pos = 0;          // +1 per detent step in one direction, -1 in the other (direction unverified)
  bool pressed = false;
  void begin() {
    pinMode(PIN_ROT_A, INPUT_PULLUP); pinMode(PIN_ROT_B, INPUT_PULLUP); pinMode(PIN_ROT_PRESS, INPUT_PULLUP);
    _last = state();
  }
  // returns -1, 0, +1 for this call
  int poll() {
    pressed = digitalRead(PIN_ROT_PRESS) == LOW;
    uint8_t s = state();
    if (s == _last) return 0;
    // Gray-code transition table: index = (last<<2)|now, value = step
    static const int8_t T[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    _acc += T[(_last << 2) | s];
    _last = s;
    if (_acc >= 4)  { _acc = 0; pos++; return 1; }    // one detent = 4 quarter steps
    if (_acc <= -4) { _acc = 0; pos--; return -1; }
    return 0;
  }
private:
  uint8_t _last = 0; int8_t _acc = 0;
  static uint8_t state() { return (digitalRead(PIN_ROT_A) << 1) | digitalRead(PIN_ROT_B); }
};

// TCA8418: 4 rows x 10 columns assumed (rows R0-R3, columns C0-C9). Event byte: bit7 = press, bits 0-6 = key number.
class Keys {
public:
  bool begin() {
    Wire.beginTransmission(ADDR_TCA8418);
    if (Wire.endTransmission() != 0) return false;
    wr(0x1D, 0x0F);   // KP_GPIO1: rows 0-3 are keypad rows
    wr(0x1E, 0xFF);   // KP_GPIO2: columns 0-7
    wr(0x1F, 0x03);   // KP_GPIO3: columns 8-9
    wr(0x01, 0x11);   // CFG: key event interrupt enable + overflow mode
    wr(0x02, 0xFF);   // clear interrupt flags
    return _ok = true;
  }
  // Returns key event (bit7 = press, 0-6 = key number), or 0 when none waiting.
  uint8_t poll() {
    if (!_ok) return 0;
    uint8_t cnt = rd(0x03) & 0x0F;
    if (!cnt) return 0;
    uint8_t e = rd(0x04);
    wr(0x02, 0xFF);
    return e;
  }
private:
  bool _ok = false;
  static void wr(uint8_t r, uint8_t v) { Wire.beginTransmission(ADDR_TCA8418); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
  static uint8_t rd(uint8_t r) {
    Wire.beginTransmission(ADDR_TCA8418); Wire.write(r); Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ADDR_TCA8418, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
  }
};
