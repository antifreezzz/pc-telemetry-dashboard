#pragma once
#include <lvgl.h>

void ui_init(int w, int h);

void ui_set_cpu_pct(int pct);
void ui_set_gpu_pct(int pct);
void ui_set_ram_pct(int pct, int used_mb, int total_mb);
void ui_set_net(uint32_t rx_kbps, uint32_t tx_kbps);
