#include <stdio.h>
#include "buzzquiet.h"
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { fails++; printf("FAIL: %s\n", what); } }
int main() {
  BuzzQuiet b;
  expect(!b.quiet(0) && !b.quiet(100000), "no buzz yet: motion is believed");
  b.buzzed(10000);
  expect(b.quiet(10000) && b.quiet(10230) && b.quiet(12999), "ignored during the buzz and just after (reports came about 230 ms after a beacon)");
  expect(!b.quiet(13000) && !b.quiet(20000), "believed again 3 s after the buzz started");
  b.buzzed(30000); b.buzzed(32000);
  expect(b.quiet(34000) && !b.quiet(35000), "a second buzz starts the time again");
  b.buzzed(0xFFFFFF00u);
  expect(b.quiet(0x00000100u) && !b.quiet(0x00001000u), "the millisecond counter rolling over");
  b.buzzed(5000); b.quiet(9000);
  expect(!b.quiet(5000u + 0xFFFFFFFFu), "once over it stays over");
  printf(fails ? "%d FAILED\n" : "all buzz quiet tests passed\n", fails); return fails ? 1 : 0;
}
