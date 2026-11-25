#include "../../src/txbackoff.h"
#include <cstdio>
static int fails = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
int main() {
  TxBackoff b;
  expect(b.allowed(0) && b.waitMs() == 0, "nothing failed: send when you like");
  const uint32_t W[7] = {10000, 20000, 40000, 80000, 120000, 120000, 120000};
  uint32_t t = 1000;
  for (int i = 0; i < 7; i++) {
    b.failed(t);
    expect(b.waitMs() == W[i], "waits grow, then stay at 2 minutes");
    expect(!b.allowed(t + W[i] - 1) && b.allowed(t + W[i]), "allowed again when the wait is over");
    expect(b.buzz() == (i == 0), "only the first failure buzzes");
    t += W[i];
  }
  b.sent(); expect(b.allowed(t) && !b.fails, "a success clears it");
  b.failed(0xFFFFF000u); expect(!b.allowed(100) && b.allowed(10000), "works across the millisecond counter wrapping");
  for (int i = 0; i < 5000; i++) b.failed(0);
  expect(b.waitMs() == 120000, "many failures do not overflow");
  if (!fails) printf("txbackoff: all ok\n");
  return fails ? 1 : 0;
}
