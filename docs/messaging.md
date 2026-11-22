# Messaging design (draft, 2026-11-22)

Goal: make sending, answering and cleaning up APRS messages quick on a keyboard-and-encoder device, and be
more complete than the T-Deck screen (conversation list with unread badges, bubble thread, reply button,
long-press delete of one/selected messages or a whole conversation, message detail). The pager has no
confirmed touch screen, so everything is keys + encoder.

## APRS facts to build on
Verified 2026-11-22 by an agent from APRS101 chapter 14 (read via the op-co.de mirror; aprs.org itself returned 403, so the
PDF was not read directly), aprs.org/txt/messages101.txt, aprs.org/aprs11/replyacks.txt and aprs-is.net/IGating.aspx.
- Message packet: `:ADDRESSEE:text{msgno`. Addressee is a fixed 9-character field, space padded. Text is up to 67 characters,
  printable ASCII except `|`, `~` and `{`. The message number is optional: `{` plus up to 5 alphanumerics, no spaces.
- A message without a number must NOT be acked. Ack is `ackNNN`, reject `rejNNN`, sent as message text.
- The sender repeats until acked, cancelled or timed out. The receiver acks EVERY copy of a repeated message (acks at least
  30 s apart, because digipeaters suppress duplicates for 30 s) but should show it only once.
- `ALL`, `QST`, `CQ` are group addressees: read, never acked. Queries start with `?`, have no number, are not acked.
- Bulletins: `BLN` + digit + 5 spaces (general), `BLN` + digit + group name padded to 5 (group, e.g. `BLN4WX`), `BLN` + capital
  letter (announcement). Repeated by the sender a few times an hour for a few hours (announcements far less often); a new line
  with the same ID from the same station overwrites the old one; alert on new or changed lines. NWS uses `NWS-xxxxx`.
- Reply-Ack (optional, partly supported, extent unverified): `{MM}AA` = my number MM plus a piggybacked ack for your message AA;
  the receiver still sends `ackMM}AA`, and strips `}AA` before duplicate detection. A client must tolerate `}` in numbers/acks.
- Not verified: the APRS 1.2 draft c message changes (unofficial, wb2osz/aprsspec, Nov 2024); no 2025-26 spec revision found.
  Multi-line, Unicode or longer messages: nothing found, 67 characters stands.
- No real group chat in APRS: groups are bulletin conventions or `CQ`/`ALL` addressing.

## Reaching the pager from the internet (verified rules, inferred consequence)
An iGate may gate internet traffic to RF only for stations heard locally within the last hour or less, and must not gate
generic `?` queries. Inference: the pager is reachable from APRS-IS only if a TX-capable iGate nearby heard it within the hour,
so it needs to beacon regularly, and without such an iGate it can only talk over RF. Our own iGate is RF to internet only.

## Services reachable by message (status uncertain, check before promising anything in the UI)
EMAIL-2 (email gateway; the old server retired 2019, javAPRSSrvr replaced it; wants numbered, acked messages, first word is
the address; 2026 operation not confirmed). SMSGTE (conflicting reports, status unknown). Winlink via APRSlink (not read).
CA2RXU's tracker has an "APRSThursday" menu entry. WHO-IS/INFO/position responders: not verified.

## Controls: use the rotary encoder (Walter, 2026-11-22: remember the scroll knob)
The pager's rotary encoder (A=GPIO40, B=41, press=7; Meshtastic enables internal pull-ups for it) is the main way to move:
turn = scroll lists, threads and canned replies (hold a key or modifier for page-sized jumps); press = open/select (Enter);
long press = context menu (reply, delete, detail). Keyboard is for typing and shortcuts. Same idea elsewhere: knob zooms
the map, arrows pan, press recentres. Debounce/direction need testing on the real knob.

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
6. **Bulletins**: tab for BLN/group bulletins heard, subscribe to group names, send a bulletin with a repeat schedule.
7. **Commands**: the Digi Remote function (choose a command, Send) as a screen of its own, replies land in the thread.

## Delete
- One message, a multi-selection, a whole conversation, or everything; always a confirm, plus a short undo window (about 10 s)
  before the file is rewritten.
- Deleting a conversation or message that still has an unacked outgoing message cancels its retries first (and says so).

## Send engine
- Per-destination message numbers; retry with backoff. The classic AFSK schedule (WB4APR: start at 8 s, decaying) is too
  aggressive for SF9-SF12 LoRa (my inference; no published LoRa guidance found): start around 30 s, then 60 s, 120 s, capped,
  configurable; honour the 30 s duplicate window; an ack cancels retries; a rej stops them; a key stops retrying manually.
- Receive: ack every copy of a numbered message (30 s apart), show it once; never ack unnumbered, group or bulletin messages.
- Queue persists across reboots. Path per message (default WIDE1-1, option direct).
- Be polite on air: listen before talk where the radio supports it, rate limit, never ack bulletins.
- Airtime estimate (my calculation, not measured): a 60-byte packet at SF12 / 125 kHz / CR 4/5 is roughly 2.5 s on air,
  so an ack round trip is about 5 s plus waiting; this is why retries must be patient.
  Compressing messages (the LACF work) is a possible later step, but that format is still a draft in flux; nothing here depends on it.

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
- Legal: US rules (47 CFR 97.113(a)(4), from the agent's background knowledge, not verified here) forbid messages encoded to
  obscure their meaning, so never encrypt message content on RF. Authentication-only codes (like the Digi Remote's keyed
  codes) are commonly considered a different thing, but Walter should confirm his own reading. Polaric's `#MAC{id` authenticated
  and AES-GCM encrypted message extensions exist; they are vendor extensions, not standards, and not planned.
- Not researched: message features of APRSdroid, Xastir, YAAC, APRSIS32, UI-View (a follow-up would fill this in).

## Order of work
Receive and show messages first (needs only the radio decode milestone), then the send engine with acks and retries (needs the
tracker milestone's licence/callsign settings), then the Conversations/Thread/Compose screens, then bulletins and commands.
