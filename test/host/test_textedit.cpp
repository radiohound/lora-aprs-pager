// Host tests for src/keymap.h and src/textedit.h.  Run:
//   c++ -std=c++17 -I src test/host/test_textedit.cpp -o /tmp/test_textedit && /tmp/test_textedit
#include <cstdio>
#include <string>
#include "textedit.h"
#include "emoticons.h"

static int fails = 0;
static void expect(bool c, const char *w) { if (!c) { fails++; printf("FAIL: %s\n", w); } }
// real key numbers of the pager keyboard (confirmed on hardware)
enum { Q=1, W=2, E=3, R=4, T=5, Y=6, U=7, I=8, O=9, P=97, A=11, S=12, D=13, F=14, G=15, H=16, J=17, K=18, L=19, Z=22, X=23, C=24, V=25, B=26, N=27, M=28,
       ALT=21, SHIFT=29, BKSP=30, SPACE=31, ENTER=98 };
static void tap(TextEdit &t, int k) { t.feed(k, true); t.feed(k, false); }
static void type(TextEdit &t, std::initializer_list<int> keys) { for (int k : keys) tap(t, k); }
static std::string text(const TextEdit &t) { return t.buf; }

int main() {
  { TextEdit t; type(t, {H, E, L, L, O}); expect(text(t) == "hello", "plain typing"); }
  { TextEdit t; tap(t, SHIFT); type(t, {H, E}); expect(text(t) == "He", "Shift = next letter capital, then back to lower case"); }
  { TextEdit t; t.feed(ALT, true); tap(t, Q); tap(t, P); t.feed(ALT, false); tap(t, Q); expect(text(t) == "10q", "Alt gives digits (p is 0), releasing Alt goes back"); }
  { TextEdit t; t.feed(ALT, true); t.feed(SHIFT, true); tap(t, Q); tap(t, W); tap(t, E); tap(t, R); tap(t, T); tap(t, Y); tap(t, U); tap(t, I); tap(t, O); tap(t, P);
    expect(text(t) == "()<>[]\\#%&", "Alt+Shift chord: the second layer on the top row (p is &)"); }
  { TextEdit t; t.feed(ALT, true); t.feed(SHIFT, true); tap(t, A); tap(t, S); expect(text(t) == "^`", "second layer, second row"); }
  { TextEdit t; type(t, {H, I, SPACE}); t.feed(ALT, true); tap(t, H); t.feed(SHIFT, true); tap(t, W); t.feed(SHIFT, false); t.feed(ALT, false);
    expect(text(t) == "hi :)", "':)' can be typed: Alt+h is ':' and Alt+Shift+w is ')'");
    size_t len = 0; int k = emoticonMatch(t.buf, t.len, 3, &len); expect(k >= 0 && len == 2 && emoticonIconIndex(k) == 0, "and the typed text is recognised as the smile emoticon"); }
  { TextEdit t; t.feed(ALT, true); t.feed(SHIFT, true); tap(t, E); t.feed(SHIFT, false); tap(t, E); t.feed(ALT, false);
    expect(text(t) == "<3", "'<3': Alt+Shift+e is '<', then Alt+e is '3'");
    size_t len = 0; int k = emoticonMatch(t.buf, t.len, 0, &len); expect(k >= 0 && len == 2 && std::string(EMOTICONS[k].name) == "heart", "recognised as the heart"); }
  { TextEdit t; t.feed(ALT, true); t.feed(SHIFT, true); tap(t, Q); t.feed(SHIFT, false); t.feed(ALT, false); tap(t, A);
    expect(text(t) == "(a", "after the chord, Shift has not left a capital pending"); }
  { TextEdit t; tap(t, SHIFT); t.feed(ALT, true); tap(t, Q); t.feed(ALT, false); expect(text(t) == "1", "Shift then Alt: symbol layer, no crash"); }
  { TextEdit t; type(t, {A, B}); expect(t.feed(BKSP, true) == TextEdit::CHANGED && text(t) == "a", "backspace removes one"); t.feed(BKSP, true);
    expect(t.feed(BKSP, true) == TextEdit::BACK_EMPTY, "backspace on an empty field says so"); }
  { TextEdit t; expect(t.feed(ENTER, true) == TextEdit::ENTER && text(t).empty(), "Enter is reported and never typed"); }
  { TextEdit t; tap(t, 20); tap(t, 50); tap(t, 0); expect(text(t).empty(), "matrix key 20 (Enter's second contact) and unknown keys type nothing"); }
  { TextEdit t; t.callMode = true; t.maxLen = 9; type(t, {K, SPACE, Q, A}); expect(text(t) == "KQA", "call mode: upper case, no spaces");
    t.feed(ALT, true); tap(t, F); expect(text(t) == "KQA-", "call mode: '-' is allowed"); t.feed(SHIFT, true); tap(t, Q); t.feed(SHIFT, false); t.feed(ALT, false);
    expect(text(t) == "KQA-", "call mode: '(' is refused"); t.feed(ALT, true); tap(t, Q); expect(text(t) == "KQA-1", "call mode: digits are fine"); }
  { TextEdit t; t.maxLen = 5; type(t, {A, B, C, D, E, F, G}); expect(text(t) == "abcde" && t.len == 5, "the length limit holds"); }
  { // keyLookup directly
    expect(keyLookup(P).ch == 'p' && keyLookup(P, false, true).ch == 'P' && keyLookup(P, true).ch == '0' && keyLookup(P, false, false, true).ch == '&', "p key in every layer");
    expect(keyLookup(Q, true).ch == '1' && keyLookup(Q, false, true).ch == 'Q' && keyLookup(Q, true, false, true).ch == '(', "q key in every layer");
    expect(keyLookup(SPACE).mod == KM_SPACE && keyLookup(BKSP).mod == KM_BACKSPACE && keyLookup(ENTER).mod == KM_ENTER, "special keys"); }
  { // nothing in any layer may be a character that message text refuses ({ | ~) or a control character
    bool ok = true;
    for (auto *layer : {KEYMAP_BASE, KEYMAP_SYMBOL, KEYMAP_SYMBOL2}) for (int r = 0; r < 4; r++) for (int c = 0; c < 10; c++) { char ch = layer[r][c]; if (ch == '{' || ch == '|' || ch == '~' || (ch && (unsigned char)ch < 32 && ch != '\n')) ok = false; }
    expect(ok, "no layer holds a character the messages refuse"); }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
