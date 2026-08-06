#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Touch_GT911.h>

// GT911 on I2C0: SDA=GPIO19, SCL=GPIO45 (matches the board schematic; see
// esphome devices DB and ha5dzs/Guition-ESP32-4848S040). Do NOT use 15/7:
// those are RGB B4/B3 and Wire.begin() on them corrupts the display.
#define TP_SDA 19
#define TP_SCL 45
#define TP_ADDR 0x5D
#define TP_W 480
#define TP_H 480

static Touch_GT911 ts = Touch_GT911(TP_SDA, TP_SCL, -1, -1, TP_W, TP_H);

inline void touch_init()
{
    Wire.begin(TP_SDA, TP_SCL);
    ts.begin(TP_ADDR);
    ts.setRotation(ROTATION_NORMAL);
}

inline bool touch_read(int16_t &x, int16_t &y)
{
    ts.read();
    if (ts.isTouched)
    {
        x = ts.points[0].x;
        y = ts.points[0].y;
        return true;
    }
    return false;
}
