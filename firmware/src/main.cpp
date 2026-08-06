#include <Arduino.h>
#include "panel.h"
#include "touch.h"
#include "ui.h"
#include "protocol.h"

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("ESP32-4848S040C dashboard");
    Serial.printf("PSRAM: %u bytes, FreeHeap: %u\n",
                  ESP.getPsramSize(), ESP.getFreeHeap());

    panel_init();
    Serial.println("Panel init done");

    uint16_t *fb = gfx->getFramebuffer();
    Serial.printf("Framebuffer: %p\n", fb);
    if (fb) {
        for (int i = 0; i < 480 * 480; i++) fb[i] = 0x0000;
        Serial.println("Framebuffer cleared to black");
    }

    touch_init();
    Serial.println("Touch init done");

    ui_init(480, 480);
    Serial.println("UI init done");

    protocol_init();
    Serial.println("Protocol init done");
}

void loop()
{
    int16_t x, y;
    if (touch_read(x, y)) {
        Serial.printf("touch %d %d\n", x, y);
    }
    protocol_poll();
    lv_timer_handler();
    delay(5);
}
