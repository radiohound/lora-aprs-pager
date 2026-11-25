# Low power on the LilyGO T-LoRa Pager

What we measured and learned about where this device spends its battery, what we changed because of it, and what did not work.
Everything here was measured on one pager (SX1262, 433 MHz, 1500 mAh cell) with the firmware in this repository. Numbers are rough
(see "How we measured"); treat differences smaller than about 3 mA as noise.

## The short version

**Now:** with the screen off, the latest builds use about **20 to 21 mA** (20 minute runs with the pager still).
**Before any power-saving changes** they used about **65 mA**, which is roughly 23 hours on the 1500 mAh cell. That is about two thirds less
(approximate: the conditions were not identical). **We are still looking into ways to reduce power**, starting with why the later builds draw
more than an earlier 14 mA run. Estimated battery life for different kinds of use is below.

These are the main things that were drawing power in that 65 mA starting point, and what stopping each one saved in our experiments. Much of
it was not the processor, the GPS or the radio doing their jobs:

| What | Saves about | How |
|---|---|---|
| Processor in **light sleep** between events | 22 mA | stop the CPU; wake on radio, key, knob, the motion sensor or a 1 s timer (not on GPS data, see below) |
| **SD card** chip-select floated while its supply is off | 14 mA *against the leaking state* | not a saving against a card left powered: see "Open: the later builds draw more" |
| **Display panel** put to sleep (not just the backlight) | 13 mA | panel sleep command |
| **GPS** unpowered *and its serial pins floated* | 28 mA | measured, but the firmware uses GPS backup mode instead (below) |
| Radio in standby instead of receiving | 6 mA | not used: receiving is the job |

The pieces add up: 47 mA (light sleep, GPS off, SD off) - 14 (SD chip-select) - 28 (GPS pins) = about 5-6 mA, which is what we
measured with the radio in standby. With the radio receiving that is about 12 mA.

What the firmware does today while the screen is off: light sleep, SD rail off with the chip-select floated, panel sleep, and the GPS
in backup mode, waking for a fix on an adaptive interval (a minute at first, then 5 minutes while the pager sits still).
An earlier build (2026-11-24) measured **14 mA** with the pager still (the first one without the adaptive GPS read 24 mA). The latest builds
read about 20 to 21 mA, and why is not yet known: see "Open: the later builds draw more" near the end.

### Estimated battery life (1500 mAh)

Estimates for planning, **not measured battery life**. Each day is 24 hours with auto-beacon on; outside the 8 hours named, the pager is still
with the screen off.

| Use | Current assumed in those 8 hours | Days at full rated capacity | Days if 85% of it is usable |
|---|---|---|---|
| Standing still, screen off all day | 21 mA (measured in 20 minute runs) | about 3.0 | about 2.5 |
| Moving 8 hours a day, screen off | 28 mA (estimate) | about 2.7 | about 2.3 |
| Standing still, screen on 8 hours a day | 108 mA (measured on an earlier build) | about 1.2 | about 1.1 |
| Moving 8 hours a day, screen on for those hours | 112 mA (estimate) | about 1.2 | about 1.0 |

For comparison, 65 mA with the screen off all day was about 23 hours, at full rated capacity.

How the moving figures were estimated: the "moving, screen off" case is driving, with a beacon about every 68 s. It takes about 19 mA outside
GPS searches, adds about 5 mA for searches (their lengths in a drive log: about 13% of the time, at about 35 mA extra) and about 5 mA for
transmitting (2.6 s each time, assumed about 120 mA extra, not measured). Walking beacons less often. With the screen on the GPS runs all the
time and 4 mA is added for the beacons. The screen-on current depends on the brightness, and has not been measured on the current build. Cold,
battery age, weak GPS reception, messages and busy use will all change these figures.

## How to save energy

### If you use the pager

What follows is measured where a number is given. Where there is none, the advice is reasonable but we did not measure it.

* **Let the screen go off.** With the screen on the pager draws about 108 mA; with it off, 65 mA before this work and far less now.
  The screen timeouts are on the phone setup page (Enter key, "Pager" card): the screen dims after one minute and goes off after five by
  default. A shorter "off" time is the single cheapest saving. Any key or knob turn wakes it at once; there is no lock screen.
* **Keep USB unplugged when you want to know the real current.** On USB the charger supplies everything, so the pager does not use
  light sleep (the serial port would disappear). Battery life figures only apply on battery.
* **Turn the brightness down.** The backlight is a large part of the screen-on current. (Not measured separately.)
* **Beacon less often.** Each beacon is a transmission and wakes the GPS. The moving and standing-still intervals and the distance
  that counts as "moved" are on the setup page. Longer intervals cost less. (Not measured separately.)
* **Lower the transmit power** if your contacts hear you well. The setup page has a power setting. (Not measured separately.)
* **Switch auto-beacon off** if you only want to listen: Backspace menu, "Auto-beacon". The pager still receives, and the GPS still wakes
  on its usual schedule while the screen is off.
* **Power off** (Backspace menu) when you will not use it for days. The pager refuses to power off while USB is connected.
* **Leave the SD card in.** Reading tiles is cheap; the card is powered down by the firmware while the screen is off.

### If you write firmware for this board

The order that mattered, biggest first:

1. **Use light sleep when idle, and do it properly.** Wake on the radio's DIO1 (high), the keyboard interrupt (low), the knob (level
   opposite to the current one) and a timer of about a second. After waking, undo the wake setup and restore each pin's edge interrupt
   type. Skip the sleep if DIO1 is already high. Never sleep with USB attached. Verify it with a counter of the time actually spent asleep.
2. **Power down only what you can power down cleanly.** When you switch a chip's supply off, also stop driving every signal line to it
   (make the pins inputs). Otherwise the chip lives off the pins and you save nothing, or lose current. Then check the chips that share
   those lines: on this board the radio needs the SD card's chip-select (see "The SD card also stops the radio"). Check each one with a gauge reading:
   cutting the SD rail alone saved nothing, cutting the radio rail made things 18 mA worse.
3. **Put the display panel itself to sleep**, not just the backlight (about 13 mA).
4. **Do not stream the GPS to a sleeping processor.** The continuous serial data keeps waking it. Put the GPS in backup mode (it stays
   powered and hot-starts) and wake it on a schedule. Cyclic tracking is unreliable without a good sky view.
5. **Drive the pins you depend on.** `GPS_RST` (expander pin 7) must be set high or the GPS never starts.
6. **Mind the knob on battery.** Switch its interrupts off during a transmission.
7. **Measure with the instantaneous gauge register, on battery, and log the time spent asleep.** If the processor is being woken half the
   time, no other saving will show.

Things that are not worth the effort here: cutting the NFC, amplifier, haptic or keyboard rails, and holding the backlight pins low.
They are already off, or already small.

## How we measured

All readings come from the BQ27220 fuel gauge on the pager's I2C bus (address 0x55). That is a measurement of the **battery**, so it
only means something with USB unplugged.

* **USB plugged in makes the gauge useless.** The charger feeds the system and the gauge reports the charge current (about +700 mA,
  flat, whatever the load).
* **Register 0x0C (instantaneous current)** has no averaging lag. Average it yourself over many one-second samples.
* **Register 0x14 (average current)** has a window of well over 30 seconds. Right after unplugging USB it still contains the charging
  current, which made our first results nonsense (a "nothing saved" step read 37 mA, lower than every other step). Do not use it for
  short steps.
* **Remaining capacity (0x10)** counts whole mAh. Over 30 minutes at 16 mA that is 8 mAh, an error of about +/-12%. Longer is better.
* The gauge wanders by roughly +/-3 mA between identical runs. Everything below should be read with that in mind.
* We found no current offset or overall accuracy figure in mA in the BQ27220 data sheet's specification tables, so the absolute values at
  around 20 mA are unverified. Comparing runs on the same pager is likely to be more reliable than any single value. The battery is inside the
  case with no outside connection, so checking against an external power analyser means opening the pager.
* To know whether light sleep was really sleeping, the experiment code also logged the share of each step spent inside
  `esp_light_sleep_start()`. A reading is only meaningful if that is near 100%. Many early "no saving" results turned out to be the
  processor being woken constantly (GPS streaming, a floating wake pin).

## Where the power goes: the board

The schematic is LilyGO's: `schematic/T-Lora Pager V1.0 SCH 25-06-13.pdf` in
[Xinyuan-LilyGO/LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib). It is not copied here.

* The battery feeds the BQ25896 charger, which makes the system rail `VSYS`.
* A buck converter (SY8089) makes **`VDD3V3`, always on**. It feeds the ESP32-S3 and its flash and PSRAM, the display module, the audio
  codec (ES8311), the motion sensor (BHI260AP, with a 1.8 V LDO), the RTC, the haptic driver, the expander and the fuel gauge.
* Four **switched LDOs** (RT9080, enabled from the XL9555 expander) feed only: the radio (`LORA_EN`), the GPS (`GPS_EN`), the SD card
  (`SD_EN`) and the keyboard (`KEY_EN`).
* The GPS backup supply (`V_BCKP`) is tied to the always-powered RTC supply (`VRTC`). The GPS therefore keeps its satellite data even
  with its main rail off, and restarts hot.
* The NFC chip has its own P-channel switch, driven by an NPN transistor from `RF_EN`. A pull-down keeps it off by default.

### Expander pin names, from the schematic

`src/pins.h` was taken from other firmware. The schematic agrees on every pin we use; the names worth knowing:

| Expander pin | Schematic net | Our name | Notes |
|---|---|---|---|
| P00 | `M_EN` | `XP_DRV_EN` | haptic driver enable |
| P01 | `SHUTDOWN` | `XP_AMP_EN` | speaker amplifier control, pulled down: off by default |
| P02 | `KEY_RST` | `XP_KB_RST` | |
| P03 | `LORA_EN` | `XP_LORA_EN` | radio LDO |
| P04 | `GPS_EN` | `XP_GPS_EN` | GPS LDO |
| P05 | `RF_EN` | `XP_NFC_EN` | **powers the NFC chip**, not an RF amplifier |
| P07 | `GPS_RST` | `XP_GPS_RST` | must be driven high or the GPS stays in reset |
| P10 | `KEY_EN` | `XP_KB_EN` | keyboard LDO |
| P11 | `NRF_CE` | `XP_GPIO_EN` | header pin, nothing on the board uses it |
| P12 | `SD_DET` | `XP_SD_DET` | |
| P13 | `SPI_PULLUP_EN` | `XP_SD_PULLEN` | |
| P14 | `SD_EN` | `XP_SD_EN` | SD card LDO |

Direct ESP32 pins that control power: `BL_EN` (IO42, screen backlight driver) and `LED_EN` (IO46, keyboard backlight). Neither was a
problem.

## The leaks

This was the big finding, and it is the one most likely to help other firmware on this board.

**When a chip's supply is switched off but the ESP32 keeps driving its signal pins, current flows in through the chip's protection
diodes and partly powers it.** The chip then draws current from the pin instead of from its rail.

* **SD card.** With `SD_EN` low and the chip-select (GPIO 21) driven high, the pager drew about 14 mA more than with the chip-select
  floating. Floating just that one pin took the "SD off" configuration from 47 mA to 33 mA. Cutting the SD rail *without* floating the
  chip-select saved nothing. The same applies to the first test of "panel sleep + SD off", which read no change: the panel saved about
  13 mA and the SD leak cost about 14 mA.
* **The SD card also stops the radio.** They share the SPI bus. With the card's power off and CS floating, the radio stopped answering (error -20),
  so beacons failed with the screen off. CS held high all the time works but leaks (above). The firmware drives CS high only while it talks to the
  radio and waits 2 ms before each exchange; without that wait it still failed (in `PX`: 0, 20 and 100 us failed; 500 us, 2 ms and 10 ms
  passed). Confirmed on battery on one pager: before the fix every screen-off beacon failed; with it, the scheduled beacons went out and
  stations were received with the screen off. A first comparison with the card left powered showed no clear difference in current (see "Open: the later builds draw more").
* **GPS.** With `GPS_EN` low and the ESP32 still driving the serial lines, the GPS leak was about 28 mA. Floating GPIO 4, 12 and 13
  (permanently, as plain inputs) took the configuration from 34 mA to 6 mA.
* **Radio.** Cutting `LORA_EN` made things *worse* by 18 mA (65 mA vs 47 mA), for the same reason: the SPI and control lines kept
  driving an unpowered radio. Do not cut the radio's rail without floating every line to it.
* **NFC.** The chip-select (GPIO 39) did not matter (47 mA either way).

In deep sleep the ESP32's pins are released, which is why deep sleep does not show these leaks.

## What did not help

Measured, no change beyond noise:

* Driving `XP_NFC_EN` (NFC power), `XP_AMP_EN` (amp) and `XP_DRV_EN` (haptic) low. They are already off by default.
* Keyboard rail off (`XP_KB_EN`), keyboard backlight pin (GPIO 46) held low, screen backlight pin (GPIO 42) held low through sleep.
* Cutting the GPS rail (`XP_GPS_EN`) while the processor was awake: 64 mA vs 65 mA. The ESP32 was leaking into it (see above).
* Panel sleep and SD rail together, measured as a pair in the first test (the two effects cancelled).
* CPU at 80 MHz instead of 240 MHz is worth having, but is a small part of the picture.

## GPS

* **Cyclic tracking** (`CFG-PM-OPERATEMODE` = cyclic, one fix per 10 s) works when the receiver has a good view. The pager was asleep 90%
  of the time and read 42 mA. **Indoors it cannot hold a fix** and falls back to streaming continuously: 26% asleep, 60 mA. It is
  unreliable and we do not use it.
* **Backup mode** (`UBX-RXM-PMREQ`, backup flag, wake on UART RX edge). It is woken by sending it `0x00` bytes (see "A wake-up can go
  unheard"). The firmware wakes it, waits for a new fix (at least 6 s), then puts it back. How long it may search (`src/gpsduty.h`): up to 5 minutes until the first
  fix since start-up; after that 30 s, then 90 s and 3 minutes after misses. After a miss it retries after 1 minute twice, then every 5 minutes
  (15 minutes after five misses if it has had a fix). The earlier fixed 30 s limit is the one the 14 and 24 mA figures were measured with; searching longer
  costs more, as the processor does not light-sleep meanwhile (not yet measured). While the screen is on it runs
  continuously.
* **Position and time hint at boot** (the receiver accepts both: checked with the acknowledgments of `CFG-NAVSPG-ACKAIDING`, which a
  development check in the firmware turns on, `GPS_HINT_CHECK`; how much they speed up a fix has not been measured): the last good fix (saved to flash after a move of 3 km or more, at most every 10 minutes) and the
  RTC time are sent as `UBX-MGA-INI-POS_LLH` and `UBX-MGA-INI-TIME_UTC`. They shorten the first fix after a cold start. The M10's backup
  supply keeps satellite data through normal operation, so this mostly helps after the pager has been fully off.
* **Motion wake** (setting "Wake the GPS when the pager moves"): the BHI260AP's tilt, significant-motion and step reports cut the long sleep
  short. Tested walking: the GPS woke as the pager was picked up. While the sensor is quiet, a GPS position change under 150 m counts as wander,
  not movement (an indoor position moved 40 to 48 m with the pager lying still). Driving with motion wake is not yet tested, so the 5 minute check while still stays. While walking the step reports arrived about twice
  a second, so the sensor wakes the processor from light sleep only during the long GPS sleep; otherwise the one-second timer wake reads it.
* **A wake-up can go unheard.** Waking with `0xFF` bytes (the line low for one bit time) often failed: in one run 12 of 16 searches got no
  data until the wake bytes had been sent again 2 to 12 times, and 2 got none at all. With `0x00` bytes (low for nine bit times) 7 of 7 and
  then 18 of 18 searches in the next two runs got data, one of them after one retry. The retry is kept: if nothing arrives after 1.5 s the
  firmware wakes the receiver again (reopening the serial port from the third try). `PW` shows the retries for each search.
* **A restart can catch the GPS in backup.** Start-up therefore always sends the wake bytes.
* The M10 key IDs and message layouts in `src/ubx.h` are from the u-blox M10 SPG 5.10 interface description (UBX-21035062) and are
  unit-tested in `test/host/test_ubx.cpp`.
* **The wake interval is adaptive and a setting** (phone setup, "Battery, screen off"). While the pager is moving it wakes the GPS every
  `gpsmv` seconds (default 60). Once two wake-ups in a row find it within 40 m of the previous fix (150 m while the motion sensor is running
  and has reported nothing), it backs off to `gpsst` minutes (default 5). A fix that shows movement brings the short interval back at once.
  Searches that find no fix have their own pauses (above). The logic is
  `src/gpsduty.h`, tested in `test/host/test_gpsduty.cpp`. "GPS sleeps while the screen is off" can be switched off to keep the GPS running.
* Beacons use the most recent fix, so a beacon sent while the screen is off can be a little old. Without motion wake, a move that starts
  during the long interval is noticed at the next wake (up to `gpsst` minutes; on a drive this left an 8 minute gap in the beacons).

## Radio: listening in short bursts (setting, off by default)

The SX1262 can sleep between short listening windows and wake when it hears the start of a packet (the preamble). LoRa APRS stations send an
8-symbol preamble, so the radio has to decide after seeing only 3 of those 8 symbols. RadioLib's default of 8 minimum symbols leaves no time
to sleep at all, which is why `startReceiveDutyCycleAuto()` with its defaults does nothing here. The timing is in `src/rxduty.h`
(host-tested), mirroring RadioLib's formula:

| Spreading factor (125 kHz) | Symbol | Sleeps | Awake | Asleep after the wake-up overhead | Used? |
|---|---|---|---|---|---|
| SF12 | 32.8 ms | 65.5 ms | 131 ms | about 30% | yes |
| SF11 | 16.4 ms | 32.8 ms | 65.5 ms | about 27% | yes |
| SF10 | 8.2 ms | 16.4 ms | 32.8 ms | about 21% | no, too little |
| SF9 and lower | 4.1 ms or less | 8.2 ms or less | | under 10% | no: the radio's start-up (TCXO 5 ms + 1 ms) eats it |

So it does something at SF11 and SF12 only; the firmware is fixed at SF12. Expected saving about 2 mA (30% of the 6 mA receive current).
**Not tested on air yet.** The risk is missing packets whose preamble is short or weak. Test it beside a second receiver for an hour and compare
packet counts before relying on it. Only with the screen off; with the screen on the radio listens continuously.

## Light sleep: things that bit us

* **`gpio_wakeup_enable()` changes a pin's interrupt type to level-triggered.** If the pin is still at its wake level when the
  processor wakes (a packet still pending on DIO1, a knob line held), its interrupt fires continuously and the interrupt watchdog
  resets the chip (`Interrupt wdt timeout on CPU1`). After waking, the firmware calls `gpio_wakeup_disable()` and restores each pin's
  edge type. Meshtastic's `sleep.cpp` handles the same trap (it masks the radio interrupt while the level wake is armed).
* **Do not wake on the GPS serial line while the GPS is talking.** An overnight soak test of the adaptive-GPS build restarted 26 times, all
  interrupt-watchdog resets inside `esp_light_sleep_start()`, soon after a GPS wake-up. The GPS line's level wake fired in a storm. The firmware
  now has no wake on that line and does not light-sleep while the GPS is awake (at least 6 s, and up to 5 minutes while it searches). The restart list in `PW` (reason and
  stage of each restart, kept in RTC memory) is what found this.
* **Do not sleep with a packet already latched.** If DIO1 is high before sleeping, skip the sleep and service the radio.
* **Never sleep with USB attached.** The serial port disappears. The firmware reads the charger's VBUS status and stays awake.
* **The sleep-only pad setting (`gpio_sleep_set_direction`) worked for the SD chip-select but made the processor wake constantly when
  applied to the GPS serial pins.** (Asleep fell to 50% and the current rose.) The cause was not found; leaving the GPS powered and using
  its backup mode avoids the question.
* **Knob noise during a transmission.** Reported by the Meshtastic project on this pager: on battery the encoder lines pick up
  transmitter noise, the pin-change interrupts fire thousands of times a second, and the watchdog trips. The firmware switches the knob
  interrupts off around each transmission.
* **After light sleep the display stayed dark although the pager had woken** (the haptic buzzed, the key presses registered, the SD card
  remounted). The panel's "sleep out" command, sent at the moment of waking, did not take. Sent again a minute later by hand it did, and
  the screen came back; the backlight PWM was not the problem (re-attaching it alone changed nothing). We did not pin down why. The fix that
  works: on waking, remount the SD card first, then wake the panel (sleep-out, 130 ms wait, display-on), and keep repeating the sleep-out
  every half second for the first 3 seconds (it is harmless when the panel is already awake). A full display re-initialisation was not
  needed. Earlier tests never exposed this because they either did not use light sleep or ended with an automatic restart.
* **A pager that "will not wake" on battery works fine on USB**, because USB disables light sleep. Debugging it needs the pager to stay
  unplugged. The firmware keeps a small in-memory event log (serial `PW`): every sleep stretch, wake cause, key press and power change with
  the time. Plug USB in afterwards **without resetting** and read it. The log only survives while the app keeps running.
* A key press or knob turn while the screen is off only wakes the screen; it is not acted on. There is no lock screen.

## What Meshtastic does

From their `variants/esp32s3/tlora-pager` and `src/sleep.cpp` (read 2026-11-23, nothing measured):

* `USE_POWERSAVE` with `SLEEP_TIME 120` and light sleep in their power state machine, using the same wake sources as ours.
* Their pager init leaves the GPS, keyboard, SD, header and haptic rails on and only keeps the amplifier off. They do not cut rails or
  float pins, so they very likely carry the leaks described above. We have not measured their firmware on this pager, so we make no
  claim about the difference.
* Related pager issues: [#12074](https://github.com/meshtastic/firmware/pull/12074) (encoder noise causing watchdog resets on
  battery) and [#11929](https://github.com/meshtastic/firmware/pull/11929) (the GPS reset line must be driven).

## Open questions

* **Deep sleep read 16 mA, higher than the 6 mA light-sleep result.** In a 30-minute deep-sleep test (capacity 1160 to 1152 mAh,
  gauge average 19 mA) the board used more than in the best light-sleep configuration. Possible explanations: pins left in a leaking
  state during deep sleep, or the +/-1 mAh error. Not resolved, so we do not treat 6 mA as exact.
* **Real listening configuration.** The 6 mA figure has the GPS off and the radio in standby. Receiving adds about 6 mA, and the GPS
  duty cycle adds more. The measured result of the real build is below.
* **Radio, GPS and beacons on battery** have been tested on one pager in runs of up to about an hour, including a 40 minute drive (27
  beacons sent, none failed; the 26 in the compared period were all heard). Longer runs are still to be done.
* **Restarts in light sleep.** Two causes were found and fixed: interrupt-watchdog resets from a storm of level interrupts on the wake pins
  (every wake pin's processor interrupt is now switched off before sleeping), and task-watchdog resets with the main loop stuck in the radio
  library's channel check, which waits with no time limit (the firmware now limits it, resets the radio, and backs off if it keeps failing).
  Why the channel check stuck on that occasion is not known. The runs since have had no restarts, but they were short. `PW` keeps a restart
  list in memory that survives such resets.

## Reproducing and testing

USB serial commands (115200 baud). Open the port without setting DTR or RTS, which reset this chip.

| Command | Does |
|---|---|
| `P` + `S0/1` | SD rail saving off / on |
| `PX` | development test, USB: which SD card and chip-select states stop the radio (receive only; takes a few minutes, restores afterwards) |
| `P` + `D0/1` | panel sleep off / on |
| `P` + `G0/1` | GPS backup cycle off / on |
| `P` + `L0/1` | light sleep off / on |
| `PW` | print the logs: restarts, power events, beacons, GPS searches (retries, motion, metres moved, next sleep), stuck channel checks |
| `PT` | pretend the next channel check never finishes (tests the recovery) |
| `PB0`..`PB3` | screen repairs for diagnosing a dark screen: backlight PWM, panel sleep-out, display-on, full re-init |
| `PO<seconds>` | set the screen-off time (dim comes at half of it) and save it |
| `PA<seconds>` | print the average, minimum and maximum gauge current (on USB it reads the charger) |
| `PN` | count the bytes the GPS sends, per second, for 12 s |
| `B` | dump the fuel gauge registers |

For real numbers, use the menu: with USB unplugged and the screen off for a few minutes, wake it and read the screen-off battery line
in the Backspace menu.

The experiment code that produced the tables above (a self-running test that stepped through the savings while the screen was off,
logged the average current and the share of time asleep, and a 30-minute deep-sleep capacity test) is in the git history of the
`power-saving` branch.

## Open: the later builds draw more, and it is not yet explained

Two 20 minute runs on one build, on battery, the pager still in the same place, motion wake on, 3 beacons each:

| Screen off | SD card power off (with the chip-select fix) | SD card left powered (`PS0`) |
|---|---|---|
| Average (about 600 gauge readings) | 21 mA | 21 mA |
| Share of the time searching for GPS | 6% | 4% |

Allowing for the searches (estimated at about 35 mA extra: the receiver's 12 to 15 mA from the MIA-M10Q data sheet, and about 22 mA because
the processor does not light-sleep meanwhile; an estimate, not a measurement) leaves about 19 and 20 mA. So **these runs do not show that
switching the card off saves anything**: the fix is needed to keep the radio working with the card off, but a saving is not established. Other
runs that day gave about 16 to 21 mA after the same allowance, against about 11 mA for the 14 mA run below. The cause of that difference is not
known. The 14 mA build also had the radio and GPS wake faults described above, so it is not certain that everything in it was working. Short
runs on the fuel gauge cannot resolve differences of a few mA with confidence; the next steps are to repeat the old build in the same place and
to measure with an external power analyser.

## Result of the earlier build (2026-11-24)

**14 mA** with the screen off, USB unplugged: the average of 2,524 gauge readings (one every 2 s, readings taken with USB present are
skipped), minimum -68 mA, maximum -10 mA, over about 1.4 hours with the pager still, the GPS in backup mode on a 5-minute interval (30 s
awake per wake), the radio receiving continuously, and nothing else changed. Read with the serial command `PA`. The earlier build, with the
GPS waking every minute, read 24 mA.

Compared with 65 mA before the work, that is a reduction of about 78%. The pieces add up to about 12 mA in the best case (radio
receiving, GPS off), so only about 2 mA was left to explain, thought at the time to be the GPS wakes. Estimates made then for later work (never
measured): receiving in short bursts at SF11/SF12 about 1 to 2 mA, waking the GPS on motion instead of a timer about 1.5 to 2 mA, both
together about 10 to 11.5 mA. Still to try:
* measuring the adaptive build while moving, and outdoors,
* the cost of the longer GPS searches, and of motion wake, with everything else the same,
* switching the GPS off completely while the screen is off (then pin 12 and the other GPS pins must be floated, see "The leaks"),
* once motion wake is proven in a car, a much longer GPS sleep while still.
