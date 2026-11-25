#include "../../src/repeater.h"
#include <cstdio>
static int fails = 0;
static void is(const char *path, const char *want) {
  char o[10] = "x"; bool ok = aprsRepeater(path, o, sizeof o);
  if (want ? (!ok || strcmp(o, want) != 0) : ok) { printf("FAIL: %s -> %s (wanted %s)\n", path, ok ? o : "(none)", want ? want : "(none)"); fails++; }
}
int main() {
  is("N0DIG-10*", "N0DIG-10");
  is("N0DIG-10*,WIDE2-1", "N0DIG-10");
  is("N0DIG-10,WIDE1*", "N0DIG-10");
  is("N0AAA-3,N0DIG-10*", "N0DIG-10");
  is("N0AAA-3*,N0DIG-10*", "N0DIG-10");
  is("K1AB*", "K1AB");
  is("WIDE1-1", nullptr);                       // not repeated
  is("N0DIG-10,WIDE1-1", nullptr);              // nothing marked as used
  is("WIDE1*", nullptr);                        // alias only: who sent it is unknown
  is("N0DIG-10,WIDE1*,WIDE2*", nullptr);        // a later hop that did not name itself
  is("TCPIP*", nullptr);
  is("qAO,N0IGATE", nullptr);
  is("", nullptr);
  if (!fails) printf("repeater: all ok\n");
  return fails ? 1 : 0;
}
