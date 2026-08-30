#pragma once
#include <lvgl.h>

void ui_init(int w, int h);
void ui_set_cpu_pct(int pct, const uint8_t *cores, int ncores);
void ui_set_gpu_pct(int pct, int vram_pct, uint32_t vram_used_mb, uint32_t vram_total_mb);
void ui_set_ram_pct(int pct, uint32_t used_mb, uint32_t total_mb);
void ui_set_net(uint32_t rx_kibs, uint32_t tx_kibs);
void ui_set_disk(uint32_t rd_kibs, uint32_t wr_kibs, int used_pct1, int used_pct2 = 255);
void ui_set_header(uint32_t uptime_sec, uint32_t epoch_sec, const char *hostname);
void ui_set_proc(const uint8_t *data, uint16_t len);
void ui_set_llm(uint8_t status, float tps, const char *model, uint8_t cache_hit_pct = 255, uint16_t prompt_k = 0, uint8_t flags = 0);
void ui_set_llm_models(const uint8_t *data, uint16_t len);
void ui_set_llm_profiles(const uint8_t *data, uint16_t len);
void ui_open_llm_menu();
void ui_open_screen(uint8_t screen_id);
