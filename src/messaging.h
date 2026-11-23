// APRS messaging engine for the pager: parsing, conversations, acks, retries, delete with undo, persistence.
// Plain C++ (no Arduino), tested in test/host/test_messaging.cpp. Rules follow the APRS 1.2 draft c, chapter 14, and the
// engine in lora-aprs-pi (messages.py):
//   :ADDRESSEE:text{number   addressee = 9 characters padded with spaces, text up to 67 characters, number "{" + 1-5 letters/digits
//   :CALL     :ackNNN / :rejNNN
//   a message without a number is never acked; a numbered one is acked (every copy, at least 30 s apart) and shown once
//   ALL/QST/CQ and BLN* are group/bulletin addresses: never acked (bulletins are not stored yet)
// Everything is readable plain text; nothing here encrypts.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace msg {

enum Kind { K_NONE, K_MESSAGE, K_ACK, K_REJ, K_BULLETIN };
struct Parsed {
  Kind kind = K_NONE;
  char from[10] = {}, to[10] = {}, id[7] = {}, replyAck[7] = {};
  std::string text;
};

static const int MAX_TEXT = 67;                 // characters we send
static const int STORE_TEXT = 168;              // characters we keep of a received message
static const uint32_t RETRY_S[6] = {30, 60, 120, 240, 480, 960};   // wait after try 1, 2, ... (LoRa at SF12 is slow)
static const int MAX_TRIES = 6;
static const uint32_t BUSY_RETRY_MS = 30000, MSG_GAP_MS = 3000, ACK_DELAY_MS = 1500, ACK_SPACING_MS = 30000;
static const uint32_t DUP_WINDOW_MS = 20UL * 60UL * 1000UL, UNDO_MS = 10000;
static const size_t MAX_MESSAGES = 300;

inline bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
inline std::string trimSpaces(const char *s, size_t n) {
  size_t a = 0; while (a < n && s[a] == ' ') a++;
  size_t b = n; while (b > a && s[b - 1] == ' ') b--;
  return std::string(s + a, b - a);
}
inline std::string upperStr(std::string s) { for (auto &c : s) c = toupper((unsigned char)c); return s; }
// 3-6 letters/digits with at least one of each, optional -SSID 0..15 (same rule as the settings page)
inline bool validCall(const std::string &c) {
  size_t dash = c.find('-'); std::string base = c.substr(0, dash);
  if (base.size() < 3 || base.size() > 6) return false;
  bool l = false, d = false;
  for (char ch : base) { if (isupper((unsigned char)ch)) l = true; else if (isdigit((unsigned char)ch)) d = true; else return false; }
  if (!l || !d) return false;
  if (dash == std::string::npos) return true;
  std::string s = c.substr(dash + 1); if (s.empty() || s.size() > 2) return false;
  for (char ch : s) if (!isdigit((unsigned char)ch)) return false;
  return atoi(s.c_str()) <= 15;
}
inline bool alnum(const std::string &s) { for (unsigned char c : s) if (!isalnum(c)) return false; return true; }

// Parse the text of a LoRa APRS frame (after the 3C FF 01 header): "SRC>DST,PATH::ADDRESSEE:text{id"
inline Parsed parse(const char *t, size_t n) {
  Parsed p;
  const char *gt = (const char *)memchr(t, '>', n), *colon = (const char *)memchr(t, ':', n);
  if (!gt || !colon || gt > colon || gt == t || (size_t)(gt - t) > 9) return p;
  memcpy(p.from, t, gt - t);
  const char *info = colon + 1; size_t m = n - (info - t);
  if (m < 11 || info[0] != ':' || info[10] != ':') return p;
  std::string to = upperStr(trimSpaces(info + 1, 9));
  if (to.empty()) return p;
  strncpy(p.to, to.c_str(), 9);
  std::string body(info + 11, m - 11);
  if (to == "ALL" || to == "QST" || to == "CQ" || to.compare(0, 3, "BLN") == 0) { p.kind = K_BULLETIN; p.text = body; return p; }
  if (body.size() >= 4 && (body.compare(0, 3, "ack") == 0 || body.compare(0, 3, "rej") == 0)) {
    std::string id = body.substr(3); size_t br = id.find('}'); if (br != std::string::npos) id = id.substr(0, br);
    if (!id.empty() && id.size() <= 5 && alnum(id)) { p.kind = body[0] == 'a' ? K_ACK : K_REJ; strcpy(p.id, id.c_str()); p.text = body; return p; }
  }
  p.kind = K_MESSAGE;
  size_t br = body.rfind('{');
  if (br != std::string::npos) {
    std::string num = body.substr(br + 1), reply;
    size_t cb = num.find('}'); if (cb != std::string::npos) { reply = num.substr(cb + 1); num = num.substr(0, cb); }
    if (!num.empty() && num.size() <= 5 && alnum(num) && alnum(reply) && reply.size() <= 5) {
      strcpy(p.id, num.c_str()); strcpy(p.replyAck, reply.c_str()); body = body.substr(0, br);
    }
  }
  p.text = body;
  return p;
}

inline std::string pad9(const char *call) { char b[16]; snprintf(b, sizeof b, "%-9.9s", call); return b; }
// info part of a message / ack frame; the caller puts "CALL>TOCALL,PATH" in front
// The whole frame text: "SRC>TOCALL,PATH" + ":" (ends the header) + info. For a message the info itself starts with ":", so the
// frame has TWO colons in a row: "K6ATV-6>APZPGR,WIDE1-1::K6ATV-4  :hello{1".
inline std::string tnc2(const char *call, const char *tocall, const char *path, const std::string &info) {
  return std::string(call) + ">" + tocall + (path && *path ? std::string(",") + path : std::string()) + ":" + info;
}
inline std::string messageInfo(const char *to, const std::string &text, const char *id) { return ":" + pad9(to) + ":" + text + "{" + id; }
inline std::string ackInfo(const char *to, const char *id) { return ":" + pad9(to) + ":ack" + id; }

enum State : uint8_t { S_QUEUED, S_WAIT, S_ACKED, S_REJECTED, S_FAILED, S_CANCELLED, S_RECEIVED };
struct Msg {
  uint32_t seq = 0, epoch = 0, nextAt = 0;
  char peer[10] = {}, id[7] = {}, text[STORE_TEXT] = {};
  uint8_t state = S_RECEIVED, tries = 0; bool out = false, read = true;
  int16_t rssi = 0;
  bool open() const { return out && (state == S_QUEUED || state == S_WAIT); }
};
struct TxItem { std::string info, to; uint32_t seq = 0; bool isAck = false; };
struct Conv { std::string peer; int count = 0, unread = 0; uint32_t lastSeq = 0; size_t lastIdx = 0; };
enum Event { EV_NONE, EV_NEW, EV_ACKED, EV_REJECTED, EV_DUP };

class Messenger {
public:
  std::vector<Msg> m;
  bool changed = false;                // set on any change; the caller saves the file and clears it
  void setCall(const char *c) { strncpy(call, upperStr(c).c_str(), 9); call[9] = 0; }
  const char *myCall() const { return call; }
  bool callOk() const { return strcmp(call, "N0CALL") != 0 && validCall(call); }

  // ---- receiving ----
  Event onRx(const Parsed &p, uint32_t now, uint32_t epoch, int rssi) {
    if (p.kind == K_NONE || p.kind == K_BULLETIN || upperStr(p.to) != call) return EV_NONE;
    bool plain = false;                                               // an "ack..." that is really just text
    if (p.kind == K_ACK || p.kind == K_REJ) {
      if (hasOut(p.from, p.id)) {                                     // an ack or rej for a message of ours (a repeat of one already settled is ignored)
        return settle(p.from, p.id, p.kind == K_ACK ? S_ACKED : S_REJECTED) ? (p.kind == K_ACK ? EV_ACKED : EV_REJECTED) : EV_NONE;
      }
      bool numeric = true; for (const char *c = p.id; *c; c++) if (!isdigit((unsigned char)*c)) numeric = false;
      if (numeric) return EV_NONE;                                    // a stray ack for a message we no longer have: drop it
      plain = true;                                                   // "ackbar": an ordinary message
    }
    if (p.replyAck[0]) settle(p.from, p.replyAck, S_ACKED);          // "{MM}AA": a free ack for our message AA
    std::string text = p.text.substr(0, STORE_TEXT - 1);
    if (p.id[0] && !plain) {                                                    // numbered: ack every copy, show once
      for (auto &s : seen) if (!strcmp(s.from, p.from) && !strcmp(s.id, p.id) && s.text == text && (uint32_t)(now - s.firstAt) < DUP_WINDOW_MS) {
        if (due(now, s.lastAckAt + ACK_SPACING_MS)) { queueAck(p.from, p.id, now); s.lastAckAt = now + ACK_DELAY_MS; }
        return EV_DUP;
      }
      seen.push_back({{}, {}, text, now, now + ACK_DELAY_MS});
      strcpy(seen.back().from, p.from); strcpy(seen.back().id, p.id);
      if (seen.size() > 60) seen.erase(seen.begin());
      queueAck(p.from, p.id, now);
    } else {                                                          // unnumbered: never acked; same text within the window is a repeat
      for (auto &s : seen) if (!strcmp(s.from, p.from) && !s.id[0] && s.text == text && (uint32_t)(now - s.firstAt) < DUP_WINDOW_MS) return EV_DUP;
      seen.push_back({{}, {}, text, now, 0}); strcpy(seen.back().from, p.from);
    }
    Msg x; x.seq = nextSeq++; x.out = false; x.read = false; x.state = S_RECEIVED; x.epoch = epoch; x.rssi = (int16_t)rssi;
    strncpy(x.peer, p.from, 9); strncpy(x.id, p.id, 6); strncpy(x.text, text.c_str(), STORE_TEXT - 1);
    add(x); return EV_NEW;
  }

  // ---- sending ----
  // Returns the new message's seq, or 0 with `err` set.
  uint32_t send(const char *to, const std::string &text, uint32_t now, uint32_t epoch, const char **err) {
    std::string t = upperStr(to);
    if (!callOk()) { *err = "Set your call sign first"; return 0; }
    if (!validCall(t)) { *err = "To: not a call sign"; return 0; }
    if (text.empty()) { *err = "Nothing to send"; return 0; }
    if ((int)text.size() > MAX_TEXT) { *err = "Longer than 67 characters"; return 0; }
    for (unsigned char c : text) if (c < 32 || c > 126 || c == '{' || c == '|' || c == '~') { *err = "Unsupported character"; return 0; }
    Msg x; x.seq = nextSeq++; x.out = true; x.read = true; x.state = S_QUEUED; x.epoch = epoch; x.nextAt = now;
    strncpy(x.peer, t.c_str(), 9); snprintf(x.id, sizeof x.id, "%u", (unsigned)idCounter);
    idCounter = idCounter >= 99999 ? 1 : idCounter + 1;
    strncpy(x.text, text.c_str(), STORE_TEXT - 1);
    add(x); return x.seq;
  }

  // The next frame to put on the air, or false. Acks first, then the oldest due message per addressee; one open message per
  // addressee; our own messages at least MSG_GAP_MS apart. Call txDone() after trying.
  bool nextTx(uint32_t now, TxItem &out) {
    if (inflight) return false;
    if (lastTxAt && !due(now, lastTxAt + MSG_GAP_MS)) return false;
    for (size_t i = 0; i < acks.size(); i++) if (due(now, acks[i].at)) {
      out.info = ackInfo(acks[i].to, acks[i].id); out.to = acks[i].to; out.seq = 0; out.isAck = true;
      acks.erase(acks.begin() + i); inflight = true; inflightSeq = 0; return true;
    }
    std::vector<std::string> seenPeers;
    for (auto &x : m) {
      if (!x.open()) continue;
      std::string peer = x.peer;
      if (std::find(seenPeers.begin(), seenPeers.end(), peer) != seenPeers.end()) continue;   // one open message per addressee
      seenPeers.push_back(peer);
      if (!due(now, x.nextAt)) continue;
      if (x.tries >= MAX_TRIES) { x.state = S_FAILED; changed = true; continue; }              // last wait is over, no ack: give up
      out.info = messageInfo(x.peer, x.text, x.id); out.to = x.peer; out.seq = x.seq; out.isAck = false;
      inflight = true; inflightSeq = x.seq; return true;
    }
    return false;
  }
  void txDone(bool sent, uint32_t now) {
    if (!inflight) return;
    inflight = false;
    if (sent) lastTxAt = now ? now : 1;
    if (!inflightSeq) return;
    for (auto &x : m) if (x.seq == inflightSeq && x.open()) {
      if (sent) { x.tries++; x.state = S_WAIT; x.nextAt = now + RETRY_S[std::min<int>(x.tries, MAX_TRIES) - 1] * 1000UL; }
      else x.nextAt = now + BUSY_RETRY_MS;                          // channel busy: try again soon, without using up a try
      changed = true;
    }
  }
  void noteTx(uint32_t now) { lastTxAt = now ? now : 1; }          // any of our transmissions (a beacon too) keeps the 3 s gap
  // seconds until the next try (0 if due or not waiting)
  uint32_t secondsToRetry(const Msg &x, uint32_t now) const { return (x.state == S_WAIT && !due(now, x.nextAt)) ? (x.nextAt - now + 999) / 1000 : 0; }
  void cancel(uint32_t seq) { for (auto &x : m) if (x.seq == seq && x.open()) { x.state = S_CANCELLED; changed = true; } }

  // ---- conversations ----
  std::vector<Conv> conversations() const {
    std::vector<Conv> v;
    for (size_t i = 0; i < m.size(); i++) {
      Conv *c = nullptr; for (auto &k : v) if (k.peer == m[i].peer) { c = &k; break; }
      if (!c) { v.push_back({}); c = &v.back(); c->peer = m[i].peer; }
      c->count++; if (!m[i].read) c->unread++;
      if (m[i].seq >= c->lastSeq) { c->lastSeq = m[i].seq; c->lastIdx = i; }
    }
    std::sort(v.begin(), v.end(), [](const Conv &a, const Conv &b) { return a.lastSeq > b.lastSeq; });
    return v;
  }
  std::vector<size_t> thread(const std::string &peer) const {
    std::vector<size_t> v; for (size_t i = 0; i < m.size(); i++) if (peer == m[i].peer) v.push_back(i); return v;   // m is kept in seq order
  }
  int unreadTotal() const { int n = 0; for (auto &x : m) if (!x.read) n++; return n; }
  void markRead(const std::string &peer) { for (auto &x : m) if (peer == x.peer && !x.read) { x.read = true; changed = true; } }

  // ---- delete with undo (the deleted messages wait in the trash for UNDO_MS) ----
  void removeMessage(uint32_t seq, uint32_t now) { take([&](const Msg &x) { return x.seq == seq; }, now); }
  void removeConversation(const std::string &peer, uint32_t now) { take([&](const Msg &x) { return peer == x.peer; }, now); }
  void removeAll(uint32_t now) { take([](const Msg &) { return true; }, now); }
  uint32_t trashSecondsLeft(uint32_t now) const { return due(now, undoUntil) ? 0 : (undoUntil - now + 999) / 1000; }
  bool canUndo(uint32_t now) const { return !trash.empty() && !due(now, undoUntil); }
  size_t undo(uint32_t now) {
    if (!canUndo(now)) return 0;
    size_t n = trash.size();
    for (auto &x : trash) m.push_back(x);
    trash.clear();
    std::sort(m.begin(), m.end(), [](const Msg &a, const Msg &b) { return a.seq < b.seq; });
    changed = true; return n;
  }

  // ---- saving ----
  std::string serialize() const {
    std::string s("PMG1"); uint32_t h[4] = {(uint32_t)sizeof(Msg), nextSeq, idCounter, (uint32_t)m.size()};
    s.append((const char *)h, sizeof h);
    for (auto &x : m) s.append((const char *)&x, sizeof x);
    return s;
  }
  bool deserialize(const uint8_t *d, size_t n) {
    uint32_t h[4];
    if (n < 4 + sizeof h || memcmp(d, "PMG1", 4) != 0) return false;
    memcpy(h, d + 4, sizeof h);
    if (h[0] != sizeof(Msg) || h[3] > MAX_MESSAGES || n != 4 + sizeof h + (size_t)h[3] * sizeof(Msg)) return false;
    std::vector<Msg> v(h[3]);
    if (h[3]) memcpy(v.data(), d + 4 + sizeof h, (size_t)h[3] * sizeof(Msg));
    m = v; nextSeq = h[1] ? h[1] : 1; idCounter = h[2] ? h[2] : 1; return true;
  }
  // After loading: timers are relative to the boot clock, so restart them. Sent-but-unacked messages try again in 30 s.
  void restoreTimers(uint32_t now) {
    for (auto &x : m) if (x.open()) x.nextAt = now + (x.tries ? 30000 : 5000);
  }
  void startIds(uint32_t seed) { if (idCounter <= 1) idCounter = 1 + seed % 90000; }

private:
  struct AckJob { char to[10]; char id[7]; uint32_t at; };
  struct Seen { char from[10]; char id[7]; std::string text; uint32_t firstAt, lastAckAt; };
  char call[10] = "N0CALL";
  uint32_t nextSeq = 1, idCounter = 1, lastTxAt = 0, undoUntil = 0, inflightSeq = 0;
  bool inflight = false;
  std::vector<AckJob> acks; std::vector<Seen> seen; std::vector<Msg> trash;

  void add(const Msg &x) {
    m.push_back(x);
    while (m.size() > MAX_MESSAGES) {                       // drop the oldest that is not still being sent
      size_t k = 0; while (k < m.size() && m[k].open()) k++;
      if (k >= m.size()) break;
      m.erase(m.begin() + k);
    }
    changed = true;
  }
  void queueAck(const char *to, const char *id, uint32_t now) {
    for (auto &a : acks) if (!strcmp(a.to, to) && !strcmp(a.id, id)) return;
    AckJob a{}; strncpy(a.to, to, 9); strncpy(a.id, id, 6); a.at = now + ACK_DELAY_MS; acks.push_back(a);
  }
  bool hasOut(const char *from, const char *id) const { for (auto &x : m) if (x.out && !strcmp(x.peer, from) && !strcmp(x.id, id)) return true; return false; }
  bool settle(const char *from, const char *id, State to) {
    for (auto &x : m) if (x.out && !strcmp(x.peer, from) && !strcmp(x.id, id) && (x.open() || x.state == S_FAILED)) { x.state = to; changed = true; return true; }
    return false;
  }
  template <class F> void take(F pred, uint32_t now) {
    std::vector<Msg> keep, gone;
    for (auto &x : m) (pred(x) ? gone : keep).push_back(x);
    if (gone.empty()) return;
    m = keep; trash = gone; undoUntil = now + UNDO_MS; changed = true;
  }
};

// LoRa time on air in milliseconds (the standard Semtech formula): explicit header, CRC on, coding rate 4/(4+cr)
inline double airtimeMs(int payloadBytes, int sf = 12, double bwHz = 125000.0, int cr = 1, int preamble = 8) {
  double tsym = (double)(1 << sf) / bwHz * 1000.0;
  bool lowDr = tsym > 16.0;
  double num = 8.0 * payloadBytes - 4.0 * sf + 28.0 + 16.0;
  double den = 4.0 * (sf - (lowDr ? 2 : 0));
  double npay = 8.0 + std::max(std::ceil(num / den) * (cr + 4), 0.0);
  return (preamble + 4.25 + npay) * tsym;
}

}  // namespace msg
