#pragma once
#include <Arduino.h>

// GT911 on I2C0: SDA=GPIO19, SCL=GPIO45 (matches board schematic). NOT 15/7:
// those are RGB B4/B3 and Wire.begin() on them corrupts the display.
//
// Decoupled touch path: touch_poll() reads the sensor once per main-loop
// iteration and caches a debounced press; touch_read() hands that state to
// LVGL without touching I2C, so a heavy repaint can't stall the indev.

void touch_init();
void touch_poll();
bool touch_read(int16_t &x, int16_t &y);