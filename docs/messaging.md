# Messaging design (draft, 2026-11-22)

Goal: make sending, answering and cleaning up APRS messages quick on a keyboard-and-encoder device, and be
more complete than the T-Deck screen (conversation list with unread badges, bubble thread, reply button,
long-press delete of one/selected messages or a whole conversation, message detail). The pager has no
confirmed touch screen, so everything is keys + encoder.

## APRS facts to build on
Verified 2026-11-22: chapter 14 of the APRS 1.2 draft c (wb2osz/aprsspec APRS12c.pdf, Nov 2024, an UNOFFICIAL compilation of
APRS101 plus the later updates; the "official" spec is APRS101 + errata) was read directly; the agent also read APRS101 ch.14
via a mirror, aprs.org/txt/messages101.txt, aprs.org/aprs11/replyacks.txt and aprs-is.net/IGating.aspx.
- Message packet: `:ADDRESSEE:text{msgno`. Addressee is a fixed 9-character field, space padded. Text is up to 67 characters,
  printable ASCII or UTF-8 except `{` (older text also excluded `|` and `~`: do not SEND those). In practice longer text than 67
  is seen, and the spec says receivers must handle it: always display the whole text, limit only what we send to 67. The message number is optional: `{` plus up to 5 alphanumerics, no spaces.
- A message without a number must NOT be acked. Ack is `ackNNN`, reject `rejNNN`, sent as message text.
- The sender repeats until acked, cancelled or timed out. The receiver acks EVERY copy of a repeated message (acks at least
  30 s apart, because digipeaters suppress duplicates for 30 s) but should show it only once.
- `ALL`, `QST`, `CQ` are group addressees: read, never acked. Queries start with `?`, have no number, are not acked.
- Bulletins: `BLN` + digit + 5 spaces (general), `BLN` + digit + group name padded to 5 (group, e.g. `BLN4WX`), `BLN` + capital
  letter (announcement). Repeated by the sender a few times an hour for a few hours (announcements far less often); a new line
  with the same ID from the same station overwrites the old one; alert on new or changed lines. NWS uses `NWS-xxxxx`.
- Reply-Ack (Dec 1999, in the 1.2 draft; optional and backward compatible; how widely implemented is unverified): send every
  message number as `{MM}AA` (AA = the free ack for the latest message received from that station, empty if none; a trailing `}`
  alone says "I can take reply-acks"). AA is attached only at the instant of transmission, so it can change between retries.
  On receive: if AA matches one of my outgoing MM, that message is acked; strip `}AA` before storing and duplicate checking;
  still send the normal `ackMM}AA` (an exact copy); remember the incoming MM as my AA for my next message to that station.
  A plain `ackMM` must also still match `{MM}` in the outgoing queue. Saves one packet per exchange, valuable on LoRa.
- Message groups (client-side): a user-defined list of callsigns whose messages a station also reads, plus it always reads
  `ALL`, `QST`, `CQ`; acks only messages addressed to itself. Bulletin groups: a user-defined list of group names; if the list is
  empty all bulletins are shown and alert, otherwise only those groups plus general bulletins.
- Bulletin Screen recommendations (spec): sort by originating callsign then ID, re-sort as lines arrive; a new line with the
  same ID from the same station replaces the old; user can clear any/all (they reappear if still valid); alert on any new or
  replaced line and re-alert periodically until the user acknowledges; log old bulletins.
- NTS radiograms (optional): messages whose text begins `N#\`, `NA\`, `N1\`..`N6\`, `NS\`, `NR\`; each line max 67 characters.
  Not planned; they display as ordinary messages.
- No 2025-26 official spec revision found; multi-line and longer-than-67 sending are not specified.
- No real group chat in APRS: groups are bulletin conventions or `CQ`/`ALL` addressing.

## Do bulletins show on aprs.fi? (verified 2026-11-22, aprs.fi user guide "Messages and Status")
Yes: aprs.fi has a Bulletin board page listing all bulletins recently transmitted and passed on to APRS-IS, with lookup of custom
bulletin groups. It is a view-only page (no alert; aprs.fi cannot transmit). A bulletin only gets there if an iGate hears it and
forwards it to APRS-IS; whether common LoRa APRS iGates forward bulletins is not verified.

## Reaching the pager from the internet (verified rules, inferred consequence)
An iGate may gate internet traffic to RF only for stations heard locally within the last hour or less, and must not gate
generic `?` queries. Inference: the pager is reachable from APRS-IS only if a TX-capable iGate nearby heard it within the hour,
so it needs to beacon regularly, and without such an iGate it can only talk over RF. Our own iGate is RF to internet only.

## Services reachable by message (status uncertain, check before promising anything in the UI)
EMAIL-2 (email gateway; the old server retired 2019, javAPRSSrvr replaced it; wants numbered, acked messages, first word is
the address; 2026 operation not confirmed). SMSGTE (conflicting reports, status unknown). Winlink via APRSlink (not read).
CA2RXU's tracker has an "APRSThursday" menu entry. WHO-IS/INFO/position responders: not verified.

## What existing clients do (survey 2026-11-22; web access was thin, so "not verified" matters)
Verified from sources: **APRSdroid** (changelog only) has a conversation view per callsign, resend and abort in the long-press
menu, "Clear messages" per callsign, and its history shows real bugs: duplicate notifications on retransmits (fixed 1.1.1), duplicate
message IDs (1.2.1), SSID filtering and ACK parsing (1.2.0, 1.2.3). **Kenwood TM-D710A** stores 100 messages newest first, shows
unread / addressed-to-me / bulletin symbols, Reply and New, CLR deletes (or cancels an outgoing one), an outgoing message shows
its remaining transmissions (5 sends, then marked failed), RE-TX resets the count. **TH-D74** adds Edit (quotes the original),
canned phrases while composing, and auto-reply with delay (details not verified). **Yaesu FT-5DR** holds 60 messages, separate
unread/read icons, shows remaining transmissions, TX CLR cancels, ack received vs not received are distinguished; FT2DR has 8
preset messages. **CA2RXU tracker**: read/write/delete via CardKB or phone, 500+ messages, TX buffer with retries and ack
request, LED + buzzer + OLED unread count; no reply feature mentioned. **aprs.fi** cannot send (view only); its iOS app skipped
acks for messages older than 2 h and only woke to ack after the user opened a push, so senders retried endlessly; it acks only
the exact SSID. Not verified: Xastir (auto-reply exists, Send is greyed until acked, via snippet), YAAC (IM-style chat sessions,
one open ticket), APRSIS32 (per-message ack/path settings, via snippet), UI-View32 (nothing found), bulletin and group handling in
any client except the D710 "B" marker, retry intervals of most clients.
Pain points: duplicate notifications, duplicate IDs, SSID mismatch breaking acks, late/missing acks causing endless retries,
fixed retry counts that waste airtime, tiny flat stores (60-100) without per-callsign delete.
Gaps we can fill (no verified client has them all): per-conversation threads on a handheld, adaptive backoff with a visible
countdown, reply-ack, no notification or re-display for duplicates, SSID-tolerant addressing with a recent-contacts picker,
a knob-driven quick-reply list, per-callsign and delete-all.

Design decisions taken from this:
1. Notify once per message; a retransmitted copy is acked (spec) but never notifies or re-displays.
2. Show the exact SSID in conversations and warn when a message is addressed to a different SSID of ours (spec: only the
   addressee acks); the To picker offers recent contacts and heard stations, SSID shown.
3. Resend and Abort in the context menu of any outgoing message; delivery state always visible (queued, try n of N with
   countdown to the next, acked, rejected, failed).
4. Quote-edit: Reply can start with the original quoted; canned phrases editable and on the knob list.
5. Optional auto-reply ("away") with a delay that cancels if the user operates the device first, off by default.
6. Send messages to the same addressee one at a time, each after the previous is acked (aprs.org guidance).
7. Large store on the SD card (thousands, not 60-100), unread count on the home screen, status-bar unread indicator.

## Controls: use the rotary encoder (Walter, 2026-11-22: remember the scroll knob; "knob for messaging and menus")
Rule: EVERY screen and menu must be fully usable with the knob alone (turn = move, press = select, long press = back/context
menu); the keyboard is for typing text and optional shortcuts, never required to navigate. That covers the main menu, settings,
context menus, confirm dialogs (turn to choose Yes/No, press to confirm), pickers (To field autocomplete, canned replies,
station list, region/map picker), the conversation list, threads and bulletins.
The pager's rotary encoder (A=GPIO40, B=41, press=7; Meshtastic enables internal pull-ups for it) is the main way to move:
turn = scroll lists, threads and canned replies (hold a key or modifier for page-sized jumps); press = open/select (Enter);
long press = context menu (reply, delete, detail). Keyboard is for typing and shortcuts. Same idea elsewhere: knob zooms
the map, arrows pan, press recentres. Debounce/direction need testing on the real knob.

## Map and location controls without touch (proposal, 2026-11-22; test the feel on the real knob)
- **Location in a message:** the pager detects a lat/lon or Maidenhead grid square in any received message (from any client) and
  shows a pin mark beside it in the thread. Knob highlights the message, press opens its detail, the first menu entry is
  "Show on map" (only when a location was found; the knob picks between several). The map opens on that spot with a temporary pin,
  long press returns to the thread. A keyboard key could be an optional shortcut. (To SEND a map pin use an APRS object, see
  "Meeting spot as an APRS object" below; parsing text is the fallback for pins other clients sent as plain text.)
- **Stations on the map:** press switches the knob from Zoom to Station-hop: each turn jumps to the next station, nearest first,
  with a ring and label; press opens the station card (Message, Info); long press goes back to Zoom. A filter ("heard in the last
  hour") keeps the hop list short.
- **Pan by knob alone:** the knob cycles functions: Zoom, then Pan east-west, with a press swapping to north-south. Keyboard arrows
  could also pan if the pager has them (not confirmed). Alternative to try: long press enters hop mode and a short press toggles
  zoom/pan.
- **Unread dot, but never "click the marker" (Walter 2026-11-22: visually yes, but hard to click):** a station with an unread
  message gets a coloured dot or ring on its map marker, as something to LOOK at only. Acting on it does not need aiming:
  a status-bar badge shows the unread count and a knob press on it opens the conversation list; a thread has a "Locate" action
  that centres the map on that station; station-hop orders stations unread first, then nearest first, and a long press or key
  can jump to the next unread station. Hopping also handles overlapping markers in crowded areas. Summary: the map shows who
  wrote and where; the lists and the knob do the acting. Try alternatives on the real device if one-press mode cycling annoys.
- Touch is assumed absent; the GT911 touch controller was mentioned only in an unconfirmed search snippet.

## Screens (keys are proposals)
1. **Conversations**: one row per callsign (exact SSID; option to merge SSIDs), last-message preview, time, unread badge,
   distance and bearing when the station has been heard. Enter = open, N = new message, Del = delete conversation (confirm),
   / = search.
2. **Thread**: in/out bubbles, per-message status for sent ones: queued, sent (try n of N), acked, rejected, failed; received
   ones show time, RSSI/SNR, digipeater path, RF vs internet. R = reply, Space = select (multi-select), Del = delete selected
   or the highlighted one, Enter on a message = detail.
3. **Compose**: To field with autocomplete from heard stations, contacts and recent callsigns; live counter of the 67
   characters; canned replies (editable: "QSL", "On my way", "Where are you?"); insert my position/time; estimated airtime shown.
4. **Reply in one keystroke** from the new-message notification: opens Compose already addressed to the sender, in that thread.
5. **From the map**: select a station, then Message / Info (symbol, last position, comment, path, age, distance, bearing).
6. **Bulletins** (spec "Bulletin Screen"): tab of BLN/group bulletins and announcements, sorted by sender then ID, a new line
   with the same ID replaces the old, clear one/all, a repeating alert until acknowledged, a log of old ones, a group-name
   filter list (empty = all), and sending a bulletin with a repeat schedule (a few times an hour for a few hours).
7. **Commands**: the Digi Remote function (choose a command, Send) as a screen of its own, replies land in the thread.

## Delete
- One message, a multi-selection, a whole conversation, or everything; always a confirm, plus a short undo window (about 10 s)
  before the file is rewritten.
- Deleting a conversation or message that still has an unacked outgoing message cancels its retries first (and says so).

## Send engine
- Per-destination message numbers; retry with backoff. The classic AFSK schedule (WB4APR: start at 8 s, decaying) is too
  aggressive for SF9-SF12 LoRa (my inference; no published LoRa guidance found): start around 30 s and double up to a cap
  (aprs.org message-issues.txt suggests 0, 15 s, 30 s, 1, 2, 4, 8, 16 min, then stop: take that shape but begin at 30 s), configurable, with the countdown to the next try shown; honour the 30 s duplicate window; an ack cancels retries; a rej stops them; a key stops retrying manually.
- Receive: ack every copy of a numbered message (30 s apart), show it once; never ack unnumbered, group or bulletin messages.
- Queue persists across reboots. Path per message (default WIDE1-1, option direct).
- Be polite on air: listen before talk where the radio supports it, rate limit, never ack bulletins.
- Airtime estimate (my calculation, not measured): a 60-byte packet at SF12 / 125 kHz / CR 4/5 is roughly 2.5 s on air,
  so an ack round trip is about 5 s plus waiting; this is why retries must be patient.
  Compressing messages (the LACF work) is a possible later step, but that format is still a draft in flux; nothing here depends on it.

## Compact quote by reference (proposal, 2026-11-22; our own convention, NOT an APRS standard, not agreed with anyone)
Idea (Walter): quoting should not resend the quoted bytes. Both units already store the conversation, so a reply carries only a
short reference to an earlier message (its number and direction, "yours" or "mine"), e.g. `re:12`, and the receiving pager shows
message 12 quoted from its own store. Saves roughly 20-60 bytes per quote; at SF12 about 1 symbol, roughly 33 ms, per payload byte
(my estimate from the symbol count, not measured), so a 30-character quote saves about 0.8 s of airtime, plus fewer retries.
- **Readable by others:** the token is plain text, so clients that do not understand it just show "re:12 OK on my way".
- **Capability:** use the compact form only with stations known to understand it. Our firmware sends the experimental tocall `APZPGR` (no registered one yet),
  which the station table sees in each station's beacons. Otherwise send a short text fragment of the quote, or none.
- **T-Deck:** needs extra code in Walter's fork to resolve references; not checked whether that fork stores message numbers.
  The stock firmware just shows the token.
- **Risks:** the target may have been deleted on the other side (show "quoted message not available", optionally with its first
  few characters); message numbers wrap and are unique only per sender (a 1-2 character check of the quoted text would catch a
  mismatch, costing a byte or two); the log must therefore store number and direction for every message (already planned).
- **Extension:** the same idea could make one-tap replies ("OK", "QSL") a one-byte code between our own units.
- **Order of work:** after plain messages and reply-ack work; combines well with reply-ack. Document the convention publicly
  before relying on it, and never describe it as part of APRS.

## Meeting spot as an APRS object (Walter 2026-11-22; chapter 11 of the 1.2 draft c read directly)
Spec facts: an object report is `;` + 9-character name + `*` (live) or `_` (killed) + timestamp (always) + position (full lat/long;
compressed is not recommended on RF) + symbol + comment of up to 43 characters (36 with an extension). Item reports (`)`, 3-9
character name, no timestamp) are not recommended on RF, so use objects. Any station may take over an object by sending a new
report with the same name (the original station stops when it hears that); killing it is the same report with `_`, and receivers
should stop displaying it but keep the data. Displays should show the callsign of the station that sent the object. Area objects
(circle, ellipse, box, triangle, line; 60 feet to 100 miles; colour) are possible with the `\l` symbol and a `Tyy/Cxx` extension.
Flow: map action "Mark meeting spot" (here, or at a knob-moved cursor) -> name (default `MEET-<callsign>`) + short comment (time)
-> send the object; receivers see a marker with name, comment, setter, distance and bearing. For one specific person also send an
acked message pointing at the object (object = everyone's map, acked message = that person is told). Kill it when done. Optionally
an area object for "meet within this circle". Our hop list includes objects.
Cautions: it is public (RF, and APRS-IS if an iGate forwards it): warn before sending; no authentication, anyone can overwrite it,
so names include the callsign; airtime is roughly 40-50 bytes before the AX.25 header, so about 1.5 s at SF12 (estimate); how often
to repeat an object is not in chapter 11, read the rest of the spec before choosing; display in other clients (aprs.fi, apps) is
not verified. Symbol choice for a meeting spot: check the symbol table (APRS-Symbols.pdf in the wb2osz/aprsspec repo).

## Emoticons: ASCII on the air, graphic icons on the screen (Walter 2026-11-22)
Send plain ASCII (`:)`, `:(`, `;)`, `<3` ...) so every client and radio can read it and no extra bytes are used (2-3 bytes each);
the pager draws small graphic icons in their place when displaying. The stored log and the packet always keep the ASCII; the
rendering is display-only, with a "show raw text" toggle. UTF-8 emoji are not sent (UTF-8 is allowed in message text per the 1.2
draft, but costs up to 4 bytes each, it is unknown whether 67 counts characters or bytes, and many clients cannot show them).
- **Input:** type the ASCII, or pick from an emoticon palette with the knob (the pick inserts the ASCII); the one-tap replies can
  use the same list. The compose box shows the icon live.
- **Default list (source of truth: `src/emoticons.h`, 33 entries, host-tested by `test/host/test_emoticons.cpp`):**
  smile `:)` `:-)`; sad `:(` `:-(`; wink `;)` `;-)`; grin `:D` `:-D`; laughing `XD`; tongue `:P` `:-P`; surprised `:O` `:-O`;
  neutral `:|` `:-|`; unsure `:/` `:-/`; crying `:'(` and `T_T`; kiss `:*`; heart `<3`; broken heart `</3`; thumbs up `(y)`;
  thumbs down `(n)`; wave `o/`; cheer `\o/`; cool `B)` `8-)`; angel `O:)`; angry `>:(`; unimpressed `-_-`; happy `^_^`; cute `:3`.
  All available in the knob palette; users can add their own later.
- **Matching rule (implemented and tested):** the longest list entry at a position, preceded by the start of the text or
  whitespace, followed by the end, whitespace or one of `. , ! ?`. So `10:30`, `http://x`, `(c)`, `re:)`, `3:1` and `:))` are NOT
  turned into faces. Icon bitmaps are not drawn yet (the list order is the future icon index).
- **Icons:** our own small bitmaps (about 16x16 or 20x20 RGB565 in flash, drawn inline at text height). Do not copy a vendor's
  emoji set; if an open emoji set is wanted, check its licence first (not looked up).
- **Incoming UTF-8 emoji from other clients:** show a placeholder box at first; optionally map a few very common ones to our icons
  later (font and byte-count questions unverified).
- **Screenshots/mockups:** the icons appear in the screenshots, so the screenshot tool shows exactly what users see.

## Group text: individual acked messages that look like one group message (Walter 2026-11-22)
Problem: APRS group addresses (`ALL`, `QST`, `CQ`, bulletin groups) are never acked, so the sender never knows who got it.
Design: a group is a saved list of callsigns. One typed message is sent to each member as an ordinary one-to-one message with its
own message number, one at a time; each ack comes back normally. Other clients just see a direct message from us (works with
every client and radio). UI: the group message shows "Delivered 3 of 5"; the knob expands a per-member list (queued, sent try n
of N, acked, failed); "Resend to those who missed it" retries only those; members not heard recently wait in the queue
("deliver on hearing") instead of burning airtime. Replies: v1 they arrive as normal 1:1 messages in each person's thread; later,
a reply that uses the compact `re:` reference could be filed in the group thread (needs an agreed convention and a shared member list).
A cheaper "one broadcast + a short ack from each member" variant is NOT planned: the spec says stations never ack group-addressed
messages, so it would go against the spec and other clients could not read such acks; only ever consider it as a clearly documented
extension between our own units. Soft cap on group size (about 8) with a warning, and show the estimated airtime before sending.

## Airtime by spreading factor, and settings profiles (my calculation, standard LoRa formula, NOT measured)
Assumptions: 125 kHz, coding rate 4/5, preamble 8, CRC on, low-data-rate optimisation at SF11/12, LoRa APRS 3-byte header, a
64-byte message (`K6ATV-14>APLAB1,WIDE1-1::K6ATV-3  :On my way, gate is open{12`) and a 42-byte ack, both with a WIDE1-1 path.
| SF | message | ack | per recipient | group of 5 | group of 8 |
| 7 | 0.12 s | 0.09 s | 0.21 s | 1.0 s | 1.6 s |
| 8 | 0.22 s | 0.15 s | 0.37 s | 1.8 s | 3.0 s |
| 9 | 0.39 s | 0.29 s | 0.68 s | 3.4 s | 5.4 s |
| 10 | 0.70 s | 0.53 s | 1.23 s | 6.2 s | 9.9 s |
| 11 | 1.56 s | 1.15 s | 2.71 s | 13.6 s | 21.7 s |
| 12 | 2.79 s | 2.14 s | 4.93 s | 24.7 s | 39.5 s |
Group text is quick at SF7-SF9 and slow at SF12. Costs: roughly 2.5 dB less sensitivity per step down (about 12-14 dB from SF12 to
SF7, from memory, not verified), so a faster group only works when members are fairly close; and stations on different spreading
factors cannot hear each other even on the same frequency (Walter's notes: Arizona runs SF7, Las Cruces SF12, so there is no single
standard). A receiver listens on one spreading factor, so the whole group must share one setting, and per-member speeds are not
possible. Therefore: **named settings profiles** (frequency, spreading factor, path, maybe TX power), switched with the knob, e.g.
"Standard APRS" (433.775, SF12) and "Local net" (SF9 or SF7); groups and nets are tied to a profile; the pager shows the estimated
airtime before sending ("5 recipients, about 3 s at SF9") and warns when sending a group at SF12. Anything faster than the standard
channel is a separate network that only works between people who agreed on the setting. The same profiles serve ARES nets (below).

## Radiograms and ARES/RACES (later phase; Walter sees possibilities, 2026-11-22; practice questions need his input)
Spec (1.2 draft c ch.14): an NTS radiogram travels as ordinary APRS messages, one per line, all addressed to the same station and
numbered with the normal message ID: `N#\number\precedence\handling\originator\check\place\time\date`, `NA\` address lines,
`NP\` phone, `N1\`..`N6\` text lines, `NS\` signature, `NR\` received-from trail. Each line max 67 characters including the 3-character
tag; max 6 text lines (about 380 characters of body); fields `N#`, `NA`, `NS`, `NR` required. An application need not understand them.
Possible pager features: a fill-in form driven by knob and keyboard with precedence/handling pickers, automatic word count for the
check field, receive and reassemble into one radiogram view with a missing-line warning, forward (add our own `NR\` line and pass
it on), export to SD or text over USB. A full radiogram is about 10 packets (roughly a minute at SF12 with acks, estimate; far less
at SF9); lines go out one at a time, each acked. ICS-213 is a different form that ARES/RACES use a lot, and Winlink forms are
probably the usual route: which forms matter is a question for Walter; not verified whether any APRS client implements radiograms.
An "event kit" combines pieces already in this design: objects for aid stations/checkpoints (the spec itself uses first-aid posts
and marathon checkpoints as examples), the bulletin board for net announcements, acked messages for tactical traffic, position
beacons, radiograms for formal traffic, and a settings profile per net.

## Notifications
Haptic (DRV2605), status-bar badge, backlight flash, optional sound via the audio codec later; per-contact mute and quiet hours.

## Storage
- Append-only log per callsign on the SD card (`messages/<CALL>.log`, one line per message: time, direction, number, state, text),
  index kept in RAM, flash ring buffer as fallback when there is no card. Export to plain text or CSV.
- The T-Deck firmware keeps its own `aprsMessages.txt`; we do not have to be compatible with it (maybe an importer later).

## Limits to be honest about
- The thread view shows how each message arrived (RF direct, via digi, internet); nothing promises delivery (see reachability above).
- Acks double the traffic on a shared 433.775 channel; the design keeps traffic low rather than chatty.
- Prior art: CA2RXU's tracker stores more than 500 received messages, has a TX buffer with retries and an ack request option,
  and uses a CardKB or a phone for input; its retry count/timing is undocumented. Users have reported lost acks (a missing byte
  in one fork's packet) and one client sending duplicates while showing "failed": test ack/duplicate handling carefully.
- **RULE (Walter 2026-11-22): NO ENCRYPTION, ever.** Nothing this firmware sends on RF is encrypted or obscured: no encrypted
  message content, no obfuscated compact formats meant to hide meaning, no Polaric-style encrypted messages. Everything on air stays
  plain and readable (the compact quote and emoticons are plain text conventions, documented publicly).
- Legal: US rules (47 CFR 97.113(a)(4), from the agent's background knowledge, not verified here) forbid messages encoded to
  obscure their meaning, so never encrypt message content on RF. Authentication-only codes (like the Digi Remote's keyed
  codes) are commonly considered a different thing, but Walter should confirm his own reading. Polaric's `#MAC{id` authenticated
  and AES-GCM encrypted message extensions exist; they are vendor extensions, not standards, and not planned.
- Not researched: message features of APRSdroid, Xastir, YAAC, APRSIS32, UI-View (a follow-up would fill this in).

## Order of work
Receive and show messages first (needs only the radio decode milestone), then the send engine with acks and retries (needs the
tracker milestone's licence/callsign settings), then the Conversations/Thread/Compose screens, then bulletins and commands.
