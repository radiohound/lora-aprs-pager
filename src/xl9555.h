// Minimal XL9555 16-bit I/O expander driver (registers: in 0/1, out 2/3, pol 4/5, cfg 6/7).
#pragma once
#include <Arduino.h>
#include <Wire.h>

class XL9555 {
public:
  bool begin(uint8_t addr = 0x20) {
    _addr = addr;
    Wire.beginTransmission(_addr);
    if (Wire.endTransmission() != 0) return false;
    _out = readPair(0x02);
    _cfg = readPair(0x06);
    return true;
  }
  // Drive a pin as an output. Level is applied BEFORE the pin becomes an output
  // so it never glitches through the old output latch value.
  void output(uint8_t pin, bool level) {
    set(_out, pin, level);
    writePair(0x02, _out);
    _cfg &= ~(1u << pin);
    writePair(0x06, _cfg);
  }
  void input(uint8_t pin) { _cfg |= (1u << pin); writePair(0x06, _cfg); }
  bool read(uint8_t pin) { return (readPair(0x00) >> pin) & 1; }
private:
  uint8_t _addr = 0x20;
  uint16_t _out = 0xFFFF, _cfg = 0xFFFF;
  static void set(uint16_t &v, uint8_t pin, bool on) { if (on) v |= (1u << pin); else v &= ~(1u << pin); }
  uint16_t readPair(uint8_t reg) {
    Wire.beginTransmission(_addr); Wire.write(reg); Wire.endTransmission(false);
    Wire.requestFrom(_addr, (uint8_t)2);
    uint16_t lo = Wire.read(), hi = Wire.read();
    return lo | (hi << 8);
  }
  void writePair(uint8_t reg, uint16_t v) {
    Wire.beginTransmission(_addr); Wire.write(reg); Wire.write(v & 0xFF); Wire.write(v >> 8);
    Wire.endTransmission();
  }
};
