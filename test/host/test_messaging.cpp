// Host tests for src/messaging.h.  Run:  c++ -std=c++17 -I src test/host/test_messaging.cpp -o /tmp/test_messaging && /tmp/test_messaging
#include <cstdio>
#include "messaging.h"
#include "aprs.h"
using namespace msg;
static int fails = 0;
static void expect(bool c, const char *w) { if (!c) { fails++; printf("FAIL: %s\n", w); } }
static Parsed P(const std::string &s) { return parse(s.c_str(), s.size()); }
static Messenger mk() { Messenger M; M.setCall("K6ATV-6"); return M; }

int main() {
  // ---- parsing ----
  { Parsed p = P("K6ATV-4>APLRT1,WIDE1-1::K6ATV-6  :hello there{42");
    expect(p.kind == K_MESSAGE && !strcmp(p.from, "K6ATV-4") && !strcmp(p.to, "K6ATV-6") && p.text == "hello there" && !strcmp(p.id, "42"), "numbered message");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :no number"); expect(p.kind == K_MESSAGE && p.id[0] == 0 && p.text == "no number", "unnumbered message");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :ack42"); expect(p.kind == K_ACK && !strcmp(p.id, "42"), "ack");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :rej42"); expect(p.kind == K_REJ && !strcmp(p.id, "42"), "rej");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :ack7}9"); expect(p.kind == K_ACK && !strcmp(p.id, "7"), "ack with a reply-ack tail is still an ack of 7");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :thanks{12}7"); expect(p.kind == K_MESSAGE && p.text == "thanks" && !strcmp(p.id, "12") && !strcmp(p.replyAck, "7"), "reply-ack");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :reject this"); expect(p.kind == K_MESSAGE && p.text == "reject this", "'reject this' is a message, not a rej");
    p = P("K6ATV-4>APLRT1::K6ATV-6  :ackbar"); expect(p.kind == K_ACK && p.text == "ackbar", "'ackbar' parses as an ack candidate that keeps its text");
    p = P("K6ATV-4>APLRT1::BLN1     :power outage"); expect(p.kind == K_BULLETIN, "bulletin");
    p = P("K6ATV-4>APLRT1::ALL      :cq cq"); expect(p.kind == K_BULLETIN, "ALL is group");
    p = P("K6ATV-4>APLRT1:!3720.50N/12154.30W>"); expect(p.kind == K_NONE, "position is not a message");
    p = P("K6ATV-4>APLRT1::k6atv-6  :lower{1"); expect(p.kind == K_MESSAGE && !strcmp(p.to, "K6ATV-6"), "addressee case-insensitive");
    p = P("K6ATV-4>APLRT1::K6ATV-6 :short"); expect(p.kind == K_NONE, "8-char addressee field is not a message");
    p = P("garbage"); expect(p.kind == K_NONE, "garbage"); }

  // ---- receiving: ack every copy, show once ----
  { Messenger M = mk(); uint32_t t = 100000;
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :hi{5"), t, 0, -90) == EV_NEW && M.m.size() == 1 && !M.m[0].read && !M.m[0].out, "new message stored unread");
    TxItem it; expect(!M.nextTx(t + 1000, it), "ack not before its delay");
    expect(M.nextTx(t + 1600, it) && it.isAck && it.info == ":K6ATV-4  :ack5", "ack goes out after the delay");
    M.txDone(true, t + 1600);
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :hi{5"), t + 10000, 0, -90) == EV_DUP && M.m.size() == 1, "repeat is shown once");
    expect(!M.nextTx(t + 20000, it), "no second ack inside 30 s of the first");
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :hi{5"), t + 40000, 0, -90) == EV_DUP, "later repeat");
    expect(M.nextTx(t + 45000, it) && it.isAck, "the later copy is acked again");
    M.txDone(true, t + 45000);
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :other text{5"), t + 50000, 0, -90) == EV_NEW && M.m.size() == 2, "same number, different text is a new message");
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :no number"), t + 51000, 0, -90) == EV_NEW, "unnumbered stored");
    TxItem x; bool anyAck = false; for (int i = 0; i < 3; i++) { if (M.nextTx(t + 60000 + i * 4000, x)) { if (x.isAck && x.info.find("no number") != std::string::npos) anyAck = true; M.txDone(true, t + 60000 + i * 4000); } }
    expect(!anyAck, "unnumbered is never acked");
    expect(M.onRx(P("K6ATV-4>X::W1AW      :not for me{3"), t, 0, -90) == EV_NONE, "message for someone else ignored");
    expect(M.onRx(P("K6ATV-4>X::BLN1     :bulletin"), t, 0, -90) == EV_NONE, "bulletins not stored yet");
    expect(M.unreadTotal() == 3, "unread count"); M.markRead("K6ATV-4"); expect(M.unreadTotal() == 0, "marked read"); }

  // ---- sending, retries, acks ----
  { Messenger M = mk(); uint32_t t = 1000; const char *err = nullptr;
    expect(M.send("k6atv-4", "hello", t, 0, &err) == 1 && M.m[0].state == S_QUEUED && M.m[0].peer == std::string("K6ATV-4"), "send queues");
    TxItem it; expect(M.nextTx(t, it) && !it.isAck && it.info == ":K6ATV-4  :hello{1", "first transmit");
    expect(!M.nextTx(t, it), "nothing else while one is in flight");
    M.txDone(true, t); expect(M.m[0].state == S_WAIT && M.m[0].tries == 1, "waiting for ack after try 1");
    expect(!M.nextTx(t + 29000, it), "no retry inside 30 s"); expect(M.secondsToRetry(M.m[0], t + 20000) == 10, "countdown");
    expect(M.nextTx(t + 31000, it), "retry after 30 s"); M.txDone(true, t + 31000);
    expect(!M.nextTx(t + 31000 + 59000, it) && M.nextTx(t + 31000 + 61000, it), "second wait is 60 s"); M.txDone(false, t + 92000);
    expect(M.m[0].tries == 2, "a busy channel does not use up a try");
    expect(M.nextTx(t + 92000 + 31000, it), "busy: retry in 30 s"); M.txDone(true, t + 123000);
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :ack1"), t + 130000, 0, -90) == EV_ACKED && M.m[0].state == S_ACKED, "ack settles it");
    expect(!M.nextTx(t + 1000000, it), "no more retries after the ack");
    expect(M.onRx(P("W1AW>X::K6ATV-6  :ack1"), t, 0, -90) == EV_NONE && M.m.size() == 1, "a stray numeric ack from another station is dropped");
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :ack1"), t + 200000, 0, -90) == EV_NONE && M.m.size() == 1, "a repeated ack of an already settled message is ignored");
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :ackbar"), t + 200000, 0, -90) == EV_NEW && std::string(M.m[1].text) == "ackbar", "'ackbar' is shown as a message"); }
  { Messenger M = mk(); uint32_t t = 1000; const char *err; M.send("K6ATV-4", "x", t, 0, &err);
    TxItem it; uint32_t now = t; int sent = 0;
    for (int step = 0; step < 200000 && M.m[0].open(); step++) { now += 1000; if (M.nextTx(now, it)) { M.txDone(true, now); sent++; } }
    expect(sent == 6 && M.m[0].state == S_FAILED, "six tries, then failed");
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :ack1"), now, 0, -90) == EV_ACKED && M.m[0].state == S_ACKED, "a late ack still counts"); }
  { Messenger M = mk(); const char *err; M.send("K6ATV-4", "x", 0, 0, &err); TxItem it; M.nextTx(0, it); M.txDone(true, 0);
    expect(M.onRx(P("K6ATV-4>X::K6ATV-6  :rej1"), 5000, 0, -90) == EV_REJECTED && M.m[0].state == S_REJECTED, "rej stops it"); }
  { Messenger M = mk(); const char *err; uint32_t t = 0;
    M.send("K6ATV-4", "first", t, 0, &err); M.send("K6ATV-4", "second", t, 0, &err); M.send("W1AW", "other", t, 0, &err);
    TxItem a, b; expect(M.nextTx(t, a) && a.info == ":K6ATV-4  :first{1", "oldest first"); M.txDone(true, t);
    expect(M.nextTx(t + 3100, b) && b.info == ":W1AW     :other{3", "second to the same addressee waits, another addressee goes");
    M.txDone(true, t + 3100);
    M.onRx(P("K6ATV-4>X::K6ATV-6  :ack1"), t + 5000, 0, -90);
    TxItem c; expect(M.nextTx(t + 7000, c) && c.info == ":K6ATV-4  :second{2", "next message goes once the first is settled"); }
  { Messenger M = mk(); uint32_t t = 0; TxItem it; const char *err = nullptr; M.send("W1AW", "a", t, 0, &err);
    M.nextTx(t, it); M.txDone(true, t); M.send("N7XYZ", "b", t, 0, &err);
    expect(!M.nextTx(t + 1000, it), "two of our messages are never closer than 3 s"); expect(M.nextTx(t + 3100, it), "after the gap"); }

  // ---- send checks ----
  { Messenger M; const char *err = nullptr; expect(M.send("K6ATV-4", "x", 0, 0, &err) == 0 && err, "no call sign set: refused");
    M.setCall("K6ATV-6"); expect(M.send("hello", "x", 0, 0, &err) == 0, "bad To refused"); expect(M.send("K6ATV-4", "", 0, 0, &err) == 0, "empty refused");
    expect(M.send("K6ATV-4", std::string(68, 'a'), 0, 0, &err) == 0, "68 characters refused"); expect(M.send("K6ATV-4", std::string(67, 'a'), 0, 0, &err) != 0, "67 allowed");
    expect(M.send("K6ATV-4", "a{b", 0, 0, &err) == 0 && M.send("K6ATV-4", "a|b", 0, 0, &err) == 0 && M.send("K6ATV-4", "a~b", 0, 0, &err) == 0, "reserved characters refused");
    expect(M.send("K6ATV-4", "tab\there", 0, 0, &err) == 0, "control character refused"); }
  { Messenger M; M.setCall("N0CALL"); const char *err; expect(M.send("K6ATV-4", "x", 0, 0, &err) == 0, "N0CALL cannot send"); }

  // ---- conversations and delete with undo ----
  { Messenger M = mk(); uint32_t t = 1000; const char *err;
    M.onRx(P("K6ATV-4>X::K6ATV-6  :one{1"), t, 0, -90); M.send("W1AW", "two", t, 0, &err); M.onRx(P("K6ATV-4>X::K6ATV-6  :three{2"), t, 0, -90);
    auto cv = M.conversations(); expect(cv.size() == 2 && cv[0].peer == "K6ATV-4" && cv[0].count == 2 && cv[0].unread == 2 && cv[1].peer == "W1AW", "conversations newest first");
    expect(M.thread("K6ATV-4").size() == 2, "thread");
    M.removeMessage(M.m[1].seq, t + 100); expect(M.m.size() == 2 && M.canUndo(t + 5000), "message deleted, undo offered");
    expect(M.undo(t + 5000) == 1 && M.m.size() == 3 && M.m[1].seq == 2, "undo restores it in place");
    M.removeConversation("K6ATV-4", t + 100); expect(M.m.size() == 1, "conversation deleted");
    expect(!M.canUndo(t + 100 + 11000) && M.undo(t + 100 + 11000) == 0, "undo expires after 10 s");
    M.removeAll(t); expect(M.m.empty(), "delete all");
  }
  { Messenger M = mk(); const char *err; TxItem it; M.send("W1AW", "to cancel", 0, 0, &err); uint32_t s = M.m[0].seq; M.removeMessage(s, 0); expect(!M.nextTx(5000, it), "deleting an open message cancels its retries"); }
  { Messenger M = mk(); const char *err; M.send("W1AW", "x", 0, 0, &err); M.cancel(M.m[0].seq); TxItem it; expect(M.m[0].state == S_CANCELLED && !M.nextTx(5000, it), "cancel stops retries"); }

  // ---- saving and loading ----
  { Messenger M = mk(); const char *err; M.onRx(P("K6ATV-4>X::K6ATV-6  :saved{9"), 0, 1700000000, -88); M.send("W1AW", "pending", 0, 1700000100, &err);
    TxItem it; M.nextTx(5000, it); M.txDone(true, 5000);
    std::string s = M.serialize(); Messenger N = mk();
    expect(N.deserialize((const uint8_t *)s.data(), s.size()) && N.m.size() == M.m.size() && !strcmp(N.m[0].text, "saved") && N.m[0].epoch == 1700000000, "round trip");
    N.restoreTimers(100); TxItem r; expect(!N.nextTx(100, r) && N.nextTx(31000, r) && r.info == ":W1AW     :pending{" + std::string(N.m[1].id), "unacked message resumes ~30 s after loading");
    std::string bad = s; bad[0] = 'X'; expect(!N.deserialize((const uint8_t *)bad.data(), bad.size()), "bad magic refused");
    expect(!N.deserialize((const uint8_t *)s.data(), s.size() - 1), "truncated file refused");
    expect(!N.deserialize((const uint8_t *)s.data(), 3), "tiny file refused"); }

  // ---- the frame that actually goes on the air must parse back (this caught a missing colon: ":" ends the header, the info starts with ":") ----
  { std::string f = tnc2("K6ATV-6", "APZPGR", "WIDE1-1", messageInfo("K6ATV-4", "hello there", "12"));
    expect(f == "K6ATV-6>APZPGR,WIDE1-1::K6ATV-4  :hello there{12", "message frame text has two colons after the header");
    Parsed p = parse(f.c_str(), f.size()); expect(p.kind == K_MESSAGE && !strcmp(p.from, "K6ATV-6") && !strcmp(p.to, "K6ATV-4") && p.text == "hello there" && !strcmp(p.id, "12"), "message frame parses back");
    std::string a = tnc2("K6ATV-6", "APZPGR", "WIDE1-1", ackInfo("K6ATV-4", "7"));
    expect(a == "K6ATV-6>APZPGR,WIDE1-1::K6ATV-4  :ack7", "ack frame text");
    Parsed q = parse(a.c_str(), a.size()); expect(q.kind == K_ACK && !strcmp(q.to, "K6ATV-4") && !strcmp(q.id, "7"), "ack frame parses back");
    std::string np = tnc2("K6ATV-6", "APZPGR", "", messageInfo("W1AW", "x", "1")); expect(np == "K6ATV-6>APZPGR::W1AW     :x{1", "no path: no comma");
    // end to end: what one Messenger sends, another one receives and acks, and the first one settles
    Messenger A = mk(), B; B.setCall("K6ATV-4"); const char *err; A.send("K6ATV-4", "ping", 0, 0, &err);
    TxItem it; A.nextTx(0, it); A.txDone(true, 0);
    std::string air = tnc2("K6ATV-6", "APZPGR", "WIDE1-1", it.info);
    expect(B.onRx(parse(air.c_str(), air.size()), 1000, 0, -50) == EV_NEW && std::string(B.m[0].text) == "ping", "B receives A's message");
    TxItem ack; expect(!B.nextTx(6900, ack), "A's path asks for a digipeater: B waits for the repeat");
    expect(B.nextTx(7100, ack) && ack.isAck, "B acks");
    std::string air2 = tnc2("K6ATV-4", "APZPGR", "WIDE1-1", ack.info);
    expect(A.onRx(parse(air2.c_str(), air2.size()), 9000, 0, -50) == EV_ACKED && A.m[0].state == S_ACKED, "A sees the ack and settles the message"); }

  // ---- ack timing: do not talk over the digipeater that repeats the message ----
  { auto R = [](const char *f) { return parse(f, strlen(f)).mayRepeat; };
    expect(R("K6ATV-4>APX,WIDE1-1::K6ATV-6  :a{1") && R("K6ATV-4>APX,WIDE2-2::K6ATV-6  :a{1") && R("K6ATV-4>APX,N0DIG-3::K6ATV-6  :a{1"), "a repeat may come");
    expect(!R("K6ATV-4>APX::K6ATV-6  :a{1") && !R("K6ATV-4>APX,N0DIG-3,WIDE1*::K6ATV-6  :a{1") && !R("K6ATV-4>APX,N0DIG-3*,WIDE2-1::K6ATV-6  :a{1"), "no path, or already repeated");
    expect(!R("K6ATV-4>APX,WIDE1-0::K6ATV-6  :a{1") && !R("K6ATV-4>APX,TCPIP,NOGATE::K6ATV-6  :a{1") && !R("K6ATV-4>APX,qAO,RFONLY::K6ATV-6  :a{1"), "nothing left to repeat"); }
  const char *DIRECT = "K6ATV-4>APX,WIDE1-1::K6ATV-6  :via{8", *RPT = "K6ATV-4>APX,N0DIG-10,WIDE1*::K6ATV-6  :via{8";
  { Messenger M = mk(); uint32_t t = 100000; TxItem it;
    M.onRx(parse(DIRECT, strlen(DIRECT)), t, 0, -90);
    expect(!M.nextTx(t + 5900, it), "heard direct: not before 6 s");
    expect(M.nextTx(t + 6100, it) && it.isAck, "then acked"); }
  { Messenger M = mk(); uint32_t t = 100000; TxItem it;
    M.onRx(parse(DIRECT, strlen(DIRECT)), t, 0, -90); expect(M.onRx(parse(RPT, strlen(RPT)), t + 3000, 0, -90) == EV_DUP && M.m.size() == 1, "the repeat shows once");
    expect(!M.nextTx(t + 4400, it), "not on top of the repeat");
    expect(M.nextTx(t + 4600, it) && it.isAck, "1.5 s after the repeat, without waiting out the 6 s");
    M.txDone(true, t + 4600); expect(!M.nextTx(t + 20000, it), "one ack for the two copies");
    M.onRx(parse(DIRECT, strlen(DIRECT)), t + 32000, 0, -90);             // the sender's retry, about 30 s after its first try
    expect(!M.nextTx(t + 37900, it) && M.nextTx(t + 38100, it) && it.isAck, "a retry 32 s later is acked again"); }
  { Messenger M = mk(); uint32_t t = 100000; TxItem it;
    M.onRx(parse(DIRECT, strlen(DIRECT)), t, 0, -90); M.nextTx(t + 6100, it); M.txDone(true, t + 6100);
    M.onRx(parse(RPT, strlen(RPT)), t + 9000, 0, -90); expect(!M.nextTx(t + 20000, it), "a late repeated copy right after our ack adds no second ack");
    M.onRx(parse(DIRECT, strlen(DIRECT)), t + 30500, 0, -90); expect(M.nextTx(t + 36600, it) && it.isAck, "a retry 30.5 s after the first copy is not skipped"); }
  { Messenger M = mk(); uint32_t t = 100000; TxItem it;
    M.onRx(parse(RPT, strlen(RPT)), t, 0, -90); expect(M.nextTx(t + 1600, it) && it.isAck, "only heard repeated: acked after 1.5 s as before"); }

  // ---- signal strength is kept with a received message, and the saved-file layout did not change ----
  { Messenger M = mk(); M.onRx(P("K6ATV-4>X::K6ATV-6  :sig{1"), 0, 0, -97, (int)lround(-12.5 * 4));
    expect(M.m[0].rssi == -97 && M.m[0].snr4 == -50, "rssi and snr (quarter dB) stored");
    M.onRx(P("K6ATV-4>X::K6ATV-6  :loud{2"), 0, 0, -40, 400); expect(M.m[1].snr4 == 127, "snr is clamped to the int8 range");
    std::string s = M.serialize(); Messenger N = mk(); expect(N.deserialize((const uint8_t *)s.data(), s.size()) && N.m[0].snr4 == -50, "snr survives saving");
    expect(sizeof(Msg) == 204, "Msg is still 204 bytes, so files saved by earlier builds still load"); }

  // ---- limits ----
  { Messenger M = mk(); for (int i = 0; i < 350; i++) M.onRx(P("K6ATV-4>X::K6ATV-6  :m" + std::to_string(i) + "{" + std::to_string(i % 99999 + 1)), i * 1000, 0, -90);
    expect(M.m.size() == MAX_MESSAGES, "history is capped at 300"); }

  // ---- airtime: the real beacon was 2636 ms on air ----
  { unsigned char f[128]; size_t n = aprsBuildBeacon(f, sizeof f, "K6ATV-6", "APZPGR", "WIDE1-1", 37.0, -121.5, '/', '[', "T-LoRa Pager");
    double a = airtimeMs((int)n); printf("beacon %zu bytes -> %.0f ms (measured on air: 2636 ms)\n", n, a);
    expect(fabs(a - 2636) < 60, "airtime formula matches the measured beacon"); }

  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
