// Motion sensor (Bosch BHI260AP, I2C 0x28, interrupt on IO8): the idea is to wake the GPS when the pager moves, instead of on a timer.
// The BHI260AP has no firmware of its own on this board: we load Bosch's image into its RAM at every start (about 130 kB, from SensorLib).
// Virtual sensors used: Tilt detector and Significant motion, both in their wake-up form (they raise the interrupt line while the host sleeps).
// Datasheet BST-BHI260AP-DS000 table 14: about 37 uA each. Any-motion exists only on the BHI360, not on this chip.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#define BOSCH_APP30_SHUTTLE_BHI260_FW
#include <SensorBHI260AP.hpp>
#include <BoschFirmware.h>
#include "../pins.h"

#define PIN_MOTION_INT 8
// Which of these catches walking and driving is not known yet; the counts are logged so a test can tell.
// Sensor numbers as in SensorLib's BoschSensorID.hpp. Which ones this firmware image offers is only known by trying: PW shows it.
#define MOT_KINDS 5
static const uint8_t MOT_ID[MOT_KINDS] = {48,        55,           94,     77,              143};   // tilt, significant motion, step (wake-up),
static const char *const MOT_NAME[MOT_KINDS] = {"tilt", "significant", "steps", "motion detect", "any motion"};   // motion detect, any motion (low power, wake-up)
#define MOT_SIG 1                                        // significant motion reports once, then has to be switched on again

static SensorBHI260AP motionChip;
static bool motionOk = false;
static uint32_t motionCount[MOT_KINDS] = {0};
static bool motionAvail[MOT_KINDS] = {false};
static uint8_t motionNew = 0;                            // bit per kind: reported since the main loop last looked
static bool motionRearmSig = false;

static void motionOnEvent(uint8_t id, const uint8_t *, uint32_t, uint64_t *, void *) {
  int k = -1; for (int i = 0; i < MOT_KINDS; i++) if (MOT_ID[i] == id) k = i;
  if (k < 0) return;
  motionCount[k]++; motionNew |= 1 << k;
  if (k == MOT_SIG) motionRearmSig = true;
}

// Load the firmware and switch the sensors on. Returns false if the chip does not answer. Takes about a second.
// The interrupt line (IO8) gets no processor interrupt: it is only read, and used as a level wake from light sleep.
static bool motionBegin() {
  motionChip.setFirmware(bosch_firmware_image, bosch_firmware_size, false);          // into RAM, not into a flash chip (there is none)
  Wire.setClock(1000000UL);                                                          // the image is large: upload at 1 MHz, then back
  motionOk = motionChip.begin(Wire, BHI260AP_SLAVE_ADDRESS_L);
  Wire.setClock(400000UL);
  if (!motionOk) { Serial.printf("motion: chip not found (%s)\n", motionChip.getError()); return false; }
  for (int k = 0; k < MOT_KINDS; k++) { motionChip.onResultEvent(MOT_ID[k], motionOnEvent); motionAvail[k] = motionChip.configure(MOT_ID[k], 1, 0); }
  pinMode(PIN_MOTION_INT, INPUT);
  Serial.print("motion: chip up;"); for (int k = 0; k < MOT_KINDS; k++) Serial.printf(" %s %s;", MOT_NAME[k], motionAvail[k] ? "on" : "not available"); Serial.println();
  return true;
}

// Call from the main loop. Reads the chip's events when its line is high, and once a second anyway (a short pulse could be missed).
static void motionPoll() {
  if (!motionOk) return;
  static uint32_t at = 0;
  if (!digitalRead(PIN_MOTION_INT) && millis() - at < 1000) return;
  at = millis();
  motionChip.update();
  if (motionRearmSig) { motionRearmSig = false; motionChip.configure(MOT_ID[MOT_SIG], 1, 0); }
}
