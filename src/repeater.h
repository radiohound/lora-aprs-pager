// Who sent the copy of a digipeated packet that we received? The signal strength belongs to that station. Plain C++, tested in
// test/host/test_repeater.cpp.
#pragma once
#include <string.h>
#include <stddef.h>

// A path entry that names a station: letters and digits with an optional -SSID, not a routing alias or an APRS-IS marker.
inline bool repIsCall(const char *s, size_t n) {
  if (n < 3 || n > 9) return false;
  static const char *const ALIAS[] = {"WIDE", "TRACE", "RELAY", "TCPIP", "TCPXX", "NOGATE", "RFONLY", "GATE", "ECHO", "qA", "SGATE", "TEMP"};
  for (const char *a : ALIAS) if (n >= strlen(a) && strncmp(s, a, strlen(a)) == 0) return false;
  bool letter = false, digit = false; size_t base = 0;
  while (base < n && s[base] != '-') { char c = s[base]; if (c >= '0' && c <= '9') digit = true; else if (c >= 'A' && c <= 'Z') letter = true; else return false; base++; }
  if (base < 3 || base > 6 || !letter || !digit) return false;
  for (size_t i = base + 1; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z'))) return false;
  return base == n || n - base - 1 >= 1;
}
// path: the entries after the destination, e.g. "K6ABC-10,WIDE1*". The last entry marked '*' was used last. If it is a station, that is the one;
// if it is an alias, the station may have put its call just before it. Otherwise we cannot tell, and say so (false) rather than guess.
inline bool aprsRepeater(const char *path, char *out, size_t cap) {
  if (!path || cap < 10) return false;
  out[0] = 0;
  const char *prev = nullptr, *used = nullptr, *usedPrev = nullptr; size_t prevLen = 0, usedLen = 0, usedPrevLen = 0;
  const char *start = path;
  for (const char *c = path;; c++) {
    if (*c == ',' || *c == 0) {
      size_t n = c - start; bool star = n && start[n - 1] == '*'; if (star) n--;
      if (star) { used = start; usedLen = n; usedPrev = prev; usedPrevLen = prevLen; }
      prev = start; prevLen = n;
      if (*c == 0) break;
      start = c + 1;
    }
  }
  if (!used) return false;
  if (!repIsCall(used, usedLen)) { used = usedPrev; usedLen = usedPrevLen; if (!used || !repIsCall(used, usedLen)) return false; }
  memcpy(out, used, usedLen); out[usedLen] = 0; return true;
}
