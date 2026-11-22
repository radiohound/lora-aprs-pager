// LilyGO T-LoRa Pager pin map.
// Source: LilyGoLib variants/lilygo_tlora_pager/pins_arduino.h and Meshtastic
// variants/esp32s3/tlora-pager/variant.h (both agree; read 2026-11-22).
#pragma once

#define PIN_SDA 3
#define PIN_SCL 2

// One SPI bus shared by display, SD, LoRa, NFC. Each has its own CS.
#define PIN_SCK  35
#define PIN_MISO 33
#define PIN_MOSI 34

#define PIN_DISP_CS 38
#define PIN_DISP_DC 37
#define PIN_DISP_BL 42   // AW9364 backlight driver (16 steps by pulsing)
#define PIN_SD_CS   21
#define PIN_NFC_CS  39

#define PIN_LORA_CS   36
#define PIN_LORA_RST  47
#define PIN_LORA_IRQ  14   // DIO1
#define PIN_LORA_BUSY 48

#define PIN_GPS_RX 4       // ESP32 receives from the GPS on this pin
#define PIN_GPS_TX 12
#define PIN_GPS_PPS 13
#define GPS_BAUD 38400

#define PIN_KB_INT 6
#define PIN_KB_BL  46
#define PIN_ROT_A 40
#define PIN_ROT_B 41
#define PIN_ROT_PRESS 7

// XL9555 I/O expander (I2C 0x20) bit numbers. Port 0 = bits 0-7, port 1 = 8-15.
#define XP_DRV_EN  0
#define XP_AMP_EN  1
#define XP_KB_RST  2
#define XP_LORA_EN 3
#define XP_GPS_EN  4
#define XP_NFC_EN  5
#define XP_GPS_RST 7
#define XP_KB_EN   8
#define XP_GPIO_EN 9
#define XP_SD_DET  10
#define XP_SD_PULLEN 11
#define XP_SD_EN   12

// I2C addresses
#define ADDR_XL9555 0x20
#define ADDR_TCA8418 0x34   // keyboard
#define ADDR_BQ25896 0x6B   // charger
#define ADDR_BQ27220 0x55   // fuel gauge
#define ADDR_DRV2605 0x5A   // haptic
#define ADDR_ES8311  0x18   // audio codec
#define ADDR_PCF85063 0x51  // RTC
