#pragma once
#include <Arduino_GFX_Library.h>

#define GFX_BL 38

extern Arduino_SWSPI *gfx_bus;
extern Arduino_ESP32RGBPanel *gfx_rgbpanel;
extern Arduino_RGB_Display *gfx;

void panel_init();
