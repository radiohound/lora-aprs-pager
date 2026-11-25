#include "../../src/rsthist.h"
#include <cstdio>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  RstHist h; memset(&h, 0xA5, sizeof h);                       // whatever flash might hold
  expect(!rstValid(h), "rubbish is not a history");
  rstAdd(h, 100, 1, 0, 0); expect(rstValid(h) && h.count == 1 && h.starts == 1 && h.crashes == 0, "first start clears the rubbish");
  rstAdd(h, 200, 6, 20, 77); expect(h.crashes == 1 && rstGet(h, 0).reason == 6 && rstGet(h, 0).stage == 20 && rstGet(h, 0).ranMin == 77, "a watchdog is a crash, newest first");
  expect(rstGet(h, 1).utc == 100, "older after it");
  rstAdd(h, 300, 3, 0, 5); rstAdd(h, 400, 9, 0, 5); expect(h.crashes == 1, "restart command and brownout are not crashes");
  for (int i = 0; i < 25; i++) rstAdd(h, 1000 + i, 5, 2, 100000);
  expect(h.count == RSTH_N && h.starts == 29 && h.crashes == 26, "keeps the last ten, totals keep counting");
  expect(rstGet(h, 0).utc == 1024 && rstGet(h, 9).utc == 1015, "the ten newest, in order");
  expect(rstGet(h, 0).ranMin == 65535, "long runs are capped, not wrapped");
  h.starts = h.crashes = 65535; rstAdd(h, 1, 4, 0, 0); expect(h.starts == 65535 && h.crashes == 65535, "totals do not wrap");
  if (!fails) printf("rsthist: all ok\n");
  return fails ? 1 : 0;
}
