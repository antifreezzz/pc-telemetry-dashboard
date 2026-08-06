#include "panel.h"
#include <Arduino.h>

// Verified pin mapping for GUITION ESP32-4848S040 (ESP32-S3 + 480x480 RGB ST7701)
// Source: github.com/aquaElectronics/esp32-4848s040-st7701 (PlatformIO reference).
// All other changes (touch.h, platformio.ini USB-CDC, LVGL) are layered on top of this.

Arduino_SWSPI *gfx_bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED,  // dc (no DC — see comment on 3-wire SPI below)
    39,                // cs  = GPIO39
    48,                // sck = GPIO48
    47,                // mosi= GPIO47
    GFX_NOT_DEFINED);  // miso

Arduino_ESP32RGBPanel *gfx_rgbpanel = new Arduino_ESP32RGBPanel(
    18, 17, 16, 21,                // de, vsync, hsync, pclk
    11, 12, 13, 14, 0,             // R0..R4
    8, 20, 3, 46, 9, 10,           // G0..G5
    4, 5, 6, 7, 15,                // B0..B4
    1, 10, 8, 50,                  // hsync: active_low, front_porch=10, pulse=8, back_porch=50
    1, 10, 8, 20,                  // vsync: active_low, front_porch=10, pulse=8, back_porch=20
    0,                             // pclk_active_neg = 0
    12 * 1000 * 1000);             // prefer_speed = 12 MHz

// ST7701 init sequence for GUITION ESP32-4848S040
// (verbatim from aquaElectronics reference).
static const uint8_t st7701_4848s040c_init[] = {
    BEGIN_WRITE,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8, 0xCD, 0x00,

    WRITE_COMMAND_8, 0xB0,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E,
    0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12,
    0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1,
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E,
    0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11,
    0x11, 0xA9, 0x32, 0x18,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,
    WRITE_C8_D8, 0xB1, 0x32,
    WRITE_C8_D8, 0xB2, 0x07,
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,

    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x1B, 0x02,

    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x08, 0xA0, 0x00, 0x00,
    0x07, 0xA0, 0x00, 0x00,
    0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x11, 0x11, 0x44, 0x44,
    0xED, 0xA0, 0x00, 0x00,
    0xEC, 0xA0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x0A, 0xE9, 0xD8, 0xA0,
    0x0C, 0xEB, 0xD8, 0xA0,
    0x0E, 0xED, 0xD8, 0xA0,
    0x10, 0xEF, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x09, 0xE8, 0xD8, 0xA0,
    0x0B, 0xEA, 0xD8, 0xA0,
    0x0D, 0xEC, 0xD8, 0xA0,
    0x0F, 0xEE, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7,
    0x02, 0x00, 0xE4, 0xE4,
    0x88, 0x00, 0x40,

    WRITE_C8_D16, 0xEC, 0x3C, 0x00,

    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0xAB, 0x89, 0x76, 0x54,
    0x02, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x20,
    0x45, 0x67, 0x98, 0xBA,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    WRITE_C8_D8, 0x3A, 0x60,  // RGB666 (compatible with RGB565 input)

    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,  // sleep out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,  // display on
    END_WRITE};

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480, 480, gfx_rgbpanel, 0, true,
    gfx_bus, GFX_NOT_DEFINED,
    st7701_4848s040c_init, sizeof(st7701_4848s040c_init));

void panel_init()
{
    gfx->begin(16 * 1000 * 1000);
    gfx->fillScreen(BLACK);

    ledcSetup(0, 600, 8);
    ledcAttachPin(GFX_BL, 0);
    ledcWrite(0, 150);
}
