// u-blox M10 (MIA-M10Q) messages the pager sends to its GPS. Pure byte building, no hardware, so the host tests can check it.
// Key IDs and message layouts: u-blox M10 SPG 5.10 interface description (UBX-21035062).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "clock.h"

inline void ubxPut(uint8_t *b, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i)); }

// A complete frame: B5 62 class id len(2) payload CK_A CK_B. `out` needs n + 8 bytes. Returns the length.
inline size_t ubxFrame(uint8_t cls, uint8_t id, const uint8_t *pl, size_t n, uint8_t *out) {
  out[0] = 0xB5; out[1] = 0x62; out[2] = cls; out[3] = id; out[4] = (uint8_t)n; out[5] = (uint8_t)(n >> 8);
  for (size_t i = 0; i < n; i++) out[6 + i] = pl[i];
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < 6 + n; i++) { a += out[i]; b += a; }
  out[6 + n] = a; out[7 + n] = b;
  return n + 8;
}

// UBX-CFG-VALSET into the RAM layer only (a GPS power cycle undoes it, no flash wear). At most 5 items of up to 4 bytes.
inline size_t ubxValset(const uint32_t *keys, const uint32_t *vals, const uint8_t *sizes, int n, uint8_t *out) {
  uint8_t pl[4 + 5 * 8]; size_t len = 0; pl[len++] = 0; pl[len++] = 1; pl[len++] = 0; pl[len++] = 0;   // version 0, layer RAM
  for (int i = 0; i < n && i < 5; i++) { ubxPut(pl + len, keys[i], 4); len += 4; ubxPut(pl + len, vals[i], sizes[i]); len += sizes[i]; }
  return ubxFrame(0x06, 0x8A, pl, len, out);
}

// UBX-RXM-PMREQ: put the receiver into backup mode until something wakes it: any edge on its receive line (so sending it a byte wakes it).
// It keeps its satellite data through the backup supply, so the next start is a hot start. `out` needs 24 bytes.
inline size_t ubxBackup(uint8_t *out) {
  uint8_t pl[16] = {0};                     // version 0, duration 0 = wait for a wake-up signal
  pl[8] = 0x02;                             // flags: bit 1 = backup
  pl[12] = 0x08;                            // wake-up sources: bit 3 = UART RX
  return ubxFrame(0x02, 0x41, pl, 16, out);
}

// UBX-MGA-INI-POS_LLH: where we probably are. accCm is the position accuracy the receiver should assume.
inline size_t ubxMgaPos(int32_t lat7, int32_t lon7, uint32_t accCm, uint8_t *out) {
  uint8_t m[20] = {0x01, 0x00, 0, 0}; ubxPut(m + 4, (uint32_t)lat7, 4); ubxPut(m + 8, (uint32_t)lon7, 4); ubxPut(m + 12, 0, 4); ubxPut(m + 16, accCm, 4);
  return ubxFrame(0x13, 0x40, m, 20, out);
}

// UBX-MGA-INI-TIME_UTC: the time, taken as arriving now, good to accSec seconds.
inline size_t ubxMgaTime(uint32_t utc, uint16_t accSec, uint8_t *out) {
  LocalTime c = clockBreak(utc); uint8_t m[24] = {0x10, 0x00, 0x00, 18};     // leap seconds since 1980: 18
  ubxPut(m + 4, (uint32_t)c.year, 2); m[6] = (uint8_t)c.month; m[7] = (uint8_t)c.day; m[8] = (uint8_t)c.hour; m[9] = (uint8_t)c.min; m[10] = (uint8_t)c.sec;
  ubxPut(m + 16, accSec, 2);
  return ubxFrame(0x13, 0x40, m, 24, out);
}

// CFG-NAVSPG-ACKAIDING: make the receiver answer each assistance message with UBX-MGA-ACK (off by default). `out` needs 24 bytes.
inline size_t ubxAckAidingOn(uint8_t *out) {
  const uint32_t k[1] = {0x10110025}, v[1] = {1}; const uint8_t z[1] = {1};
  return ubxValset(k, v, z, 1, out);
}

// Picks short UBX frames (payload up to 16 bytes) out of the receiver's output, which also carries NMEA text. Feed every byte;
// true = a complete frame with a good checksum is in cls, id, len, pl. Longer frames are ignored.
struct UbxScan {
  uint8_t cls = 0, id = 0, pl[16] = {0}; uint16_t len = 0;
  int st = 0; uint16_t i = 0; uint8_t a = 0, b = 0;
  void sum(uint8_t c) { a += c; b += a; }
  bool feed(uint8_t c) {
    switch (st) {
      case 0: if (c == 0xB5) st = 1; return false;
      case 1: st = c == 0x62 ? 2 : (c == 0xB5 ? 1 : 0); a = b = 0; return false;
      case 2: cls = c; sum(c); st = 3; return false;
      case 3: id = c; sum(c); st = 4; return false;
      case 4: len = c; sum(c); st = 5; return false;
      case 5: len |= (uint16_t)c << 8; sum(c); i = 0; st = len > sizeof pl ? 0 : (len ? 6 : 7); return false;
      case 6: pl[i++] = c; sum(c); if (i >= len) st = 7; return false;
      case 7: st = c == a ? 8 : 0; return false;
      default: st = 0; return c == b;
    }
  }
};

// The receiver's answer about the assistance message whose first payload byte was `msgType` (0x01 position, 0x10 time).
// UBX-MGA-ACK-DATA0 payload: type (1 = accepted, 0 = not used), version, infoCode, msgId, first 4 payload bytes of the message.
inline bool ubxIsMgaAck(const UbxScan &f, uint8_t msgType) { return f.cls == 0x13 && f.id == 0x60 && f.len == 8 && f.pl[3] == 0x40 && f.pl[4] == msgType; }
// UBX-ACK-ACK (1) or UBX-ACK-NAK (0) for a UBX-CFG-VALSET; -1 = this frame is neither.
inline int ubxValsetAnswer(const UbxScan &f) { return (f.cls == 0x05 && f.id <= 1 && f.len == 2 && f.pl[0] == 0x06 && f.pl[1] == 0x8A) ? f.id : -1; }

