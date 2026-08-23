#pragma once
#include <Arduino.h>

void ble_lamp_init();
void ble_lamp_set_power(bool on);
void ble_lamp_set_brightness(uint16_t bright_0_1000);
void ble_lamp_set_color_rgb(uint8_t r, uint8_t g, uint8_t b);
void ble_lamp_set_color_rgb_bright(uint8_t r, uint8_t g, uint8_t b, uint16_t bright_0_1000);
void ble_lamp_set_color_hsv(uint16_t hue, uint8_t sat, uint8_t val);
void ble_lamp_set_cct(uint16_t temp_0_1000);
void ble_lamp_set_ambient_sync(bool enabled);
bool ble_lamp_get_ambient_sync();
bool ble_lamp_get_power();
void ble_lamp_run_hue_sweep();
