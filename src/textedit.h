// Typing on the pager keyboard into a text field. Plain C++ (no Arduino), tested in test/host/test_textedit.cpp.
// Letters from the keyboard; hold Alt = digits and symbols; hold Alt AND Shift = the second symbol layer ( ) < > [ ] \\ # % & ^ `;
// tap Shift (without Alt) = the next letter is a capital. Control characters never get in.
#pragma once
#include <cctype>
#include <cstring>
#include "keymap.h"

class TextEdit {
public:
  char buf[72] = ""; int len = 0, maxLen = 67; bool callMode = false, shiftOnce = false, altHeld = false, shiftHeld = false;
  enum R { NONE, CHANGED, ENTER, BACK_EMPTY };
  void clear() { len = 0; buf[0] = 0; shiftOnce = altHeld = shiftHeld = false; }
  void set(const char *s) { strncpy(buf, s, maxLen); buf[maxLen] = 0; len = (int)strlen(buf); }
  // raw = key number without the press bit, down = pressed (true) or released (false)
  R feed(uint8_t raw, bool down) {
    if (raw == KEY_ALT) { altHeld = down; return NONE; }
    if (raw == KEY_SHIFT) {
      shiftHeld = down;
      if (down && !altHeld) { shiftOnce = !shiftOnce; return CHANGED; }       // with Alt held, Shift is part of the chord, not a capital
      return NONE;
    }
    if (!down) return NONE;
    if (raw == KEY_ENTER) return ENTER;
    if (raw == KEY_BACKSPACE) { if (!len) return BACK_EMPTY; buf[--len] = 0; return CHANGED; }
    KeyInfo k = keyLookup(raw, altHeld, shiftOnce && !callMode, altHeld && shiftHeld);
    if (!k.ch || (unsigned char)k.ch < 32 || (k.mod != KM_NONE && k.mod != KM_SPACE) || len >= maxLen) return NONE;     // never a control character
    if (callMode && (k.ch == ' ' || !(isalnum((unsigned char)k.ch) || k.ch == '-'))) return NONE;
    char c = callMode ? (char)toupper((unsigned char)k.ch) : k.ch;
    buf[len++] = c; buf[len] = 0;
    if (isalpha((unsigned char)k.ch)) shiftOnce = false;                     // Shift is for ONE letter (k.ch is already the capital when it applied)
    return CHANGED;
  }
};
