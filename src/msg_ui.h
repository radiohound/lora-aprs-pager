// Messages screens for the pager: conversation list, thread, compose, and the knob-driven menu.
// Included by app.cpp AFTER the stations table, drawSymbol(), the haptic object and the Messenger `M` exist; it uses them directly.
// Knob rule (docs/messaging.md): everything works with the knob alone: turn = move, press = open/select, long press = menu.
#pragma once
#include <string>
#include <vector>

static const char *CANNED[] = {"OK", "QSL", "On my way", "Where are you?", "Running late", "73"};
static const int NCANNED = 6;
static const uint16_t C_BG = TFT_BLACK, C_SEL = 0x0010, C_GRAY = 0x8410, C_BAR = 0x2104;

static uint32_t epochNow() { return gps.fix.epoch; }
static const Station *stationFor(const char *call) {
  for (auto &st : stations) if (st.used && !strcasecmp(st.call, call)) return &st;
  return nullptr;
}
// the station's APRS symbol, or a grey dot when we have not heard its position yet
static void drawPeerIcon(lgfx::LovyanGFX &D, int cx, int cy, const char *call) {
  const Station *st = stationFor(call);
  if (st && drawSymbol(D, cx, cy, st->table, st->sym)) return;
  D.fillCircle(cx, cy, 4, C_GRAY);
}
// One emoticon picture (18 x 18) with its top-left corner at (x, y), blended over what is already drawn. False if it would run off the screen.
static bool drawEmoticon(lgfx::LovyanGFX &D, int x, int y, int icon) {
  const int E = 18;
  if (icon < 0 || icon >= emoticonIconCount() || x < 0 || y < 0 || x + E > D.width() || y + E > D.height()) return false;
  uint16_t px[E * E]; D.readRect(x, y, E, E, px);
  blendRec(px, emojiBin + (size_t)icon * E * E * 3, E * E);
  D.pushImage(x, y, E, E, px); return true;
}
// Text at (x, y) in the given size with each emoticon replaced by its picture, centred in the space its characters would take (so the
// line is exactly as wide as the plain text and wrapping and right-alignment stay correct). What is sent and stored is still the ASCII.
static void drawRich(lgfx::LovyanGFX &D, int x, int y, const std::string &t, int size, uint16_t fg, uint16_t bg) {
  const int cw = 6 * size; D.setTextSize(size); D.setTextColor(fg, bg);
  std::string run; int runX = x; size_t i = 0;
  auto flush = [&]() { if (!run.empty()) { D.setCursor(runX, y); D.print(run.c_str()); run.clear(); } };
  while (i < t.size()) {
    size_t len = 0; int k = size == 2 ? emoticonMatch(t.c_str(), t.size(), i, &len) : -1;
    if (k >= 0) {
      int cx = x + (int)i * cw; flush();
      int ex = cx + ((int)len * cw - 18) / 2;
      if (!drawEmoticon(D, ex, y, emoticonIconIndex(k))) { D.setCursor(cx, y); D.print(t.substr(i, len).c_str()); }
      i += len; runX = x + (int)i * cw;
    } else { if (run.empty()) runX = x + (int)i * cw; run += t[i]; i++; }
  }
  flush();
}
static void wrapInto(std::vector<std::string> &out, const std::string &t, size_t width) {
  size_t i = 0;
  if (t.empty()) { out.push_back(""); return; }
  while (i < t.size()) {
    size_t take = std::min(width, t.size() - i);
    if (i + take < t.size()) { size_t sp = t.rfind(' ', i + take); if (sp != std::string::npos && sp > i) take = sp - i; }
    out.push_back(t.substr(i, take)); i += take; while (i < t.size() && t[i] == ' ') i++;
  }
}
static std::string ageText(uint32_t epoch) {
  uint32_t now = epochNow(); if (!epoch || !now || now < epoch) return "";
  char b[12]; geoFmtAge(b, sizeof b, now - epoch); return std::string(b) + " ago";
}
static uint16_t stateColor(uint8_t st) {
  switch (st) { case msg::S_ACKED: return TFT_GREEN; case msg::S_FAILED: case msg::S_REJECTED: return TFT_RED; case msg::S_WAIT: return TFT_YELLOW; default: return C_GRAY; }
}
static std::string stateText(const msg::Msg &x, uint32_t now) {
  char b[48];
  switch (x.state) {
    case msg::S_QUEUED: return "queued";
    case msg::S_WAIT: snprintf(b, sizeof b, "sent %d/%d  retry in %us", x.tries, msg::MAX_TRIES, (unsigned)M.secondsToRetry(x, now)); return b;
    case msg::S_ACKED: return "delivered";
    case msg::S_REJECTED: return "rejected";
    case msg::S_FAILED: return "failed: no ack";
    case msg::S_CANCELLED: return "cancelled";
  }
  return "";
}

// Typing: letters from the keyboard, Alt held = symbols/digits, Shift = next letter capital.
class TextEdit {
public:
  char buf[72] = ""; int len = 0, maxLen = 67; bool callMode = false, shiftOnce = false, altHeld = false;
  enum R { NONE, CHANGED, ENTER, BACK_EMPTY };
  void clear() { len = 0; buf[0] = 0; shiftOnce = altHeld = false; }
  void set(const char *s) { strncpy(buf, s, maxLen); buf[maxLen] = 0; len = strlen(buf); }
  R feed(uint8_t raw, bool down) {
    if (raw == KEY_ALT) { altHeld = down; return NONE; }
    if (!down) return NONE;
    if (raw == KEY_SHIFT) { shiftOnce = !shiftOnce; return CHANGED; }
    if (raw == KEY_ENTER) return ENTER;
    if (raw == KEY_BACKSPACE) { if (!len) return BACK_EMPTY; buf[--len] = 0; return CHANGED; }
    KeyInfo k = keyLookup(raw, altHeld, shiftOnce && !callMode);
    if (!k.ch || (unsigned char)k.ch < 32 || (k.mod != KM_NONE && k.mod != KM_SPACE) || len >= maxLen) return NONE;     // never a control character
    if (callMode && (k.ch == ' ' || !(isalnum((unsigned char)k.ch) || k.ch == '-'))) return NONE;
    char c = callMode ? toupper((unsigned char)k.ch) : k.ch;
    buf[len++] = c; buf[len] = 0;
    if (k.ch >= 'a' && k.ch <= 'z') shiftOnce = false;
    return CHANGED;
  }
};

enum UiScreen { UI_OFF, UI_CONVS, UI_THREAD, UI_COMPOSE };
enum UiAct { A_NONE, A_UNDO, A_REPLY, A_CANCEL_RETRY, A_DEL_MSG, A_DEL_CONV_ASK, A_DEL_ALL_ASK, A_YES, A_NO, A_BACK_LIST, A_BACK_MAP, A_NEW, A_DEL_CONV_SEL_ASK };

struct MsgUi {
  UiScreen scr = UI_OFF;
  int convSel = 0, thrSel = 0, thrTop = 0, thrCount = 0, focus = 1, canned = -1;
  std::string peer, confirmPeer; bool confirmAll = false;
  TextEdit to, body;
  bool menuOn = false; int menuSel = 0, menuN = 0; char menuItems[7][26]; UiAct menuAct[7]; char menuTitle[40] = "";
  char notice[56] = ""; uint32_t noticeUntil = 0; bool noticeBad = false;
  bool dirty = true;

  bool active() const { return scr != UI_OFF; }
  void say(const char *m, bool bad = false) { strncpy(notice, m, sizeof notice - 1); notice[sizeof notice - 1] = 0; noticeBad = bad; noticeUntil = millis() + 3000; dirty = true; }
  void open() { scr = UI_CONVS; menuOn = false; dirty = true; }
  void close() { scr = UI_OFF; menuOn = false; }
  void openThread(const std::string &p) {
    peer = p; scr = UI_THREAD; menuOn = false; M.markRead(peer);
    thrSel = (int)M.thread(peer).size() - 1; thrCount = thrSel + 1; thrTop = 0; dirty = true;
  }
  void openCompose(const char *toCall) {
    scr = UI_COMPOSE; menuOn = false; canned = -1; body.clear(); to.clear(); to.maxLen = 9; to.callMode = true; body.maxLen = msg::MAX_TEXT;
    if (toCall && *toCall) { to.set(toCall); focus = 1; } else focus = 0;
    dirty = true;
  }
  // an incoming message arrived: if we are looking at that thread, it is read at once
  void onIncoming(const char *from) { if (scr == UI_THREAD && peer == from) M.markRead(peer); dirty = true; }

  // ---------------------------------------------------------------- menus
  void menuAdd(const char *t, UiAct a) { if (menuN < 7) { strncpy(menuItems[menuN], t, 25); menuItems[menuN][25] = 0; menuAct[menuN++] = a; } }
  void showMenu() {
    menuN = 0; menuSel = 0; menuOn = true; dirty = true; confirmAll = false;
    uint32_t now = millis();
    if (scr == UI_CONVS) {
      strcpy(menuTitle, "Messages");
      if (M.canUndo(now)) menuAdd("Undo delete", A_UNDO);
      menuAdd("New message", A_NEW); menuAdd("Delete conversation", A_DEL_CONV_SEL_ASK); menuAdd("Delete ALL messages", A_DEL_ALL_ASK); menuAdd("Back to map", A_BACK_MAP);
    } else if (scr == UI_THREAD) {
      snprintf(menuTitle, sizeof menuTitle, "%s", peer.c_str());
      auto th = M.thread(peer); bool open = !th.empty() && thrSel >= 0 && thrSel < (int)th.size() && M.m[th[thrSel]].open();
      if (M.canUndo(now)) menuAdd("Undo delete", A_UNDO);
      menuAdd("Reply", A_REPLY); if (open) menuAdd("Cancel retries", A_CANCEL_RETRY);
      menuAdd("Delete message", A_DEL_MSG); menuAdd("Delete conversation", A_DEL_CONV_ASK); menuAdd("Back to list", A_BACK_LIST);
    } else if (scr == UI_COMPOSE) {
      strcpy(menuTitle, "Compose"); menuAdd("Keep writing", A_NO); menuAdd("Discard", A_BACK_LIST);
    }
  }
  void confirmMenu(const char *title) {
    menuN = 0; menuSel = 1; menuOn = true; strncpy(menuTitle, title, sizeof menuTitle - 1); menuTitle[sizeof menuTitle - 1] = 0;
    menuAdd("Yes, delete", A_YES); menuAdd("No", A_NO); dirty = true;
  }
  void backToConvs() { scr = UI_CONVS; menuOn = false; dirty = true; }
  void act(UiAct a) {
    uint32_t now = millis(); menuOn = false; dirty = true;
    switch (a) {
      case A_UNDO: { size_t n = M.undo(now); say(n ? "Restored" : "Nothing to undo"); break; }
      case A_NEW: openCompose(selCall[0] ? selCall : ""); break;
      case A_REPLY: openCompose(peer.c_str()); break;
      case A_CANCEL_RETRY: { auto th = M.thread(peer); if (thrSel >= 0 && thrSel < (int)th.size()) { M.cancel(M.m[th[thrSel]].seq); say("Retries cancelled"); } break; }
      case A_DEL_MSG: { auto th = M.thread(peer); if (thrSel >= 0 && thrSel < (int)th.size()) { M.removeMessage(M.m[th[thrSel]].seq, now); say("Deleted. Hold knob, Undo"); } if (M.thread(peer).empty()) backToConvs(); break; }
      case A_DEL_CONV_ASK: confirmPeer = peer; confirmAll = false; { char t[40]; snprintf(t, sizeof t, "Delete chat with %s?", peer.c_str()); confirmMenu(t); } break;
      case A_DEL_CONV_SEL_ASK: { auto cv = M.conversations(); if (convSel >= 0 && convSel < (int)cv.size()) { confirmPeer = cv[convSel].peer; confirmAll = false; char t[40]; snprintf(t, sizeof t, "Delete chat with %s?", confirmPeer.c_str()); confirmMenu(t); } break; }
      case A_DEL_ALL_ASK: confirmAll = true; confirmMenu("Delete ALL messages?"); break;
      case A_YES:
        if (confirmAll) M.removeAll(now); else M.removeConversation(confirmPeer, now);
        say("Deleted. Hold knob, Undo"); backToConvs(); convSel = 0; break;
      case A_NO: break;
      case A_BACK_LIST: backToConvs(); break;
      case A_BACK_MAP: close(); break;
      default: break;
    }
  }

  // ---------------------------------------------------------------- input
  // steps > 0 = turned "up" (up the list, towards older messages)
  void knob(int steps) {
    dirty = true;
    if (menuOn) { menuSel = constrain(menuSel + (steps > 0 ? -1 : 1), 0, menuN - 1); return; }
    int dir = steps > 0 ? -1 : 1;
    if (scr == UI_CONVS) { int n = (int)M.conversations().size(); convSel = constrain(convSel + dir, 0, std::max(0, n - 1)); }
    else if (scr == UI_THREAD) { int n = (int)M.thread(peer).size(); thrSel = constrain(thrSel + dir, 0, std::max(0, n - 1)); }
    else if (scr == UI_COMPOSE) {                         // turn = canned replies
      canned = (canned + (steps > 0 ? -1 : 1) + NCANNED + 1) % (NCANNED + 1);
      if (canned == NCANNED) { canned = -1; }
      if (canned >= 0) { body.set(CANNED[canned]); focus = 1; }
    }
  }
  void press(bool longPress) {
    dirty = true;
    if (menuOn) { if (longPress) menuOn = false; else act(menuAct[menuSel]); return; }
    if (longPress) { showMenu(); return; }
    if (scr == UI_CONVS) { auto cv = M.conversations(); if (!cv.empty() && convSel < (int)cv.size()) openThread(cv[convSel].peer); else openCompose(selCall); }
    else if (scr == UI_THREAD) openCompose(peer.c_str());
    else if (scr == UI_COMPOSE) trySend();
  }
  void trySend() {
    if (focus == 0) { if (to.len) focus = 1; return; }
    const char *err = nullptr;
    uint32_t seq = M.send(to.buf, body.buf, millis(), epochNow(), &err);
    if (!seq) { say(err ? err : "Cannot send", true); haptic.play(H_ERROR); return; }
    haptic.play(H_CLICK);
    std::string p = msg::upperStr(to.buf); openThread(p); say("Queued");
  }
  void key(uint8_t raw, bool down) {
    dirty = true;
    if (menuOn) { if (down && raw == KEY_ENTER) act(menuAct[menuSel]); else if (down && raw == KEY_BACKSPACE) menuOn = false; return; }
    char ch = down ? keyLookup(raw).ch : 0;
    if (scr == UI_CONVS) {
      if (!down) return;
      if (ch == 'n') openCompose(selCall[0] ? selCall : "");
      else if (ch == 'm' || raw == KEY_BACKSPACE) close();
      else if (raw == KEY_ENTER) press(false);
      else if (ch == 'd') { menuOn = false; act(A_DEL_CONV_SEL_ASK); }
    } else if (scr == UI_THREAD) {
      if (!down) return;
      if (raw == KEY_BACKSPACE) backToConvs();
      else if (ch == 'm') close();                       // M = back to the map from the list or a thread (in compose it is a letter)
      else if (raw == KEY_ENTER) openCompose(peer.c_str());
      else if (ch == 'd') act(A_DEL_MSG);
    } else if (scr == UI_COMPOSE) {
      TextEdit &te = focus == 0 ? to : body;
      TextEdit::R r = te.feed(raw, down);
      if (r == TextEdit::ENTER) trySend();
      else if (r == TextEdit::BACK_EMPTY) { if (focus == 1 && to.len) focus = 0; else if (focus == 1 && !to.len) focus = 0; else backToConvs(); }
      else if (r == TextEdit::CHANGED) { if (focus == 1) canned = -1; }
    }
  }

  // ---------------------------------------------------------------- drawing
  void header(lgfx::LovyanGFX &D, const char *title, int unread) {
    D.fillRect(0, 0, 480, 16, C_BAR); D.setTextSize(1); D.setTextColor(TFT_CYAN, C_BAR); D.setCursor(4, 4); D.print(title);
    D.setTextColor(M.callOk() ? C_GRAY : TFT_ORANGE, C_BAR); D.setCursor(300, 4);
    if (M.callOk()) D.printf("as %s", M.myCall()); else D.print("set your call sign: Enter on the map");
    if (unread > 0) { D.setTextColor(TFT_YELLOW, C_BAR); D.setCursor(220, 4); D.printf("%d unread", unread); }
  }
  void footer(lgfx::LovyanGFX &D, const char *hint) {
    D.fillRect(0, 210, 480, 12, C_BAR); D.setTextSize(1);
    if (notice[0] && millis() < noticeUntil) { D.setTextColor(noticeBad ? TFT_RED : TFT_GREEN, C_BAR); D.setCursor(4, 212); D.print(notice); return; }
    if (M.canUndo(millis())) { D.setTextColor(TFT_YELLOW, C_BAR); D.setCursor(4, 212); D.printf("Deleted. Hold the knob for Undo (%us)", (unsigned)((M.trashSecondsLeft(millis())))); return; }
    D.setTextColor(C_GRAY, C_BAR); D.setCursor(4, 212); D.print(hint);
  }
  void drawConvs(lgfx::LovyanGFX &D) {
    auto cv = M.conversations(); const int rowH = 30, top = 18, rows = (210 - top) / rowH;
    header(D, "Messages", M.unreadTotal());
    convSel = constrain(convSel, 0, std::max(0, (int)cv.size() - 1));
    if (cv.empty()) { D.setTextSize(2); D.setTextColor(C_GRAY, C_BG); D.setCursor(20, 80); D.print("No messages yet."); D.setCursor(20, 104); D.print("Press N to write one."); }
    int first = constrain(convSel - rows / 2, 0, std::max(0, (int)cv.size() - rows));
    for (int r = 0; r < rows && first + r < (int)cv.size(); r++) {
      const msg::Conv &c = cv[first + r]; const msg::Msg &last = M.m[c.lastIdx]; bool sel = first + r == convSel;
      int y = top + r * rowH; uint16_t bg = sel ? C_SEL : C_BG;
      D.fillRect(0, y, 480, rowH, bg);
      drawPeerIcon(D, 16, y + 15, c.peer.c_str());
      D.setTextSize(2); D.setTextColor(c.unread ? TFT_YELLOW : TFT_CYAN, bg); D.setCursor(36, y + 1); D.print(c.peer.c_str());
      D.setTextSize(1);
      if (c.unread) { D.fillCircle(176, y + 9, 8, TFT_YELLOW); D.setTextColor(TFT_BLACK, TFT_YELLOW); D.setCursor(c.unread > 9 ? 170 : 173, y + 6); D.print(c.unread); }
      std::string age = ageText(last.epoch); D.setTextColor(C_GRAY, bg); D.setCursor(480 - 6 * (int)age.size() - 6, y + 4); D.print(age.c_str());
      std::string pv = (last.out ? "> " : "") + std::string(last.text); if (pv.size() > 72) pv = pv.substr(0, 71) + "~";
      D.setTextColor(last.out ? stateColor(last.state) : TFT_WHITE, bg); D.setCursor(36, y + 19); D.print(pv.c_str());
    }
    footer(D, "turn: select  press: open  hold: menu   N: new  M: map");
  }
  void drawThread(lgfx::LovyanGFX &D) {
    auto th = M.thread(peer); uint32_t now = millis();
    thrSel = constrain(thrSel, 0, std::max(0, (int)th.size() - 1));
    if ((int)th.size() > thrCount && thrSel == thrCount - 1) thrSel = (int)th.size() - 1;     // follow new messages when at the bottom
    thrCount = (int)th.size();
    D.fillRect(0, 0, 480, 24, C_BAR); drawPeerIcon(D, 16, 12, peer.c_str());
    D.setTextSize(2); D.setTextColor(TFT_CYAN, C_BAR); D.setCursor(34, 4); D.print(peer.c_str());
    D.setTextSize(1);
    if (const Station *st = stationFor(peer.c_str())) if (gps.fix.valid) {
      double b = geoBearing(gps.fix.lat, gps.fix.lon, st->lat, st->lon); char ds[10]; geoFmtDist(ds, sizeof ds, geoDistKm(gps.fix.lat, gps.fix.lon, st->lat, st->lon));
      D.setTextColor(C_GRAY, C_BAR); D.setCursor(300, 9); D.printf("%s  %.0f %s", ds, b, geoCompass(b));
    }
    // build the lines: each message = its text lines (big) + one status line (small)
    struct Ln { std::string t; uint16_t col; bool right; int msgPos; bool meta; int h; };
    std::vector<Ln> ln; std::vector<int> firstLine(th.size()), lastLine(th.size());
    for (size_t i = 0; i < th.size(); i++) {
      const msg::Msg &x = M.m[th[i]]; std::vector<std::string> w; wrapInto(w, x.text, 36);
      firstLine[i] = (int)ln.size();
      for (auto &s : w) ln.push_back({s, x.out ? (uint16_t)TFT_GREEN : (uint16_t)TFT_WHITE, x.out, (int)i, false, 18});
      std::string meta;
      if (x.out) { meta = stateText(x, now); if (x.state == msg::S_QUEUED && heldBehind(th, i)) meta = "waiting for the earlier message"; }
      else { std::string a = ageText(x.epoch), sig; if (x.rssi) { char b[40]; if (x.snr4) snprintf(b, sizeof b, "%d dBm  SNR %+.1f", x.rssi, x.snr4 / 4.0); else snprintf(b, sizeof b, "%d dBm", x.rssi); sig = b; } meta = a + (!a.empty() && !sig.empty() ? "  " : "") + sig; }
      ln.push_back({meta, x.out ? stateColor(x.state) : (uint16_t)0xC618, x.out, (int)i, true, 13});
      lastLine[i] = (int)ln.size() - 1;
    }
    const int top = 28, visH = 208 - top;
    auto fits = [&](int from, int to) { int h = 0; for (int k = from; k <= to; k++) h += ln[k].h; return h <= visH; };
    if (!th.empty()) {
      if (firstLine[thrSel] < thrTop) thrTop = firstLine[thrSel];
      while (thrTop < firstLine[thrSel] && !fits(thrTop, lastLine[thrSel])) thrTop++;
    }
    thrTop = std::max(0, std::min(thrTop, std::max(0, (int)ln.size() - 1)));
    int y = top;
    for (int k = thrTop; k < (int)ln.size() && y + ln[k].h <= 208; k++) {
      const Ln &l = ln[k]; bool sel = l.msgPos == thrSel;
      if (sel) D.fillRect(0, y, 480, l.h, C_SEL);
      uint16_t bg = sel ? C_SEL : C_BG; D.setTextColor(l.col, bg); D.setTextSize(l.meta ? 1 : 2);
      int cw = l.meta ? 6 : 12, x = l.right ? 480 - cw * (int)l.t.size() - 8 : 8;
      if (l.meta) { D.setCursor(x, y + 3); D.print(l.t.c_str()); } else drawRich(D, x, y, l.t, 2, l.col, bg);
      y += l.h;
    }
    D.setTextSize(1);
    if (th.empty()) { D.setTextColor(C_GRAY, C_BG); D.setCursor(20, 90); D.print("No messages with this station."); }
    footer(D, "turn: select  press: reply  hold: menu  Backspace: list  M: map");
  }
  // a queued message that has an earlier open message to the same station ahead of it
  bool heldBehind(const std::vector<size_t> &th, size_t pos) const {
    for (size_t i = 0; i < pos; i++) if (M.m[th[i]].open()) return true;
    return false;
  }
  void drawCompose(lgfx::LovyanGFX &D) {
    header(D, "New message", 0);
    // To: call sign, big
    D.setTextSize(2); D.setTextColor(C_GRAY, C_BG); D.setCursor(6, 30); D.print("To");
    uint16_t toBg = focus == 0 ? C_SEL : C_BAR;
    D.fillRect(36, 20, 204, 34, toBg);
    D.setTextSize(3); D.setTextColor(TFT_CYAN, toBg); D.setCursor(42, 26); D.print(to.buf);
    if (focus == 0 && (millis() / 500) % 2) D.print("_");
    drawPeerIcon(D, 270, 37, to.buf);
    // message text, big
    uint16_t bBg = focus == 1 ? C_SEL : C_BAR;
    D.fillRect(6, 60, 468, 104, bBg);
    std::vector<std::string> w; std::string t = body.buf; if (focus == 1 && (millis() / 500) % 2) t += "_"; wrapInto(w, t, 25);
    D.setTextSize(3); D.setTextColor(TFT_WHITE, bBg);
    for (size_t i = 0; i < w.size() && i < 4; i++) { D.setCursor(12, 64 + (int)i * 25); D.print(w[i].c_str()); }
    // counter and airtime
    std::string info = msg::messageInfo(to.len ? to.buf : "N0CALL", body.buf, "12345");
    int frame = 3 + (int)msg::tnc2(S.call, TOCALL, S.path, info).size();
    D.setTextSize(2); D.setTextColor(body.len > 60 ? TFT_ORANGE : C_GRAY, C_BG); D.setCursor(8, 168);
    D.printf("%d/%d   %.1f s on air", body.len, msg::MAX_TEXT, msg::airtimeMs(frame) / 1000.0);
    D.setTextSize(1);
    D.setCursor(8, 188); D.setTextColor(canned >= 0 ? TFT_YELLOW : C_GRAY, C_BG);
    D.printf("Turn the knob for quick replies: %s", canned >= 0 ? CANNED[canned] : "(none)");
    D.setCursor(8, 199); D.setTextColor(C_GRAY, C_BG); D.print("Alt held = digits and symbols   Shift = next letter capital");
    footer(D, focus == 0 ? "type the call sign, Enter: next" : "Enter or press: send   hold: menu   Backspace: edit To");
  }
  void drawMenu(lgfx::LovyanGFX &D) {
    int h = 22 + menuN * 18, y = (222 - h) / 2, x = 120, w = 240;
    D.fillRect(x - 2, y - 2, w + 4, h + 4, TFT_WHITE); D.fillRect(x, y, w, h, C_BG);
    D.setTextSize(1); D.setTextColor(TFT_CYAN, C_BG); D.setCursor(x + 8, y + 6); D.print(menuTitle);
    for (int i = 0; i < menuN; i++) {
      int yy = y + 20 + i * 18; bool s = i == menuSel;
      D.fillRect(x + 2, yy, w - 4, 16, s ? TFT_YELLOW : C_BG);
      D.setTextColor(s ? TFT_BLACK : TFT_WHITE, s ? TFT_YELLOW : C_BG); D.setCursor(x + 10, yy + 4); D.print(menuItems[i]);
    }
  }
  void draw(lgfx::LovyanGFX &D) {
    D.fillScreen(C_BG);
    if (scr == UI_CONVS) drawConvs(D); else if (scr == UI_THREAD) drawThread(D); else if (scr == UI_COMPOSE) drawCompose(D);
    if (menuOn) drawMenu(D);
    dirty = false;
  }
};
