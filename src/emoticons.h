// Default emoticon list: ASCII on the air, a graphic icon on the pager's screen.
// Pure C++ (no Arduino dependency) so it can be unit-tested on a PC: see test/host/test_emoticons.cpp.
// The ASCII is what is sent and stored; the icon is display only (src/emoji18.bin, made by tools/make_emoji.py from this list).
#pragma once
#include <stddef.h>
#include <string.h>
#include <ctype.h>

struct Emoticon { const char *ascii; const char *name; };

// Order matters only for the icon index. Longer strings are tried first by emoticonMatch().
static const Emoticon EMOTICONS[] = {
  {":)", "smile"},       {":-)", "smile"},
  {":(", "sad"},         {":-(", "sad"},
  {";)", "wink"},        {";-)", "wink"},
  {":D", "grin"},        {":-D", "grin"},
  {"XD", "laughing"},
  {":P", "tongue"},      {":-P", "tongue"},
  {":O", "surprised"},   {":-O", "surprised"},
  {":|", "neutral"},     {":-|", "neutral"},
  {":/", "unsure"},      {":-/", "unsure"},
  {":'(", "crying"},     {"T_T", "crying hard"},
  {":*", "kiss"},
  {"<3", "heart"},       {"</3", "broken heart"},
  {"(y)", "thumbs up"},  {"(n)", "thumbs down"},
  {"o/", "wave"},        {"\\o/", "cheer"},
  {"B)", "cool"},        {"8-)", "cool"},
  {"O:)", "angel"},      {">:(", "angry"},
  {"-_-", "unimpressed"},{"^_^", "happy"},
  {":3", "cute"},
};
static const size_t EMOTICON_COUNT = sizeof(EMOTICONS) / sizeof(EMOTICONS[0]);

// Does an emoticon start at s[pos]? It must be the longest list entry matching there, preceded by the start of
// the text or whitespace, and followed by the end, whitespace or one of . , ! ?  (so "10:30", "http://x",
// "(c)" and "re:)" are NOT matched). Returns the list index (or -1) and sets *len to the matched length.
static inline int emoticonMatch(const char *s, size_t n, size_t pos, size_t *len) {
  if (pos > 0 && !isspace((unsigned char)s[pos - 1])) return -1;
  int best = -1; size_t bestLen = 0;
  for (size_t i = 0; i < EMOTICON_COUNT; i++) {
    size_t l = strlen(EMOTICONS[i].ascii);
    if (l <= bestLen || pos + l > n || strncmp(s + pos, EMOTICONS[i].ascii, l) != 0) continue;
    size_t e = pos + l;
    if (e < n && !isspace((unsigned char)s[e]) && !strchr(".,!?", s[e])) continue;
    best = (int)i; bestLen = l;
  }
  if (best >= 0 && len) *len = bestLen;
  return best;
}

// Which picture goes with list entry k: the pictures are numbered in the order in which each distinct NAME first appears in the list
// (so ":)" and ":-)" share picture 0). tools/make_emoji.py builds src/emoji18.bin in the same order from this file.
static inline int emoticonIconIndex(int k) {
  if (k < 0 || k >= (int)EMOTICON_COUNT) return -1;
  int idx = 0;
  for (int i = 0; i < (int)EMOTICON_COUNT; i++) {
    if (!strcmp(EMOTICONS[i].name, EMOTICONS[k].name)) return idx;
    bool first = true;
    for (int j = 0; j < i; j++) if (!strcmp(EMOTICONS[j].name, EMOTICONS[i].name)) { first = false; break; }
    if (first) idx++;
  }
  return -1;
}
static inline int emoticonIconCount() {
  int n = 0;
  for (int i = 0; i < (int)EMOTICON_COUNT; i++) { bool first = true; for (int j = 0; j < i; j++) if (!strcmp(EMOTICONS[j].name, EMOTICONS[i].name)) { first = false; break; } if (first) n++; }
  return n;
}
