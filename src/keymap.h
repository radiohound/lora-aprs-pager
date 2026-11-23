// T-LoRa Pager keyboard map. Matrix keys: TCA8418 raw number = row*10 + col + 1 (confirmed on hardware: q..o = 1..9, orange key 21,
// z=22 .. m=28, cap 29, space 31). The LAST column does not arrive as matrix keys: the chip reports it as GPIO-style events,
// raw 97 + row index (confirmed: p = 97, Enter = 98). Backspace is matrix key 30 (confirmed). Orange keys: left = 21 (Alt in LilyGO's table), right = 29 (Caps/Shift).
// Layout follows LilyGoLib's pager keymap (4 rows x 10 columns).
#pragma once
#include <Arduino.h>

enum KeyMod : uint8_t { KM_NONE, KM_ALT, KM_SHIFT, KM_BACKSPACE, KM_ENTER, KM_SPACE };

struct KeyInfo { char ch; KeyMod mod; };

// Special keys by raw number (index + 1)
static constexpr uint8_t KEY_ALT = 21, KEY_SHIFT = 29, KEY_SPACE = 31;          // matrix keys (confirmed)
static constexpr uint8_t KEY_P = 97, KEY_ENTER = 98, KEY_BACKSPACE = 30;        // P and Enter arrive as 97/98; Backspace is a normal matrix key (30), all confirmed

static const char KEYMAP_BASE[4][10] = {
  {'q','w','e','r','t','y','u','i','o','p'},
  {'a','s','d','f','g','h','j','k','l',0},        // the Enter key arrives as raw 98 (and may also close matrix key 20): never a character
  {0,  'z','x','c','v','b','n','m',0,  0},
  {' ',0,0,0,0,0,0,0,0,0}
};
// Symbol layer (while Space is held as the symbol key, as LilyGO does; to be refined by feel)
static const char KEYMAP_SYMBOL[4][10] = {
  {'1','2','3','4','5','6','7','8','9','0'},
  {'*','/','+','-','=',':','\'','"','@',0},
  {0,  '_','$',';','?','!',',','.',0,  0},
  {' ',0,0,0,0,0,0,0,0,0}
};

// raw: 1..40 from the TCA8418 (without the press bit). symbol: symbol layer active. shift: capital letters.
inline KeyInfo keyLookup(uint8_t raw, bool symbol = false, bool shift = false) {
  if (raw == KEY_P) return {symbol ? '0' : (shift ? 'P' : 'p'), KM_NONE};
  if (raw < 1 || raw > 40) { if (raw != KEY_ENTER) return {0, KM_NONE}; }
  if (raw == 20) return {0, KM_NONE};            // matrix key 20 is the Enter key's second contact; Enter itself is raw 98
  switch (raw) {
    case KEY_ENTER:     return {'\n', KM_ENTER};
    case KEY_ALT:       return {0, KM_ALT};
    case KEY_SHIFT:     return {0, KM_SHIFT};
    case KEY_BACKSPACE: return {0, KM_BACKSPACE};
    case KEY_SPACE:     return {' ', KM_SPACE};
  }
  uint8_t i = raw - 1;
  char c = (symbol ? KEYMAP_SYMBOL : KEYMAP_BASE)[i / 10][i % 10];
  if (shift && !symbol && c >= 'a' && c <= 'z') c -= 32;
  return {c, KM_NONE};
}
