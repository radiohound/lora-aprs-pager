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
#define MOT_ID_TILT_WU 49      // BHY2_SENSOR_ID_TILT_DETECTOR_WU
#define MOT_ID_SIG_WU  56      // BHY2_SENSOR_ID_SIG_WU

static SensorBHI260AP motionChip;
static bool motionOk = false;
static volatile uint32_t motionTilts = 0, motionSigs = 0, motionIrqs = 0;
static volatile uint32_t motionLastMs = 0;

static void IRAM_ATTR motionIsr() { motionIrqs = motionIrqs + 1; }
static void motionOnEvent(uint8_t id, const uint8_t *, uint32_t, uint64_t *, void *) {
  if (id == MOT_ID_TILT_WU) motionTilts = motionTilts + 1; else if (id == MOT_ID_SIG_WU) motionSigs = motionSigs + 1;
  motionLastMs = millis();
}

// Load the firmware and switch the two sensors on. Returns false if the chip does not answer. Takes about a second.
static bool motionBegin() {
  motionChip.setFirmware(bosch_firmware_image, bosch_firmware_size, false);          // into RAM, not into a flash chip (there is none)
  Wire.setClock(1000000UL);                                                          // the image is large: upload at 1 MHz, then back
  motionOk = motionChip.begin(Wire, BHI260AP_SLAVE_ADDRESS_L);
  Wire.setClock(400000UL);
  if (!motionOk) { Serial.printf("motion: chip not found (%s)\n", motionChip.getError()); return false; }
  motionChip.onResultEvent(MOT_ID_TILT_WU, motionOnEvent);
  motionChip.onResultEvent(MOT_ID_SIG_WU, motionOnEvent);
  bool a = motionChip.configure(MOT_ID_TILT_WU, 1, 0), b = motionChip.configure(MOT_ID_SIG_WU, 1, 0);
  pinMode(PIN_MOTION_INT, INPUT);
  attachInterrupt(PIN_MOTION_INT, motionIsr, RISING);
  Serial.printf("motion: chip up, firmware %u bytes, tilt %s, significant motion %s\n", (unsigned)bosch_firmware_size, a ? "on" : "FAILED", b ? "on" : "FAILED");
  return true;
}

// Call from the main loop: reads the chip's event FIFO after its interrupt line went high.
static void motionPoll() {
  if (!motionOk) return;
  if (digitalRead(PIN_MOTION_INT) || motionIrqs) { motionIrqs = 0; motionChip.update(); }
}
