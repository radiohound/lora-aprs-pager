// T-Deck ST7789 320x240 via LovyanGFX (pins from LilyGoLib variants/lilygo_tdeck/pins_arduino.h).
// Only for benchmarking on a T-Deck; the SPI bus is shared with SD (CS39) and LoRa (CS9).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
class TDeckDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  TDeckDisplay() {
    { auto c = _bus.config(); c.spi_host = SPI2_HOST; c.spi_mode = 0; c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 40; c.pin_mosi = 41; c.pin_miso = 38; c.pin_dc = 11; _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config(); c.pin_cs = 12; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 320; c.offset_x = 0; c.offset_y = 0;
      c.readable = false; c.invert = true; c.rgb_order = false; c.dlen_16bit = false; c.bus_shared = true;
      _panel.config(c); }
    { auto c = _light.config(); c.pin_bl = 42; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
