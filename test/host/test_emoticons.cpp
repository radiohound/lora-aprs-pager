// Host test for src/emoticons.h. Build and run:  c++ -std=c++17 -I src test/host/test_emoticons.cpp -o /tmp/test_emoticons && /tmp/test_emoticons
#include <cstdio>
#include <string>
#include <vector>
#include "emoticons.h"

// Returns the names found in a text, in order, using the same scan the display would use.
static std::vector<std::string> scan(const std::string &t) {
  std::vector<std::string> out;
  for (size_t i = 0; i < t.size();) {
    size_t len = 0; int k = emoticonMatch(t.c_str(), t.size(), i, &len);
    if (k >= 0) { out.push_back(EMOTICONS[k].name); i += len; } else i++;
  }
  return out;
}
static int fails = 0;
static void check(const std::string &t, std::vector<std::string> want) {
  auto got = scan(t);
  if (got != want) { fails++; printf("FAIL '%s': got %zu items", t.c_str(), got.size()); for (auto &g : got) printf(" [%s]", g.c_str()); printf("\n"); }
}
static void checkIcons() {
  auto idx = [](const char *a) { size_t l; return emoticonIconIndex(emoticonMatch(a, strlen(a), 0, &l)); };
  if (emoticonIconCount() != 24) { fails++; printf("FAIL: 24 distinct pictures expected, got %d\n", emoticonIconCount()); }
  if (idx(":)") != 0 || idx(":-)") != 0) { fails++; printf("FAIL: :) and :-) share picture 0\n"); }
  if (idx("<3") != 12 || idx("</3") != 13) { fails++; printf("FAIL: heart is picture 12, broken heart 13\n"); }
  if (idx("8-)") != idx("B)")) { fails++; printf("FAIL: both cool faces share a picture\n"); }
  if (idx(":3") != 23) { fails++; printf("FAIL: the last name has the last picture (23)\n"); }
  if (emoticonIconIndex(-1) != -1 || emoticonIconIndex((int)EMOTICON_COUNT) != -1) { fails++; printf("FAIL: out of range gives -1\n"); }
}
int main() {
  checkIcons();
  check("on my way :)", {"smile"});
  check(":) see you", {"smile"});
  check("ok :-) thanks!", {"smile"});
  check("bad day :(.", {"sad"});
  check("I <3 this", {"heart"});
  check("</3", {"broken heart"});          // longest match wins over <3
  check("\\o/ we made it", {"cheer"});
  check("great (y) and (n)", {"thumbs up", "thumbs down"});
  check("lol XD :P", {"laughing", "tongue"});
  check(":)", {"smile"});
  check("", {});
  // must NOT match
  check("meet at 10:30", {});
  check("see http://example.com", {});
  check("copyright (c) 2026", {});
  check("re:) fine", {});                   // preceded by a letter
  check("a:D", {});
  check("ratio 3:1", {});
  check("call K6ATV-14:)", {});
  check("sMILE:)ok", {});
  check("xo/", {});
  check(":)) double", {});                  // trailing ) not allowed: no match, stays text
  if (fails) { printf("%d FAILED\n", fails); return 1; }
  printf("all emoticon tests passed (%zu emoticons in the list)\n", EMOTICON_COUNT);
  return 0;
}
