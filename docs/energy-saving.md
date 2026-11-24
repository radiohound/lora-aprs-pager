# Energy saving on the LilyGO T-LoRa Pager

What we measured and learned about where this device spends its battery, what we changed because of it, and what did not work.
Everything here was measured on one pager (SX1262, 433 MHz, 1500 mAh cell) with the firmware in this repository. Numbers are rough
(see "How we measured"); treat differences smaller than about 3 mA as noise.

## The short version

With the screen off the firmware used about **65 mA**, which is roughly 23 hours on the 1500 mAh cell. Most of that was not the
processor, the GPS or the radio. It was:

| What | Saves about | How |
|---|---|---|
| Processor in **light sleep** between events | 22 mA | stop the CPU; wake on radio, key, knob, GPS data or a 1 s timer |
| **SD card** unpowered *and its chip-select floated* | 14 mA | cutting the rail alone is not enough, see "The leaks" |
| **Display panel** put to sleep (not just the backlight) | 13 mA | panel sleep command |
| **GPS** unpowered *and its serial pins floated* | 28 mA | measured, but the firmware uses GPS backup mode instead (below) |
| Radio in standby instead of receiving | 6 mA | not used: receiving is the job |

The pieces add up: 47 mA (light sleep, GPS off, SD off) - 14 (SD chip-select) - 28 (GPS pins) = about 5-6 mA, which is what we
measured with the radio in standby. With the radio receiving that is about 12 mA.

What the firmware does today while the screen is off: light sleep, SD rail off with the chip-select floated, panel sleep, and the GPS
in backup mode waking once a minute for a fix. **Measured with that build: 24 mA with the screen off, on battery** (about 2 to 2.5 days
on the 1500 mAh cell, against about 23 hours before). Details at the end of this file.

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
* **Switch auto-beacon off** if you only want to listen: Backspace menu, "Auto-beacon". The pager still receives and the GPS still wakes
  once a minute while the screen is off.
* **Power off** (Backspace menu) when you will not use it for days. The pager refuses to power off while USB is connected.
* **Leave the SD card in.** Reading tiles is cheap; the card is powered down by the firmware while the screen is off.

### If you write firmware for this board

The order that mattered, biggest first:

1. **Use light sleep when idle, and do it properly.** Wake on the radio's DIO1 (high), the keyboard interrupt (low), the knob (level
   opposite to the current one) and a timer of about a second. After waking, undo the wake setup and restore each pin's edge interrupt
   type. Skip the sleep if DIO1 is already high. Never sleep with USB attached. Verify it with a counter of the time actually spent asleep.
2. **Power down only what you can power down cleanly.** When you switch a chip's supply off, also stop driving every signal line to it
   (make the pins inputs). Otherwise the chip lives off the pins and you save nothing, or lose current. Check each one with a gauge reading:
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
* **Backup mode** (`UBX-RXM-PMREQ`, backup flag, wake on UART RX edge) is deterministic. Any byte sent to the receiver wakes it. The
  firmware wakes it once a minute, waits for a new fix (at least 6 s, at most 30 s), then puts it back. While the screen is on it runs
  continuously.
* **Position and time hint at boot:** the last good fix (saved to flash after a move of 3 km or more, at most every 10 minutes) and the
  RTC time are sent as `UBX-MGA-INI-POS_LLH` and `UBX-MGA-INI-TIME_UTC`. They shorten the first fix after a cold start. The M10's backup
  supply keeps satellite data through normal operation, so this mostly helps after the pager has been fully off.
* **A restart can catch the GPS in backup.** Start-up therefore always sends the wake bytes.
* The M10 key IDs and message layouts in `src/ubx.h` are from the u-blox M10 SPG 5.10 interface description (UBX-21035062) and are
  unit-tested in `test/host/test_ubx.cpp`.
* Beacons use the most recent fix. With the GPS waking once a minute, a moving pager notices movement up to a minute late while the
  screen is off. The poll interval is the constant `GPS_POLL_MS` in `src/app/app.cpp`.

## Light sleep: things that bit us

* **`gpio_wakeup_enable()` changes a pin's interrupt type to level-triggered.** If the pin is still at its wake level when the
  processor wakes (a packet still pending on DIO1, a knob line held), its interrupt fires continuously and the interrupt watchdog
  resets the chip (`Interrupt wdt timeout on CPU1`). After waking, the firmware calls `gpio_wakeup_disable()` and restores each pin's
  edge type. Meshtastic's `sleep.cpp` handles the same trap (it masks the radio interrupt while the level wake is armed).
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
* **No setting yet** for the GPS poll interval or for turning the GPS sleep off; there are serial test commands only.
* **Beacon accuracy while the screen is off** depends on the GPS poll interval, as above.

## Reproducing and testing

USB serial commands (115200 baud). Open the port without setting DTR or RTS, which reset this chip.

| Command | Does |
|---|---|
| `P` + `S0/1` | SD rail saving off / on |
| `P` + `D0/1` | panel sleep off / on |
| `P` + `G0/1` | GPS backup cycle off / on |
| `P` + `L0/1` | light sleep off / on |
| `PW` | print the power event log (sleeps, wakes, key presses, power changes) |
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

## Result of the shipped build

**24 mA** with the screen off, USB unplugged, read from the Backspace menu's "Battery, screen off" line after the pager had been asleep with
the screen off. One run, with the GPS waking once a minute (up to 30 s per wake), the radio receiving, and nothing else changed.

Compared with 65 mA before the work, that is a reduction of about 63%. It is above the 12 mA the pieces add up to in the best case
(radio receiving, GPS off), so about 12 mA is still being spent somewhere. The most likely cause is the GPS: if it cannot get a fix
(indoors) it stays on for the full 30 s window each minute, which is about half the time at roughly 20 mA. Things to try:
* a longer `GPS_POLL_MS` (the constant in `src/app/app.cpp`),
* a shorter `GPS_ON_MAX_MS`, or giving up after a number of failed fixes,
* a setting to switch the GPS off completely while the screen is off (then pin 12 and the other GPS pins must be floated, see "The leaks"),
* measuring outdoors with a good fix, where the GPS window should be only a few seconds.
