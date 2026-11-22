# Messaging design (draft, 2026-11-22)

Goal: make sending, answering and cleaning up APRS messages quick on a keyboard-and-encoder device, and be
more complete than the T-Deck screen (conversation list with unread badges, bubble thread, reply button,
long-press delete of one/selected messages or a whole conversation, message detail). The pager has no
confirmed touch screen, so everything is keys + encoder.

## APRS facts to build on (APRS101 ch.14; re-read the spec before coding, nothing here is coded yet)
- Message packet: `:ADDRESSEE:text{msgno` - addressee padded to 9 characters, text up to 67 characters, message number up to 5 characters.
- Ack: `:CALL-SS  :ackNNNNN`; reject: `:CALL-SS  :rejNNNNN`. A message without a number needs no ack.
- Duplicate (same sender + number) must be acked again but shown only once.
- Bulletins/announcements are `BLN0`-`BLN9` / `BLNA`-`BLNZ` (optional group name after the digit), no acks, repeated by the sender.
- Reply-ack (`{MM}AA`, APRS 1.2 addendum): optional, to be verified before use.
- There is no real group chat in APRS: groups are bulletin conventions or `CQ`/`ALL` addressing.

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
- Per-destination message numbers; retry with backoff (LoRa at SF12 is slow, start around 30 s, then 60 s, 120 s;
  configurable, maximum tries configurable); an ack cancels retries; a rej stops them; a key stops retrying manually.
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
- Messages from internet users reach a handheld on RF only if some iGate gates internet-to-RF messages; our own iGate is RF to
  internet only. The thread view therefore shows how each message arrived (RF direct, via digi, internet) and nothing promises delivery.
- Acks double the traffic on a shared 433.775 channel; the design keeps traffic low rather than chatty.
- Unverified: text length and field widths above come from my memory of APRS101 and must be checked against the spec text.

## Order of work
Receive and show messages first (needs only the radio decode milestone), then the send engine with acks and retries (needs the
tracker milestone's licence/callsign settings), then the Conversations/Thread/Compose screens, then bulletins and commands.
