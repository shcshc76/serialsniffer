#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

struct LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

  LGFX() {
    { // SPI-Bus konfigurieren
      auto cfg = _bus.config();
      cfg.spi_host   = SPI3_HOST;   // HSPI auf ESP32-S3
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.dma_channel= 1;

      // Pins (Beispiel, anpassen falls nötig)
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 2;
      cfg.pin_dc   = 12;

      // KEIN cfg.bus_shared in älteren Versionen!
      //cfg.bus_shared = true;        // wichtig für SD
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // Panel konfigurieren
      auto pcfg = _panel.config();
      pcfg.pin_cs   = 13;
      pcfg.pin_rst  = 9;
      pcfg.pin_busy = -1;

      pcfg.panel_width  = 240;
      pcfg.panel_height = 280;   // 320 falls dein Modul größer ist
      pcfg.offset_x     = 0;
      pcfg.offset_y     = 20;
      pcfg.offset_rotation = 0;

      pcfg.readable   = true;
      pcfg.invert     = true;   // bei falschen Farben auf true setzen
      pcfg.rgb_order  = false;

      _panel.config(pcfg);
    }
    { // Backlight (optional)
      auto lcfg = _light.config();
      lcfg.pin_bl = 3;
      lcfg.freq   = 12000;
      lcfg.pwm_channel = 7;
      _light.config(lcfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
