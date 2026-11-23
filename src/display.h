// ST7796 480x222 panel via LovyanGFX. Settings copied from Meshtastic's pager variant
// (panel 222x480, offset_x 49, SPI2 at 75 MHz write / 16 MHz read, no RESET pin, BL on GPIO42).
// The rotation/offset_rotation combination is UNVERIFIED on real hardware: check the
// test screen's corner labels and adjust PAGER_ROTATION / offset_rotation if it is mirrored.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pins.h"

#ifndef PAGER_ROTATION
#define PAGER_ROTATION 0   // landscape 480x222 (with offset_rotation 3: 0 and 2 are the landscape ones; 0 tried first)
#endif

class PagerDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  PagerDisplay() {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 75000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_SCK; c.pin_mosi = PIN_MOSI; c.pin_miso = PIN_MISO; c.pin_dc = PIN_DISP_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = PIN_DISP_CS; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 222; c.panel_height = 480;
      c.offset_x = 49; c.offset_y = 0; c.offset_rotation = 3;
      c.readable = true; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = true;   // shared with SD / LoRa / NFC
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = PIN_DISP_BL; c.invert = false; c.freq = 12000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
