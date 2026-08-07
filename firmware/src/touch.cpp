#include "touch.h"
#include <Wire.h>
#include <Touch_GT911.h>

#define TP_SDA 19
#define TP_SCL 45
#define TP_ADDR 0x5D
#define TP_W 480
#define TP_H 480

// Sole instance: this TU is the only one holding touch state. (Declared non-
// static on purpose - a `static` in the header would give main.cpp and ui.cpp
// separate copies and break the press handoff.)
static Touch_GT911 ts = Touch_GT911(TP_SDA, TP_SCL, -1, -1, TP_W, TP_H);
static bool ts_pressed = false;
static int16_t ts_x = 0, ts_y = 0;
static uint8_t ts_missing = 0;

void touch_init()
{
    Wire.begin(TP_SDA, TP_SCL);
    ts.begin(TP_ADDR);
    // Raw GT911 already reports in screen orientation. ROTATION_INVERTED is a
    // passthrough (x=x, y=y); ROTATION_NORMAL mirrors 180 deg -> taps land on
    // the opposite corner.
    ts.setRotation(ROTATION_INVERTED);
    ts_pressed = false;
    ts_missing = 0;
}

void touch_poll()
{
    ts.read();
    if (ts.isTouched) {
        ts_pressed = true;
        ts_missing = 0;
        ts_x = ts.points[0].x;
        ts_y = ts.points[0].y;
    } else if (ts_pressed) {
        // absorb single-frame sensor drops so a held press survives a flicker
        if (++ts_missing >= 3) {
            ts_pressed = false;
            ts_missing = 0;
        }
    }
}

bool touch_read(int16_t &x, int16_t &y)
{
    if (ts_pressed) {
        x = ts_x;
        y = ts_y;
        return true;
    }
    return false;
}