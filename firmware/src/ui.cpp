#include "ui.h"
#include "panel.h"
#include "protocol.h"
#include "touch.h"
#include "ble_lamp.h"
#include <Arduino.h>
#include <string.h>

// ---------- display ----------
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *buf1 = NULL;

// ---------- screen power (backlight off, tap-to-wake) ----------
static bool g_screen_off = false;
static uint8_t g_brightness = 150;  // must match panel default and slider defaults
static bool g_suppress_until_release = false;

#define SCREEN_OFF_COOLDOWN_MS 500
static uint32_t g_screen_off_ms = 0;
static bool g_released_since_off = true;

static void backlight_set(uint8_t v)
{
    g_brightness = v;
    ledcWrite(0, v);
}

static void screen_off()
{
    g_screen_off = true;
    g_screen_off_ms = millis();
    g_released_since_off = false;
    ledcWrite(0, 0);
}

static void screen_wake()
{
    g_screen_off = false;
    ledcWrite(0, g_brightness);
}

// ---------- Clean Modern Dark Palette ----------
static const lv_color_t C_BG        = lv_color_hex(0x090b10); // Deep Obsidian
static const lv_color_t C_CARD      = lv_color_hex(0x131722); // Modern Dark Slate
static const lv_color_t C_CARD_HOV  = lv_color_hex(0x1c2333); // Active / Pressed Slate
static const lv_color_t C_BORDER    = lv_color_hex(0x212836); // Subtle crisp border
static const lv_color_t C_ARC_BG    = lv_color_hex(0x1a212d); // Track background
static const lv_color_t C_CYAN      = lv_color_hex(0x38bdf8); // Sky Blue (CPU / RX)
static const lv_color_t C_PURPLE    = lv_color_hex(0xa78bfa); // Lavender / Purple (GPU)
static const lv_color_t C_GREEN     = lv_color_hex(0x34d399); // Emerald Mint (RAM / TX / Active)
static const lv_color_t C_AMBER     = lv_color_hex(0xfbbf24); // Warm Gold (Disk / Lamp / Warnings)
static const lv_color_t C_RED       = lv_color_hex(0xef4444); // Crimson (Stop / Off / Alert)
static const lv_color_t C_TEXT      = lv_color_hex(0xf8fafc); // Crisp white text
static const lv_color_t C_MUTED     = lv_color_hex(0x94a3b8); // Slate 400
static const lv_color_t C_DIM       = lv_color_hex(0x475569); // Slate 600
static const lv_color_t C_GRAY      = lv_color_hex(0x64748b); // Slate 500

// ---------- FPS accounting ----------
static volatile uint32_t g_flush_cnt = 0;

// ---------- header ----------
static lv_obj_t *lbl_uptime;
static lv_obj_t *lbl_clock;
static lv_obj_t *btn_proc;
static lv_obj_t *btn_lamp_hdr;

static bool g_hdr_init = false;
static uint32_t g_boot_ms = 0;
static uint32_t g_uptime_base = 0;
static uint32_t g_wallclock_epoch = 0;

// ---------- CPU ----------
static lv_obj_t *arc_cpu;
static lv_obj_t *lbl_cpu_pct;
static lv_obj_t *cores_cont;
static lv_obj_t *bar_cores[32];
static uint8_t g_cores[32];
static int g_ncores = 0;

// ---------- GPU ----------
static lv_obj_t *arc_gpu;
static lv_obj_t *lbl_gpu_pct;
static lv_obj_t *bar_vram;
static lv_obj_t *lbl_vram;

// ---------- RAM ----------
static lv_obj_t *bar_ram;
static lv_obj_t *lbl_ram;
static lv_obj_t *lbl_ram_pct;

// ---------- NET ----------
static lv_obj_t *chart_net;
static lv_chart_series_t *ser_rx;
static lv_chart_series_t *ser_tx;
static lv_obj_t *lbl_rx;
static lv_obj_t *lbl_tx;
static uint32_t g_net_max_seen = 1024;

// ---------- DISK (No yellow bar, 2 disk percentages + large rates) ----------
static lv_obj_t *lbl_disk_rd;
static lv_obj_t *lbl_disk_wr;
static lv_obj_t *lbl_disk_p1;
static lv_obj_t *lbl_disk_p2;

// ---------- PROC overlay ----------
#define PROC_KINDS 5
#define PROC_ROWS 10
#define PROC_ENTRY_BYTES 22

static lv_obj_t *ovl_proc;
static lv_obj_t *lbl_ovl_title;
static lv_obj_t *proc_rows[PROC_ROWS];
static uint32_t g_proc_val[PROC_KINDS][PROC_ROWS];
static uint16_t g_proc_pid[PROC_KINDS][PROC_ROWS];
static char g_proc_name[PROC_KINDS][PROC_ROWS][17];
static int g_proc_n[PROC_KINDS];

// views: what the overlay displays
enum { VIEW_CPU = 0, VIEW_RAM, VIEW_GPU, VIEW_DISK };
static int g_active_kind = VIEW_CPU;

// cards
static lv_obj_t *card_cpu;
static lv_obj_t *card_gpu;
static lv_obj_t *card_ram;
static lv_obj_t *card_disk;
static lv_obj_t *card_llm;
static lv_obj_t *lbl_llm;
static lv_obj_t *lbl_llm_badge;
static uint8_t g_llm_status = 255;
static float g_llm_tps = 0.0f;
static char g_llm_model[25] = {0};
static uint8_t g_llm_cache_hit_pct = 255;
static uint16_t g_llm_prompt_k = 0;
static uint8_t g_llm_flags = 0;

// ---------- latest load values (for ambient lamp sync) ----------
static int g_cpu_pct = 0;
static int g_gpu_pct = 255;
static int g_ram_pct = 0;
#define AMBIENT_PEAK_PCT 80

// ---------- LLM models list & overlay ----------
struct LlmModelItem {
    char id[15];
    uint8_t is_fav;
    uint8_t status;
};
static LlmModelItem g_llm_models[16];
static int g_llm_models_count = 0;
static int g_llm_page = 0;
static int g_selected_model_idx = -1;

#define LLM_ITEMS_PER_PAGE 6

static lv_obj_t *ovl_llm;
static lv_obj_t *card_llm_banner;
static lv_obj_t *lbl_llm_ovl_status;
static lv_obj_t *btn_llm_cards[6];
static lv_obj_t *lbl_llm_card_name[6];
static lv_obj_t *lbl_llm_card_status[6];
static lv_obj_t *lbl_llm_card_hint[6];
static lv_obj_t *btn_page_prev;
static lv_obj_t *btn_page_next;
static lv_obj_t *lbl_page_num;

// Profile picker modal
static lv_obj_t *ovl_llm_profiles;
static lv_obj_t *lbl_prof_model_title;
static lv_obj_t *btn_prof_stop;
static lv_obj_t *lbl_prof_subtitle;
static lv_obj_t *btn_profiles[6];
static lv_obj_t *lbl_profiles_title[6];
static lv_obj_t *lbl_profiles_desc[6];

// ---------- LAMP Modal (ovl_lamp) ----------
static lv_obj_t *ovl_lamp;
static lv_obj_t *btn_lamp_power;
static lv_obj_t *lbl_lamp_power;
static lv_obj_t *lbl_lamp_bright_val;
static lv_obj_t *slider_lamp_bright;
static lv_obj_t *lbl_lamp_cct_val;
static lv_obj_t *slider_lamp_cct;
static lv_obj_t *sw_ambient_sync;
static bool g_lamp_ui_power = true;

static lv_style_t style_card;
static lv_style_t style_arc_bg;
static lv_style_t style_arc_indic;
static lv_style_t style_arc_gpu_indic;
static lv_style_t style_caption;
static lv_style_t style_btn;
static lv_style_t style_btn_pr;
static lv_style_t style_btn_red;
static lv_style_t style_btn_green;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
    int w = a->x2 - a->x1 + 1;
    int h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&c->full, w, h);
    lv_disp_flush_ready(d);
    g_flush_cnt++;
}

static void style_init()
{
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, C_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 14);
    lv_style_set_pad_all(&style_card, 10);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, C_BORDER);
    lv_style_set_shadow_width(&style_card, 0);
    lv_style_set_shadow_spread(&style_card, 0);
    lv_style_set_shadow_opa(&style_card, LV_OPA_TRANSP);

    lv_style_init(&style_arc_bg);
    lv_style_set_arc_color(&style_arc_bg, C_ARC_BG);
    lv_style_set_arc_width(&style_arc_bg, 10);
    lv_style_set_arc_rounded(&style_arc_bg, true);

    lv_style_init(&style_arc_indic);
    lv_style_set_arc_color(&style_arc_indic, C_CYAN);
    lv_style_set_arc_width(&style_arc_indic, 10);
    lv_style_set_arc_rounded(&style_arc_indic, true);

    lv_style_init(&style_arc_gpu_indic);
    lv_style_set_arc_color(&style_arc_gpu_indic, C_PURPLE);
    lv_style_set_arc_width(&style_arc_gpu_indic, 10);
    lv_style_set_arc_rounded(&style_arc_gpu_indic, true);

    lv_style_init(&style_caption);
    lv_style_set_text_color(&style_caption, C_MUTED);
    lv_style_set_text_font(&style_caption, &lv_font_montserrat_12);

    lv_style_init(&style_btn);
    lv_style_set_bg_color(&style_btn, C_CARD);
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_btn, 10);
    lv_style_set_border_width(&style_btn, 1);
    lv_style_set_border_color(&style_btn, C_BORDER);
    lv_style_set_pad_hor(&style_btn, 10);
    lv_style_set_shadow_width(&style_btn, 0);
    lv_style_set_shadow_spread(&style_btn, 0);
    lv_style_set_shadow_opa(&style_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_btn, C_MUTED);

    lv_style_init(&style_btn_pr);
    lv_style_set_bg_color(&style_btn_pr, C_CARD_HOV);
    lv_style_set_border_color(&style_btn_pr, C_CYAN);
    lv_style_set_border_width(&style_btn_pr, 1);
    lv_style_set_shadow_width(&style_btn_pr, 0);
    lv_style_set_shadow_spread(&style_btn_pr, 0);
    lv_style_set_shadow_opa(&style_btn_pr, LV_OPA_TRANSP);
    lv_style_set_translate_y(&style_btn_pr, 0); // Flat, no 3D offset
    lv_style_set_text_color(&style_btn_pr, C_TEXT);

    lv_style_init(&style_btn_red);
    lv_style_set_bg_color(&style_btn_red, lv_color_hex(0x281116));
    lv_style_set_bg_opa(&style_btn_red, LV_OPA_COVER);
    lv_style_set_border_color(&style_btn_red, C_RED);
    lv_style_set_border_width(&style_btn_red, 1);
    lv_style_set_radius(&style_btn_red, 10);
    lv_style_set_shadow_width(&style_btn_red, 0);
    lv_style_set_shadow_spread(&style_btn_red, 0);
    lv_style_set_shadow_opa(&style_btn_red, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_btn_red, C_RED);

    lv_style_init(&style_btn_green);
    lv_style_set_bg_color(&style_btn_green, lv_color_hex(0x0c251b));
    lv_style_set_bg_opa(&style_btn_green, LV_OPA_COVER);
    lv_style_set_border_color(&style_btn_green, C_GREEN);
    lv_style_set_border_width(&style_btn_green, 1);
    lv_style_set_radius(&style_btn_green, 10);
    lv_style_set_shadow_width(&style_btn_green, 0);
    lv_style_set_shadow_spread(&style_btn_green, 0);
    lv_style_set_shadow_opa(&style_btn_green, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_btn_green, C_GREEN);
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, const char *caption, lv_color_t header_color = C_MUTED)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (caption && caption[0]) {
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, caption);
        lv_obj_set_style_text_color(lbl, header_color, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return card;
}

static void set_label_font(lv_obj_t *lbl, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
}

static void card_tap_cb(lv_event_t *e);

static void make_card_tappable(lv_obj_t *card, int view)
{
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_tap_cb, LV_EVENT_PRESSED, (void *)(intptr_t)view);
}

static void fmt_uptime(char *buf, size_t sz, uint32_t sec)
{
    uint32_t d = sec / 86400;
    uint32_t h = (sec % 86400) / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (d > 0) {
        snprintf(buf, sz, "UPTIME  %ud %02uh %02um", d, h, m);
    } else {
        snprintf(buf, sz, "UPTIME  %02uh %02um", h, m);
    }
}

static void fmt_clock(char *buf, size_t sz, uint32_t epoch)
{
    uint32_t sec = (epoch + 3 * 3600) % 86400;
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    snprintf(buf, sz, "%02u:%02u:%02u", h, m, s);
}

static void sync_cores()
{
    for (int i = 0; i < 32; i++) {
        if (i < g_ncores) {
            lv_obj_clear_flag(bar_cores[i], LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(bar_cores[i], g_cores[i], LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(bar_cores[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------- Overlays management ----------
static void rebuild_proc_rows();

static int view_storage(int view)
{
    switch (view) {
    case VIEW_CPU: return PROC_KIND_CPU;
    case VIEW_RAM: return PROC_KIND_RAM;
    case VIEW_GPU: return PROC_KIND_GPU;
    default: return PROC_KIND_DISK_RD;
    }
}

static void ovl_close()
{
    if (ovl_proc) lv_obj_add_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_open_view(int view)
{
    static const char *titles[] = {"TOP CPU", "TOP RAM", "TOP GPU", "TOP DISK (KiB/s)"};
    if (view > VIEW_DISK) view = VIEW_CPU;
    g_active_kind = view;
    lv_label_set_text(lbl_ovl_title, titles[view]);
    rebuild_proc_rows();
    lv_obj_clear_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN))
        ovl_open_view(VIEW_CPU);
    else
        ovl_close();
}

static void ovl_close_cb(lv_event_t *e)
{
    (void)e;
    ovl_close();
}

static void rebuild_llm_model_buttons();

static void ovl_close_llm()
{
    if (ovl_llm) lv_obj_add_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_open_llm()
{
    if (!ovl_llm) return;
    rebuild_llm_model_buttons();
    lv_obj_clear_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN);
}

void ui_open_llm_menu()
{
    ovl_open_llm();
}

static void ovl_llm_close_cb(lv_event_t *e)
{
    (void)e;
    ovl_close_llm();
}

// ---------- LAMP Modal Callbacks ----------
static void ovl_lamp_open()
{
    if (ovl_lamp) lv_obj_clear_flag(ovl_lamp, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_lamp_close()
{
    if (ovl_lamp) lv_obj_add_flag(ovl_lamp, LV_OBJ_FLAG_HIDDEN);
}

static void btn_lamp_power_cb(lv_event_t *e)
{
    (void)e;
    g_lamp_ui_power = !g_lamp_ui_power;
    ble_lamp_set_power(g_lamp_ui_power);

    if (g_lamp_ui_power) {
        lv_label_set_text(lbl_lamp_power, "POWER: ON");
        lv_obj_set_style_text_color(lbl_lamp_power, C_GREEN, 0);
        lv_obj_set_style_border_color(btn_lamp_power, C_GREEN, 0);
        lv_obj_set_style_bg_color(btn_lamp_power, lv_color_hex(0x0c251b), 0);
    } else {
        lv_label_set_text(lbl_lamp_power, "POWER: OFF");
        lv_obj_set_style_text_color(lbl_lamp_power, C_RED, 0);
        lv_obj_set_style_border_color(btn_lamp_power, C_RED, 0);
        lv_obj_set_style_bg_color(btn_lamp_power, lv_color_hex(0x281116), 0);
    }
}

static void color_preset_cb(lv_event_t *e)
{
    uint32_t hsv = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    uint16_t h = (hsv >> 16) & 0xFFFF;
    uint8_t s = (hsv >> 8) & 0xFF;
    uint8_t v = hsv & 0xFF;
    ble_lamp_set_color_hsv(h, s, v);
}

static void lamp_bright_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int val = lv_slider_get_value(sl);
    ble_lamp_set_brightness((uint16_t)val);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", val / 10);
    lv_label_set_text(lbl_lamp_bright_val, buf);
}

static void lamp_cct_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int val = lv_slider_get_value(sl);
    ble_lamp_set_cct((uint16_t)val);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", val / 10);
    lv_label_set_text(lbl_lamp_cct_val, buf);
}

static void ambient_sync_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ble_lamp_set_ambient_sync(state);
}

static void btn_stop_all_cb(lv_event_t *e)
{
    (void)e;
    protocol_send_cmd(CMD_STOP_ALL);
}

struct LlmProfileItem {
    char name[13];
    uint32_t ctx_size;
    char desc[19];
};
static LlmProfileItem g_current_profiles[6];
static int g_current_profiles_count = 0;
static char g_current_profile_model_id[15] = {0};

static void ovl_open_profiles_modal(int model_idx);
static void ovl_close_profiles_modal();

static void model_card_tap_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int model_idx = g_llm_page * LLM_ITEMS_PER_PAGE + slot;
    if (model_idx >= 0 && model_idx < g_llm_models_count) {
        ovl_open_profiles_modal(model_idx);
    }
}

static void btn_page_prev_cb(lv_event_t *e)
{
    (void)e;
    if (g_llm_page > 0) {
        g_llm_page--;
        rebuild_llm_model_buttons();
    }
}

static void btn_page_next_cb(lv_event_t *e)
{
    (void)e;
    int max_pages = (g_llm_models_count + LLM_ITEMS_PER_PAGE - 1) / LLM_ITEMS_PER_PAGE;
    if (max_pages < 1) max_pages = 1;
    if (g_llm_page + 1 < max_pages) {
        g_llm_page++;
        rebuild_llm_model_buttons();
    }
}

static void btn_prof_stop_cb(lv_event_t *e)
{
    (void)e;
    protocol_send_cmd(CMD_STOP_ALL);
    ovl_close_profiles_modal();
}

static void profile_btn_tap_cb(lv_event_t *e)
{
    int prof_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_selected_model_idx >= 0 && g_selected_model_idx < g_llm_models_count &&
        prof_idx >= 0 && prof_idx < g_current_profiles_count) {
        const char *m_id = g_llm_models[g_selected_model_idx].id;
        const char *prof = g_current_profiles[prof_idx].name;
        protocol_send_start_model_profile(m_id, prof);
    }
    ovl_close_profiles_modal();
}

static void ovl_open_profiles_modal(int model_idx)
{
    if (model_idx < 0 || model_idx >= g_llm_models_count || !ovl_llm_profiles) return;
    g_selected_model_idx = model_idx;
    const char *model_id = g_llm_models[model_idx].id;

    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "MODEL: %s", model_id);
    lv_label_set_text(lbl_prof_model_title, title_buf);

    if (g_llm_models[model_idx].status == LLM_STATUS_RUNNING || g_llm_models[model_idx].status == LLM_STATUS_STARTING) {
        lv_obj_clear_flag(btn_prof_stop, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_prof_subtitle, "Status: RUNNING. Switch profile or Stop:");
    } else {
        lv_obj_add_flag(btn_prof_stop, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_prof_subtitle, "Select profile to launch:");
    }

    g_current_profiles_count = 0;
    strncpy(g_current_profile_model_id, model_id, sizeof(g_current_profile_model_id) - 1);
    g_current_profile_model_id[sizeof(g_current_profile_model_id) - 1] = '\0';
    for (int i = 0; i < 6; i++) {
        lv_obj_add_flag(btn_profiles[i], LV_OBJ_FLAG_HIDDEN);
    }

    protocol_send_get_profiles(model_id);

    lv_obj_clear_flag(ovl_llm_profiles, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_close_profiles_modal()
{
    if (ovl_llm_profiles) {
        lv_obj_add_flag(ovl_llm_profiles, LV_OBJ_FLAG_HIDDEN);
    }
}

static void card_tap_cb(lv_event_t *e)
{
    int view = (int)(intptr_t)lv_event_get_user_data(e);
    ovl_open_view(view);
}

static void slider_bright_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    backlight_set((uint8_t)lv_slider_get_value(sl));
}

// Remote / Programmatic Screen Switcher
void ui_open_screen(uint8_t screen_id)
{
    ovl_close();
    ovl_close_llm();
    ovl_lamp_close();
    ovl_close_profiles_modal();

    if (screen_id == 1) {
        ovl_open_llm();
    } else if (screen_id == 2) {
        ovl_lamp_open();
    } else if (screen_id == 3) {
        ovl_open_view(VIEW_CPU);
    } else if (screen_id == 4) {
        ovl_open_view(VIEW_RAM);
    } else if (screen_id == 5) {
        ovl_open_view(VIEW_GPU);
    } else if (screen_id == 6) {
        ovl_open_view(VIEW_DISK);
    }
}

// ---------- periodic refresh (clock / uptime / load ambient sync) ----------
static void ui_periodic(lv_timer_t *t)
{
    (void)t;
    uint32_t ms = millis();

    if (g_hdr_init) {
        char buf[40];
        uint32_t elapsed = (ms - g_boot_ms) / 1000;
        fmt_clock(buf, sizeof(buf), g_wallclock_epoch + elapsed);
        lv_label_set_text(lbl_clock, buf);
        fmt_uptime(buf, sizeof(buf), g_uptime_base + elapsed);
        lv_label_set_text(lbl_uptime, buf);
    }

    // System-load Ambient Sync handler
    if (ble_lamp_get_ambient_sync()) {
        static uint32_t last_ambient_sync = 0;
        if (ms - last_ambient_sync > 1500) {
            last_ambient_sync = ms;
            int load = g_cpu_pct;
            if (g_ram_pct > load) load = g_ram_pct;
            if (g_gpu_pct != 255 && g_gpu_pct > load) load = g_gpu_pct;
            if (load > AMBIENT_PEAK_PCT) {
                ble_lamp_set_color_rgb_bright(255, 60, 40, 900);
            } else {
                ble_lamp_set_color_rgb_bright(255, 176, 32, 400);
            }
        }
    }
}

static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int16_t x, y;
    if (touch_read(x, y)) {
        bool wake_ok = (uint32_t)(millis() - g_screen_off_ms) >= SCREEN_OFF_COOLDOWN_MS &&
                       g_released_since_off;
        if (g_screen_off && wake_ok) {
            screen_wake();
            g_suppress_until_release = true;
            data->state = LV_INDEV_STATE_REL;
        } else if (!g_screen_off && !g_suppress_until_release) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = x;
            data->point.y = y;
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    } else {
        data->state = LV_INDEV_STATE_REL;
        g_suppress_until_release = false;
        g_released_since_off = true;
    }
    (void)drv;
}

void ui_init(int w, int h)
{
    lv_init();

    buf1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * w * 200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) {
        buf1 = (lv_color_t *)malloc(sizeof(lv_color_t) * w * 200);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, w * 200);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = w;
    disp_drv.ver_res = h;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    style_init();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);

    // ---------------- Header ----------------
    lbl_clock = lv_label_create(scr);
    lv_label_set_text(lbl_clock, "--:--:--");
    set_label_font(lbl_clock, &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_LEFT, 16, 8);

    lbl_uptime = lv_label_create(scr);
    lv_label_set_text(lbl_uptime, "UPTIME --:--");
    set_label_font(lbl_uptime, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_uptime, LV_ALIGN_TOP_LEFT, 16, 32);

    // Header left area toggles the proc overlay
    lv_obj_add_flag(lbl_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl_uptime, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_clock, ovl_toggle_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lbl_uptime, ovl_toggle_cb, LV_EVENT_PRESSED, NULL);

    // LAMP button in header (Flat card style)
    btn_lamp_hdr = lv_obj_create(scr);
    lv_obj_set_size(btn_lamp_hdr, 88, 36);
    lv_obj_align(btn_lamp_hdr, LV_ALIGN_TOP_RIGHT, -100, 8);
    lv_obj_add_style(btn_lamp_hdr, &style_btn, 0);
    lv_obj_add_style(btn_lamp_hdr, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_lamp_hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_lamp_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_lamp_hdr, [](lv_event_t *e){ ovl_lamp_open(); }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *btn_lamp_lbl = lv_label_create(btn_lamp_hdr);
    lv_label_set_text(btn_lamp_lbl, "LAMP");
    set_label_font(btn_lamp_lbl, &lv_font_montserrat_14, C_AMBER);
    lv_obj_center(btn_lamp_lbl);

    // OFF button in header (Flat card style)
    btn_proc = lv_obj_create(scr);
    lv_obj_set_size(btn_proc, 88, 36);
    lv_obj_align(btn_proc, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_style(btn_proc, &style_btn, 0);
    lv_obj_add_style(btn_proc, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_proc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_proc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_proc, [](lv_event_t *e) { screen_off(); }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn_proc);
    lv_label_set_text(btn_lbl, "OFF");
    set_label_font(btn_lbl, &lv_font_montserrat_14, C_DIM);
    lv_obj_center(btn_lbl);

    // ---------------- 3-Row Grid Layout ----------------
    int margin = 12;
    int gap = 10;
    int card_w = (w - 2 * margin - gap) / 2; // 223px

    // Row 1: CPU & GPU (Height = 158)
    int row1_y = 54;
    int row1_h = 158;

    // CPU Card (Top-Left)
    card_cpu = make_card(scr, margin, row1_y, card_w, row1_h, "CPU", C_MUTED);
    lv_obj_t *cpu = card_cpu;

    arc_cpu = lv_arc_create(cpu);
    lv_obj_set_size(arc_cpu, 106, 106);
    lv_arc_set_range(arc_cpu, 0, 100);
    lv_arc_set_value(arc_cpu, 0);
    lv_obj_clear_flag(arc_cpu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(arc_cpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_cpu, &style_arc_indic, LV_PART_INDICATOR);
    lv_obj_align(arc_cpu, LV_ALIGN_CENTER, 0, -12);

    lbl_cpu_pct = lv_label_create(arc_cpu);
    lv_label_set_text(lbl_cpu_pct, "0%");
    set_label_font(lbl_cpu_pct, &lv_font_montserrat_20, C_CYAN);
    lv_obj_center(lbl_cpu_pct);

    cores_cont = lv_obj_create(cpu);
    lv_obj_set_size(cores_cont, card_w - 20, 20);
    lv_obj_set_style_bg_opa(cores_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cores_cont, 0, 0);
    lv_obj_set_style_pad_all(cores_cont, 0, 0);
    lv_obj_set_layout(cores_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cores_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cores_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cores_cont, 2, 0);
    lv_obj_align(cores_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(cores_cont, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 32; i++) {
        bar_cores[i] = lv_bar_create(cores_cont);
        lv_obj_set_size(bar_cores[i], 4, 18);
        lv_bar_set_range(bar_cores[i], 0, 100);
        lv_bar_set_value(bar_cores[i], 0, LV_ANIM_OFF);
        lv_obj_clear_flag(bar_cores[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(bar_cores[i], C_ARC_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_cores[i], C_CYAN, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_cores[i], 2, 0);
        lv_obj_set_style_radius(bar_cores[i], 2, LV_PART_INDICATOR);
        lv_obj_add_flag(bar_cores[i], LV_OBJ_FLAG_HIDDEN);
    }
    make_card_tappable(card_cpu, VIEW_CPU);

    // GPU Card (Top-Right)
    card_gpu = make_card(scr, margin + card_w + gap, row1_y, card_w, row1_h, "GPU", C_MUTED);
    lv_obj_t *gpu = card_gpu;

    arc_gpu = lv_arc_create(gpu);
    lv_obj_set_size(arc_gpu, 106, 106);
    lv_arc_set_range(arc_gpu, 0, 100);
    lv_arc_set_value(arc_gpu, 0);
    lv_obj_clear_flag(arc_gpu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(arc_gpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_gpu, &style_arc_gpu_indic, LV_PART_INDICATOR);
    lv_obj_align(arc_gpu, LV_ALIGN_CENTER, 0, -16);

    lbl_gpu_pct = lv_label_create(arc_gpu);
    lv_label_set_text(lbl_gpu_pct, "0%");
    set_label_font(lbl_gpu_pct, &lv_font_montserrat_20, C_PURPLE);
    lv_obj_center(lbl_gpu_pct);

    bar_vram = lv_bar_create(gpu);
    lv_obj_set_size(bar_vram, card_w - 32, 6);
    lv_obj_align(bar_vram, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_bar_set_range(bar_vram, 0, 100);
    lv_bar_set_value(bar_vram, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar_vram, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_vram, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vram, C_PURPLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vram, 3, 0);
    lv_obj_set_style_radius(bar_vram, 3, LV_PART_INDICATOR);

    lbl_vram = lv_label_create(gpu);
    lv_label_set_text(lbl_vram, "VRAM 0 / 0 MB");
    set_label_font(lbl_vram, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_vram, LV_ALIGN_BOTTOM_MID, 0, 0);
    make_card_tappable(card_gpu, VIEW_GPU);

    // Row 2: RAM & NET (Height = 142)
    int row2_y = row1_y + row1_h + gap; // 222
    int row2_h = 142;

    // RAM Card (Mid-Left)
    card_ram = make_card(scr, margin, row2_y, card_w, row2_h, "MEMORY (RAM)", C_MUTED);
    lv_obj_t *ram = card_ram;

    lbl_ram_pct = lv_label_create(ram);
    lv_label_set_text(lbl_ram_pct, "0%");
    set_label_font(lbl_ram_pct, &lv_font_montserrat_18, C_GREEN);
    lv_obj_align(lbl_ram_pct, LV_ALIGN_TOP_RIGHT, 0, -2);

    bar_ram = lv_bar_create(ram);
    lv_obj_set_size(bar_ram, card_w - 24, 16);
    lv_obj_align(bar_ram, LV_ALIGN_CENTER, 0, 2);
    lv_bar_set_range(bar_ram, 0, 100);
    lv_bar_set_value(bar_ram, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar_ram, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_ram, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_ram, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_ram, 8, 0);
    lv_obj_set_style_radius(bar_ram, 8, LV_PART_INDICATOR);

    lbl_ram = lv_label_create(ram);
    lv_label_set_text(lbl_ram, "0 / 0 MB");
    set_label_font(lbl_ram, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lbl_ram, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    make_card_tappable(card_ram, VIEW_RAM);

    // NET Card (Mid-Right)
    lv_obj_t *net = make_card(scr, margin + card_w + gap, row2_y, card_w, row2_h, "NETWORK", C_MUTED);

    lbl_rx = lv_label_create(net);
    lv_label_set_text(lbl_rx, "RX 0 K/s");
    set_label_font(lbl_rx, &lv_font_montserrat_14, C_CYAN);
    lv_obj_align(lbl_rx, LV_ALIGN_TOP_LEFT, 0, 18);

    lbl_tx = lv_label_create(net);
    lv_label_set_text(lbl_tx, "TX 0 K/s");
    set_label_font(lbl_tx, &lv_font_montserrat_14, C_GREEN);
    lv_obj_align(lbl_tx, LV_ALIGN_TOP_RIGHT, 0, 18);

    chart_net = lv_chart_create(net);
    lv_obj_set_size(chart_net, card_w - 20, 68);
    lv_obj_align(chart_net, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(chart_net, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart_net, LV_CHART_AXIS_PRIMARY_Y, 0, 1024);
    lv_chart_set_point_count(chart_net, 50);
    lv_chart_set_update_mode(chart_net, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart_net, 0, 2);
    lv_obj_set_style_bg_opa(chart_net, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart_net, 0, 0);
    ser_rx = lv_chart_add_series(chart_net, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);
    ser_tx = lv_chart_add_series(chart_net, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_color(chart_net, C_ARC_BG, LV_PART_MAIN);

    // Row 3: DISK & LLM (Height = 94, Y = 374)
    int row3_y = row2_y + row2_h + gap; // 374
    int row3_h = 94;

    // DISK Card (Bottom-Left) - No yellow bar, dual disk percentages + large rates
    card_disk = make_card(scr, margin, row3_y, card_w, row3_h, "DISK", C_MUTED);
    lv_obj_t *disk = card_disk;

    // Primary & Secondary disk usage percentage
    lbl_disk_p1 = lv_label_create(disk);
    lv_label_set_text(lbl_disk_p1, "D1: --%");
    set_label_font(lbl_disk_p1, &lv_font_montserrat_12, C_TEXT);
    lv_obj_align(lbl_disk_p1, LV_ALIGN_TOP_RIGHT, -64, 0);

    lbl_disk_p2 = lv_label_create(disk);
    lv_label_set_text(lbl_disk_p2, "D2: --%");
    set_label_font(lbl_disk_p2, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_disk_p2, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Large Read & Write rates
    lbl_disk_rd = lv_label_create(disk);
    lv_label_set_text(lbl_disk_rd, "R 0.0 M/s");
    set_label_font(lbl_disk_rd, &lv_font_montserrat_18, C_CYAN);
    lv_obj_align(lbl_disk_rd, LV_ALIGN_LEFT_MID, 0, 8);

    lbl_disk_wr = lv_label_create(disk);
    lv_label_set_text(lbl_disk_wr, "W 0.0 M/s");
    set_label_font(lbl_disk_wr, &lv_font_montserrat_18, C_AMBER);
    lv_obj_align(lbl_disk_wr, LV_ALIGN_RIGHT_MID, 0, 8);

    make_card_tappable(card_disk, VIEW_DISK);

    // LLM Card (Bottom-Right) - Large prominent touch card
    card_llm = make_card(scr, margin + card_w + gap, row3_y, card_w, row3_h, "LLM AGENT", C_MUTED);

    lbl_llm_badge = lv_label_create(card_llm);
    lv_label_set_text(lbl_llm_badge, "IDLE");
    set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_llm_badge, LV_ALIGN_TOP_RIGHT, 0, 0);

    lbl_llm = lv_label_create(card_llm);
    lv_label_set_text(lbl_llm, "IDLE (Tap to load)");
    set_label_font(lbl_llm, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lbl_llm, LV_ALIGN_LEFT_MID, 0, -2);

    lv_obj_t *lbl_llm_sub = lv_label_create(card_llm);
    lv_label_set_text(lbl_llm_sub, "Profiles >");
    set_label_font(lbl_llm_sub, &lv_font_montserrat_12, C_CYAN);
    lv_obj_align(lbl_llm_sub, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_add_flag(card_llm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card_llm, [](lv_event_t *e) {
        ovl_open_llm();
    }, LV_EVENT_PRESSED, NULL);

    // ---------------- SMART LAMP MODAL (ovl_lamp) ----------------
    // Redesigned with Clean Modern Dark aesthetic
    ovl_lamp = lv_obj_create(scr);
    lv_obj_set_size(ovl_lamp, w, h);
    lv_obj_set_pos(ovl_lamp, 0, 0);
    lv_obj_set_style_bg_color(ovl_lamp, C_BG, 0);
    lv_obj_set_style_radius(ovl_lamp, 0, 0);
    lv_obj_set_style_border_width(ovl_lamp, 0, 0);
    lv_obj_clear_flag(ovl_lamp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_lamp, LV_OBJ_FLAG_HIDDEN);

    // Header
    lv_obj_t *lbl_lamp_title = lv_label_create(ovl_lamp);
    lv_label_set_text(lbl_lamp_title, "SMART LAMP");
    set_label_font(lbl_lamp_title, &lv_font_montserrat_20, C_AMBER);
    lv_obj_align(lbl_lamp_title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *btn_lamp_close = lv_obj_create(ovl_lamp);
    lv_obj_set_size(btn_lamp_close, 88, 36);
    lv_obj_align(btn_lamp_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_style(btn_lamp_close, &style_btn, 0);
    lv_obj_add_style(btn_lamp_close, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_lamp_close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_lamp_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_lamp_close, [](lv_event_t *e){ ovl_lamp_close(); }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_lclose = lv_label_create(btn_lamp_close);
    lv_label_set_text(lbl_lclose, "CLOSE");
    set_label_font(lbl_lclose, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_lclose);

    // Card 1: Power Hero Card (Y: 52, H: 54)
    lv_obj_t *card_lamp_pwr = make_card(ovl_lamp, 12, 52, 456, 54, "");
    lv_obj_t *lbl_pwr_t = lv_label_create(card_lamp_pwr);
    lv_label_set_text(lbl_pwr_t, "LAMP POWER");
    set_label_font(lbl_pwr_t, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lbl_pwr_t, LV_ALIGN_LEFT_MID, 4, -8);

    lv_obj_t *lbl_pwr_sub = lv_label_create(card_lamp_pwr);
    lv_label_set_text(lbl_pwr_sub, "BLE Smart Mesh Link");
    set_label_font(lbl_pwr_sub, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_pwr_sub, LV_ALIGN_LEFT_MID, 4, 10);

    btn_lamp_power = lv_obj_create(card_lamp_pwr);
    lv_obj_set_size(btn_lamp_power, 110, 36);
    lv_obj_align(btn_lamp_power, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(btn_lamp_power, &style_btn_green, 0);
    lv_obj_clear_flag(btn_lamp_power, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_lamp_power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_lamp_power, btn_lamp_power_cb, LV_EVENT_PRESSED, NULL);
    lbl_lamp_power = lv_label_create(btn_lamp_power);
    lv_label_set_text(lbl_lamp_power, "ON");
    set_label_font(lbl_lamp_power, &lv_font_montserrat_16, C_GREEN);
    lv_obj_center(lbl_lamp_power);

    // Card 2: Color Presets (Y: 114, H: 80)
    lv_obj_t *card_presets = make_card(ovl_lamp, 12, 114, 456, 80, "COLOR PRESETS", C_MUTED);
    uint32_t colors[6] = {0x00EAFF, 0x00FF9C, 0xFFB020, 0xC084FC, 0xEF4444, 0xFFFFFF};
    uint32_t hsvs[6] = {
        0x00B46464, // Cyan:   hue 180, sat 100, val 100
        0x00786464, // Green:  hue 120, sat 100, val 100
        0x00236464, // Amber:  hue 35,  sat 100, val 100
        0x01186464, // Purple: hue 280, sat 100, val 100
        0x00006464, // Red:    hue 0,   sat 100, val 100
        0x00000064, // White:  hue 0,   sat 0,   val 100
    };
    const char *pnames[6] = {"Cyan", "Mint", "Amber", "Purple", "Ruby", "Daylight"};
    int p_w = 68;
    for (int i = 0; i < 6; i++) {
        lv_obj_t *pbtn = lv_obj_create(card_presets);
        lv_obj_set_size(pbtn, p_w, 36);
        lv_obj_set_pos(pbtn, i * 73, 22);
        lv_obj_set_style_bg_color(pbtn, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_radius(pbtn, 10, 0);
        lv_obj_set_style_border_width(pbtn, 1, 0);
        lv_obj_set_style_border_color(pbtn, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_shadow_width(pbtn, 0, 0);
        lv_obj_set_style_shadow_opa(pbtn, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(pbtn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pbtn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pbtn, color_preset_cb, LV_EVENT_PRESSED, (void *)(intptr_t)hsvs[i]);

        lv_obj_t *plbl = lv_label_create(pbtn);
        lv_label_set_text(plbl, pnames[i]);
        set_label_font(plbl, &lv_font_montserrat_12, (colors[i] == 0xFFFFFF || colors[i] == 0x00EAFF || colors[i] == 0x00FF9C || colors[i] == 0xFFB020) ? C_BG : C_TEXT);
        lv_obj_center(plbl);
    }

    // Card 3: Light Controls Sliders (Y: 202, H: 114)
    lv_obj_t *card_sliders = make_card(ovl_lamp, 12, 202, 456, 114, "LIGHT CONTROLS", C_MUTED);

    // Brightness row
    lbl_lamp_bright_val = lv_label_create(card_sliders);
    lv_label_set_text(lbl_lamp_bright_val, "Brightness: 80%");
    set_label_font(lbl_lamp_bright_val, &lv_font_montserrat_12, C_AMBER);
    lv_obj_align(lbl_lamp_bright_val, LV_ALIGN_TOP_RIGHT, 0, 0);

    slider_lamp_bright = lv_slider_create(card_sliders);
    lv_obj_set_size(slider_lamp_bright, 432, 14);
    lv_obj_align(slider_lamp_bright, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_slider_set_range(slider_lamp_bright, 10, 1000);
    lv_slider_set_value(slider_lamp_bright, 800, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_lamp_bright, lamp_bright_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_set_style_bg_color(slider_lamp_bright, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_lamp_bright, C_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_lamp_bright, C_TEXT, LV_PART_KNOB);

    // Warmth / CCT row
    lbl_lamp_cct_val = lv_label_create(card_sliders);
    lv_label_set_text(lbl_lamp_cct_val, "Warmth / CCT: 50%");
    set_label_font(lbl_lamp_cct_val, &lv_font_montserrat_12, C_CYAN);
    lv_obj_align(lbl_lamp_cct_val, LV_ALIGN_TOP_RIGHT, 0, 52);

    slider_lamp_cct = lv_slider_create(card_sliders);
    lv_obj_set_size(slider_lamp_cct, 432, 14);
    lv_obj_align(slider_lamp_cct, LV_ALIGN_TOP_LEFT, 0, 72);
    lv_slider_set_range(slider_lamp_cct, 0, 1000);
    lv_slider_set_value(slider_lamp_cct, 500, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_lamp_cct, lamp_cct_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_set_style_bg_color(slider_lamp_cct, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_lamp_cct, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_lamp_cct, C_TEXT, LV_PART_KNOB);

    // Card 4: Load Ambient Sync (Y: 324, H: 76)
    lv_obj_t *ambient_card = make_card(ovl_lamp, 12, 324, 456, 76, "LOAD AMBIENT SYNC", C_CYAN);
    lv_obj_set_style_border_color(ambient_card, C_CYAN, 0);

    lv_obj_t *lbl_amb_d = lv_label_create(ambient_card);
    lv_label_set_text(lbl_amb_d, "Adapts color from warm amber to red under peak PC load");
    set_label_font(lbl_amb_d, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_amb_d, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    sw_ambient_sync = lv_switch_create(ambient_card);
    lv_obj_align(sw_ambient_sync, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw_ambient_sync, ambient_sync_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Bottom Strip: Panel Backlight
    lv_obj_t *ovl_lamp_brlbl = lv_label_create(ovl_lamp);
    lv_label_set_text(ovl_lamp_brlbl, "PANEL BRIGHT");
    set_label_font(ovl_lamp_brlbl, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(ovl_lamp_brlbl, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    lv_obj_t *ovl_lamp_slider = lv_slider_create(ovl_lamp);
    lv_obj_set_size(ovl_lamp_slider, w - 170, 12);
    lv_obj_align(ovl_lamp_slider, LV_ALIGN_BOTTOM_LEFT, 140, -18);
    lv_slider_set_range(ovl_lamp_slider, 0, 255);
    lv_slider_set_value(ovl_lamp_slider, 150, LV_ANIM_OFF);
    lv_obj_add_event_cb(ovl_lamp_slider, slider_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(ovl_lamp_slider, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ovl_lamp_slider, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ovl_lamp_slider, C_TEXT, LV_PART_KNOB);

    // ---------------- LLM overlay (Clean Structured Cards) ----------------
    ovl_llm = lv_obj_create(scr);
    lv_obj_set_size(ovl_llm, w, h);
    lv_obj_set_pos(ovl_llm, 0, 0);
    lv_obj_set_style_bg_color(ovl_llm, C_BG, 0);
    lv_obj_set_style_radius(ovl_llm, 0, 0);
    lv_obj_set_style_border_width(ovl_llm, 0, 0);
    lv_obj_clear_flag(ovl_llm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN);

    // Header
    lv_obj_t *lbl_llm_title = lv_label_create(ovl_llm);
    lv_label_set_text(lbl_llm_title, "LLM MODELS");
    set_label_font(lbl_llm_title, &lv_font_montserrat_20, C_CYAN);
    lv_obj_align(lbl_llm_title, LV_ALIGN_TOP_LEFT, 16, 10);

    lv_obj_t *btn_stop = lv_obj_create(ovl_llm);
    lv_obj_set_size(btn_stop, 108, 36);
    lv_obj_align(btn_stop, LV_ALIGN_TOP_RIGHT, -104, 8);
    lv_obj_add_style(btn_stop, &style_btn_red, 0);
    lv_obj_clear_flag(btn_stop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_stop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_stop, btn_stop_all_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_s = lv_label_create(btn_stop);
    lv_label_set_text(lbl_s, "STOP ALL");
    set_label_font(lbl_s, &lv_font_montserrat_14, C_RED);
    lv_obj_center(lbl_s);

    lv_obj_t *btn_llm_close = lv_obj_create(ovl_llm);
    lv_obj_set_size(btn_llm_close, 84, 36);
    lv_obj_align(btn_llm_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_style(btn_llm_close, &style_btn, 0);
    lv_obj_add_style(btn_llm_close, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_llm_close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_llm_close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_llm_close, ovl_llm_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_close = lv_label_create(btn_llm_close);
    lv_label_set_text(lbl_close, "CLOSE");
    set_label_font(lbl_close, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_close);

    // Status Banner Card (Y: 50, H: 44)
    card_llm_banner = make_card(ovl_llm, 12, 50, 456, 44, "");
    lbl_llm_ovl_status = lv_label_create(card_llm_banner);
    lv_label_set_text(lbl_llm_ovl_status, "Status: IDLE");
    set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lbl_llm_ovl_status, LV_ALIGN_LEFT_MID, 4, 0);

    // 6 Model Cards Grid (2 cols x 3 rows, matching main dashboard cards)
    int llm_xs[2] = {12, 244};
    int llm_ys[3] = {102, 190, 278};
    int llm_cw = 224;
    int llm_ch = 80;

    for (int i = 0; i < 6; i++) {
        int col = i % 2;
        int row = i / 2;
        btn_llm_cards[i] = lv_obj_create(ovl_llm);
        lv_obj_set_size(btn_llm_cards[i], llm_cw, llm_ch);
        lv_obj_set_pos(btn_llm_cards[i], llm_xs[col], llm_ys[row]);
        lv_obj_add_style(btn_llm_cards[i], &style_card, 0);
        lv_obj_add_style(btn_llm_cards[i], &style_btn_pr, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn_llm_cards[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn_llm_cards[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn_llm_cards[i], model_card_tap_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);

        lbl_llm_card_name[i] = lv_label_create(btn_llm_cards[i]);
        lv_label_set_text(lbl_llm_card_name[i], "");
        set_label_font(lbl_llm_card_name[i], &lv_font_montserrat_16, C_TEXT);
        lv_obj_align(lbl_llm_card_name[i], LV_ALIGN_TOP_LEFT, 0, 0);

        lbl_llm_card_status[i] = lv_label_create(btn_llm_cards[i]);
        lv_label_set_text(lbl_llm_card_status[i], "");
        set_label_font(lbl_llm_card_status[i], &lv_font_montserrat_12, C_MUTED);
        lv_obj_align(lbl_llm_card_status[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lbl_llm_card_hint[i] = lv_label_create(btn_llm_cards[i]);
        lv_label_set_text(lbl_llm_card_hint[i], "Profiles >");
        set_label_font(lbl_llm_card_hint[i], &lv_font_montserrat_12, C_CYAN);
        lv_obj_align(lbl_llm_card_hint[i], LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        lv_obj_add_flag(btn_llm_cards[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Pager Dock (Y: 366, H: 40)
    btn_page_prev = lv_obj_create(ovl_llm);
    lv_obj_set_size(btn_page_prev, 100, 38);
    lv_obj_set_pos(btn_page_prev, 12, 366);
    lv_obj_add_style(btn_page_prev, &style_btn, 0);
    lv_obj_add_style(btn_page_prev, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_page_prev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_page_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_page_prev, btn_page_prev_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_prev = lv_label_create(btn_page_prev);
    lv_label_set_text(lbl_prev, "< PREV");
    set_label_font(lbl_prev, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_prev);

    lbl_page_num = lv_label_create(ovl_llm);
    lv_label_set_text(lbl_page_num, "Page 1 of 1");
    set_label_font(lbl_page_num, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lbl_page_num, LV_ALIGN_TOP_MID, 0, 376);

    btn_page_next = lv_obj_create(ovl_llm);
    lv_obj_set_size(btn_page_next, 100, 38);
    lv_obj_set_pos(btn_page_next, 368, 366);
    lv_obj_add_style(btn_page_next, &style_btn, 0);
    lv_obj_add_style(btn_page_next, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_page_next, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_page_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_page_next, btn_page_next_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_next = lv_label_create(btn_page_next);
    lv_label_set_text(lbl_next, "NEXT >");
    set_label_font(lbl_next, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_next);

    // Bottom Brightness Slider (Y: 420)
    lv_obj_t *ovl_llm_brlbl = lv_label_create(ovl_llm);
    lv_label_set_text(ovl_llm_brlbl, "PANEL BRIGHT");
    set_label_font(ovl_llm_brlbl, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(ovl_llm_brlbl, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    lv_obj_t *ovl_llm_slider = lv_slider_create(ovl_llm);
    lv_obj_set_size(ovl_llm_slider, w - 170, 12);
    lv_obj_align(ovl_llm_slider, LV_ALIGN_BOTTOM_LEFT, 140, -18);
    lv_slider_set_range(ovl_llm_slider, 0, 255);
    lv_slider_set_value(ovl_llm_slider, 150, LV_ANIM_OFF);
    lv_obj_add_event_cb(ovl_llm_slider, slider_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_TEXT, LV_PART_KNOB);

    // ---------------- Profile Picker Modal (ovl_llm_profiles) ----------------
    ovl_llm_profiles = lv_obj_create(scr);
    lv_obj_set_size(ovl_llm_profiles, w, h);
    lv_obj_set_pos(ovl_llm_profiles, 0, 0);
    lv_obj_set_style_bg_color(ovl_llm_profiles, C_BG, 0);
    lv_obj_set_style_radius(ovl_llm_profiles, 0, 0);
    lv_obj_set_style_border_width(ovl_llm_profiles, 0, 0);
    lv_obj_clear_flag(ovl_llm_profiles, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_llm_profiles, LV_OBJ_FLAG_HIDDEN);

    lbl_prof_model_title = lv_label_create(ovl_llm_profiles);
    lv_label_set_text(lbl_prof_model_title, "MODEL: --");
    set_label_font(lbl_prof_model_title, &lv_font_montserrat_20, C_CYAN);
    lv_obj_align(lbl_prof_model_title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *btn_prof_back = lv_obj_create(ovl_llm_profiles);
    lv_obj_set_size(btn_prof_back, 88, 36);
    lv_obj_align(btn_prof_back, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_style(btn_prof_back, &style_btn, 0);
    lv_obj_add_style(btn_prof_back, &style_btn_pr, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_prof_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_prof_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_prof_back, [](lv_event_t *e) {
        ovl_close_profiles_modal();
    }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_prof_back);
    lv_label_set_text(lbl_back, "BACK");
    set_label_font(lbl_back, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_back);

    btn_prof_stop = lv_obj_create(ovl_llm_profiles);
    lv_obj_set_size(btn_prof_stop, 456, 42);
    lv_obj_set_pos(btn_prof_stop, 12, 52);
    lv_obj_add_style(btn_prof_stop, &style_btn_red, 0);
    lv_obj_clear_flag(btn_prof_stop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_prof_stop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_prof_stop, btn_prof_stop_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_ps = lv_label_create(btn_prof_stop);
    lv_label_set_text(lbl_ps, "STOP RUNNING MODEL");
    set_label_font(lbl_ps, &lv_font_montserrat_14, C_RED);
    lv_obj_center(lbl_ps);

    lbl_prof_subtitle = lv_label_create(ovl_llm_profiles);
    lv_label_set_text(lbl_prof_subtitle, "Select profile to launch:");
    set_label_font(lbl_prof_subtitle, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lbl_prof_subtitle, LV_ALIGN_TOP_LEFT, 16, 102);

    int prof_ys[3] = {126, 216, 306};
    for (int i = 0; i < 6; i++) {
        int col = i % 2;
        int row = i / 2;
        btn_profiles[i] = lv_obj_create(ovl_llm_profiles);
        lv_obj_set_size(btn_profiles[i], llm_cw, 80);
        lv_obj_set_pos(btn_profiles[i], llm_xs[col], prof_ys[row]);
        lv_obj_add_style(btn_profiles[i], &style_card, 0);
        lv_obj_add_style(btn_profiles[i], &style_btn_pr, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn_profiles[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn_profiles[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn_profiles[i], profile_btn_tap_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);

        lbl_profiles_title[i] = lv_label_create(btn_profiles[i]);
        lv_label_set_text(lbl_profiles_title[i], "");
        set_label_font(lbl_profiles_title[i], &lv_font_montserrat_16, C_TEXT);
        lv_obj_align(lbl_profiles_title[i], LV_ALIGN_TOP_LEFT, 0, 0);

        lbl_profiles_desc[i] = lv_label_create(btn_profiles[i]);
        lv_label_set_text(lbl_profiles_desc[i], "");
        set_label_font(lbl_profiles_desc[i], &lv_font_montserrat_12, C_MUTED);
        lv_obj_align(lbl_profiles_desc[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_add_flag(btn_profiles[i], LV_OBJ_FLAG_HIDDEN);
    }

    // ---------------- PROC overlay ----------------
    ovl_proc = lv_obj_create(scr);
    lv_obj_set_size(ovl_proc, w, h);
    lv_obj_set_pos(ovl_proc, 0, 0);
    lv_obj_set_style_bg_color(ovl_proc, C_BG, 0);
    lv_obj_set_style_radius(ovl_proc, 0, 0);
    lv_obj_set_style_border_width(ovl_proc, 0, 0);
    lv_obj_clear_flag(ovl_proc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);

    lbl_ovl_title = lv_label_create(ovl_proc);
    lv_label_set_text(lbl_ovl_title, "TOP CPU");
    set_label_font(lbl_ovl_title, &lv_font_montserrat_18, C_TEXT);
    lv_obj_align(lbl_ovl_title, LV_ALIGN_TOP_LEFT, 16, 12);

    for (int i = 0; i < 10; i++) {
        proc_rows[i] = lv_label_create(ovl_proc);
        lv_label_set_text(proc_rows[i], "");
        set_label_font(proc_rows[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(proc_rows[i], LV_ALIGN_TOP_LEFT, 18, 52 + i * 26);
    }

    lv_obj_t *ovl_brlbl = lv_label_create(ovl_proc);
    lv_label_set_text(ovl_brlbl, "PANEL BRIGHT");
    set_label_font(ovl_brlbl, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(ovl_brlbl, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    lv_obj_t *ovl_slider = lv_slider_create(ovl_proc);
    lv_obj_set_size(ovl_slider, w - 170, 12);
    lv_obj_align(ovl_slider, LV_ALIGN_BOTTOM_LEFT, 140, -18);
    lv_slider_set_range(ovl_slider, 0, 255);
    lv_slider_set_value(ovl_slider, 150, LV_ANIM_OFF);
    lv_obj_add_event_cb(ovl_slider, slider_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(ovl_slider, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ovl_slider, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ovl_slider, C_TEXT, LV_PART_KNOB);

    lv_obj_t *ovl_close = lv_btn_create(ovl_proc);
    lv_obj_set_size(ovl_close, 88, 36);
    lv_obj_align(ovl_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_style(ovl_close, &style_btn, LV_PART_MAIN);
    lv_obj_add_style(ovl_close, &style_btn_pr, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ovl_close, ovl_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *close_lbl = lv_label_create(ovl_close);
    lv_label_set_text(close_lbl, "CLOSE");
    set_label_font(close_lbl, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(close_lbl);

    // ---------------- touch indev ----------------
    touch_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = indev_read_cb;
    lv_indev_drv_register(&indev_drv);

    // ---------------- periodic refresh ----------------
    lv_timer_create(ui_periodic, 500, NULL);
}

// ---------- rebuild PROC rows ----------
static void rebuild_proc_rows()
{
    if (!ovl_proc) return;
    int n = (g_active_kind == VIEW_DISK) ? g_proc_n[PROC_KIND_DISK_RD]
                                         : g_proc_n[view_storage(g_active_kind)];
    for (int i = 0; i < PROC_ROWS; i++) {
        if (i < n) {
            char buf[64];
            switch (g_active_kind) {
            case VIEW_CPU:
                snprintf(buf, sizeof(buf), "%3u%% %5u %s",
                         (unsigned)g_proc_val[PROC_KIND_CPU][i],
                         g_proc_pid[PROC_KIND_CPU][i],
                         g_proc_name[PROC_KIND_CPU][i]);
                break;
            case VIEW_RAM:
                snprintf(buf, sizeof(buf), "%6u MB %5u %s",
                         (unsigned)g_proc_val[PROC_KIND_RAM][i],
                         g_proc_pid[PROC_KIND_RAM][i],
                         g_proc_name[PROC_KIND_RAM][i]);
                break;
            case VIEW_GPU:
                snprintf(buf, sizeof(buf), "%6u MB %5u %s",
                         (unsigned)g_proc_val[PROC_KIND_GPU][i],
                         g_proc_pid[PROC_KIND_GPU][i],
                         g_proc_name[PROC_KIND_GPU][i]);
                break;
            case VIEW_DISK:
                snprintf(buf, sizeof(buf), "rd %5u wr %5u %5u %s",
                         (unsigned)g_proc_val[PROC_KIND_DISK_RD][i],
                         (unsigned)g_proc_val[PROC_KIND_DISK_WR][i],
                         g_proc_pid[PROC_KIND_DISK_RD][i],
                         g_proc_name[PROC_KIND_DISK_RD][i]);
                break;
            default:
                buf[0] = '\0';
                break;
            }
            lv_label_set_text(proc_rows[i], buf);
        } else {
            lv_label_set_text(proc_rows[i], "");
        }
    }
}

void ui_set_cpu_pct(int pct, const uint8_t *cores, int ncores)
{
    if (ncores > 32) ncores = 32;
    if (ncores < 0) ncores = 0;
    g_ncores = ncores;
    for (int i = 0; i < ncores; i++) g_cores[i] = cores[i];
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_cpu_pct = pct;
    lv_arc_set_value(arc_cpu, pct);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_cpu_pct, buf);
    sync_cores();
}

void ui_set_gpu_pct(int pct, int vram_pct, uint32_t vram_used_mb, uint32_t vram_total_mb)
{
    g_gpu_pct = pct;
    if (pct == 255) {
        lv_arc_set_value(arc_gpu, 0);
        lv_obj_set_style_arc_color(arc_gpu, C_GRAY, LV_PART_INDICATOR);
        lv_label_set_text(lbl_gpu_pct, "--");
        lv_obj_set_style_text_color(lbl_gpu_pct, C_MUTED, 0);
    } else {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_arc_set_value(arc_gpu, pct);
        lv_obj_set_style_arc_color(arc_gpu, C_PURPLE, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_gpu_pct, C_PURPLE, 0);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(lbl_gpu_pct, buf);
    }

    if (vram_pct < 0) vram_pct = 0;
    if (vram_pct > 100) vram_pct = 100;
    lv_bar_set_value(bar_vram, vram_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_vram, vram_pct > 80 ? C_AMBER : C_PURPLE, LV_PART_INDICATOR);
    char buf[32];
    float used_gb = vram_used_mb / 1024.0f;
    float tot_gb = vram_total_mb / 1024.0f;
    if (vram_total_mb >= 1024) {
        snprintf(buf, sizeof(buf), "VRAM %.1f / %.1f GB", used_gb, tot_gb);
    } else {
        snprintf(buf, sizeof(buf), "VRAM %u / %u MB", vram_used_mb, vram_total_mb);
    }
    lv_label_set_text(lbl_vram, buf);
}

void ui_set_ram_pct(int pct, uint32_t used_mb, uint32_t total_mb)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_ram_pct = pct;
    lv_bar_set_value(bar_ram, pct, LV_ANIM_OFF);

    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    lv_label_set_text(lbl_ram_pct, pct_buf);

    char buf[32];
    float used_gb = used_mb / 1024.0f;
    float tot_gb = total_mb / 1024.0f;
    if (total_mb >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB", used_gb, tot_gb);
    } else {
        snprintf(buf, sizeof(buf), "%u / %u MB", used_mb, total_mb);
    }
    lv_label_set_text(lbl_ram, buf);
}

void ui_set_net(uint32_t rx_kibs, uint32_t tx_kibs)
{
    if (rx_kibs > g_net_max_seen) g_net_max_seen = rx_kibs;
    if (tx_kibs > g_net_max_seen) g_net_max_seen = tx_kibs;
    uint32_t range_max = g_net_max_seen > 1024 ? g_net_max_seen : 1024;

    if (chart_net) {
        lv_chart_set_range(chart_net, LV_CHART_AXIS_PRIMARY_Y, 0, range_max);
        lv_chart_set_next_value(chart_net, ser_rx, rx_kibs);
        lv_chart_set_next_value(chart_net, ser_tx, tx_kibs);
    }

    char buf[24];
    if (rx_kibs >= 1024) {
        snprintf(buf, sizeof(buf), "RX %.1f M/s", rx_kibs / 1024.0f);
    } else {
        snprintf(buf, sizeof(buf), "RX %u K/s", rx_kibs);
    }
    lv_label_set_text(lbl_rx, buf);

    if (tx_kibs >= 1024) {
        snprintf(buf, sizeof(buf), "TX %.1f M/s", tx_kibs / 1024.0f);
    } else {
        snprintf(buf, sizeof(buf), "TX %u K/s", tx_kibs);
    }
    lv_label_set_text(lbl_tx, buf);
}

void ui_set_disk(uint32_t rd_kibs, uint32_t wr_kibs, int used_pct1, int used_pct2)
{
    char p1_buf[16];
    snprintf(p1_buf, sizeof(p1_buf), "D1: %d%%", used_pct1);
    lv_label_set_text(lbl_disk_p1, p1_buf);

    if (used_pct2 <= 100) {
        char p2_buf[16];
        snprintf(p2_buf, sizeof(p2_buf), "D2: %d%%", used_pct2);
        lv_label_set_text(lbl_disk_p2, p2_buf);
        lv_obj_clear_flag(lbl_disk_p2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_disk_p1, LV_ALIGN_TOP_RIGHT, -64, 0);
    } else {
        lv_obj_add_flag(lbl_disk_p2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lbl_disk_p1, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    float rd = rd_kibs / 1024.0f;
    float wr = wr_kibs / 1024.0f;
    char rbuf[24], wbuf[24];
    if (rd >= 10.0f) {
        snprintf(rbuf, sizeof(rbuf), "R %.0f M/s", rd);
    } else {
        snprintf(rbuf, sizeof(rbuf), "R %.1f M/s", rd);
    }
    lv_label_set_text(lbl_disk_rd, rbuf);

    if (wr >= 10.0f) {
        snprintf(wbuf, sizeof(wbuf), "W %.0f M/s", wr);
    } else {
        snprintf(wbuf, sizeof(wbuf), "W %.1f M/s", wr);
    }
    lv_label_set_text(lbl_disk_wr, wbuf);
}

void ui_set_header(uint32_t uptime_sec, uint32_t epoch_sec, const char *hostname)
{
    (void)hostname;
    if (!g_hdr_init) {
        g_hdr_init = true;
        g_boot_ms = millis();
        g_wallclock_epoch = epoch_sec;
        g_uptime_base = uptime_sec;
    }
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ui_set_proc(const uint8_t *data, uint16_t len)
{
    if (!data || len < 2) return;
    uint8_t kind = data[0];
    if (kind >= PROC_KINDS) return;
    int n = data[1];
    int avail = (len - 2) / PROC_ENTRY_BYTES;
    if (n > avail) n = avail;
    if (n > PROC_ROWS) n = PROC_ROWS;
    g_proc_n[kind] = n;
    const uint8_t *p = data + 2;
    for (int i = 0; i < n; i++) {
        g_proc_val[kind][i] = rd_u32(p);
        g_proc_pid[kind][i] = p[4] | ((uint16_t)p[5] << 8);
        memcpy(g_proc_name[kind][i], p + 6, 16);
        g_proc_name[kind][i][15] = '\0';
        p += PROC_ENTRY_BYTES;
    }
    if (ovl_proc && !lv_obj_has_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN)) {
        bool relevant = (kind == view_storage(g_active_kind)) ||
                        (g_active_kind == VIEW_DISK &&
                          (kind == PROC_KIND_DISK_RD || kind == PROC_KIND_DISK_WR));
        if (relevant)
            rebuild_proc_rows();
    }
}

void ui_set_llm(uint8_t status, float tps, const char *model, uint8_t cache_hit_pct, uint16_t prompt_k, uint8_t flags)
{
    g_llm_status = status;
    g_llm_tps = tps;
    g_llm_cache_hit_pct = cache_hit_pct;
    g_llm_prompt_k = prompt_k;
    g_llm_flags = flags;

    if (model) {
        strncpy(g_llm_model, model, sizeof(g_llm_model) - 1);
        g_llm_model[sizeof(g_llm_model) - 1] = '\0';
    } else {
        g_llm_model[0] = '\0';
    }

    if (!lbl_llm) return;

    char buf[40];
    if (status == LLM_STATUS_PROMPT_EVAL) {
        lv_label_set_text(lbl_llm_badge, "PP EVAL");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_AMBER);
        if (cache_hit_pct <= 100) {
            snprintf(buf, sizeof(buf), "%uK (Hit %u%%)", prompt_k, cache_hit_pct);
        } else {
            snprintf(buf, sizeof(buf), "%s...", g_llm_model);
        }
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_AMBER);
        lv_obj_set_style_border_color(card_llm, C_AMBER, 0);
    } else if (status == LLM_STATUS_GENERATING) {
        lv_label_set_text(lbl_llm_badge, "GEN");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_GREEN);
        if (tps > 0.0f) {
            snprintf(buf, sizeof(buf), "%.1f tok/s", tps);
        } else {
            snprintf(buf, sizeof(buf), "%s", g_llm_model);
        }
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_GREEN);
        lv_obj_set_style_border_color(card_llm, C_GREEN, 0);
    } else if (status == LLM_STATUS_RUNNING) {
        lv_label_set_text(lbl_llm_badge, "ON");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_GREEN);
        if (tps > 0.0f) {
            snprintf(buf, sizeof(buf), "%s (%.1f t/s)", g_llm_model, tps);
        } else {
            snprintf(buf, sizeof(buf), "%s", g_llm_model);
        }
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_style_border_color(card_llm, C_BORDER, 0);
    } else if (status == LLM_STATUS_STARTING) {
        lv_label_set_text(lbl_llm_badge, "LOAD");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_AMBER);
        snprintf(buf, sizeof(buf), "%s...", g_llm_model);
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_AMBER);
        lv_obj_set_style_border_color(card_llm, C_AMBER, 0);
    } else if (status == LLM_STATUS_IDLE) {
        lv_label_set_text(lbl_llm_badge, "IDLE");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_MUTED);
        snprintf(buf, sizeof(buf), "Tap to select");
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_style_border_color(card_llm, C_BORDER, 0);
    } else {
        lv_label_set_text(lbl_llm_badge, "OFF");
        set_label_font(lbl_llm_badge, &lv_font_montserrat_12, C_DIM);
        snprintf(buf, sizeof(buf), "Offline");
        set_label_font(lbl_llm, &lv_font_montserrat_14, C_DIM);
        lv_obj_set_style_border_color(card_llm, C_BORDER, 0);
    }
    lv_label_set_text(lbl_llm, buf);

    if (ovl_llm && !lv_obj_has_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_llm_model_buttons();
    }
}

static void rebuild_llm_model_buttons()
{
    if (!lbl_llm_ovl_status || !lbl_page_num || !card_llm_banner) return;

    char st_buf[64];
    if (g_llm_status == LLM_STATUS_PROMPT_EVAL) {
        if (g_llm_flags & 1) {
            snprintf(st_buf, sizeof(st_buf), "Phase: PP EVAL | %uK tok | Cache: %u%% [ALERT]", g_llm_prompt_k, g_llm_cache_hit_pct);
            set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_RED);
            lv_obj_set_style_border_color(card_llm_banner, C_RED, 0);
        } else {
            snprintf(st_buf, sizeof(st_buf), "Phase: PP EVAL | %uK tok | Cache: %u%%", g_llm_prompt_k, g_llm_cache_hit_pct);
            set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_AMBER);
            lv_obj_set_style_border_color(card_llm_banner, C_AMBER, 0);
        }
    } else if (g_llm_status == LLM_STATUS_GENERATING) {
        if (g_llm_cache_hit_pct <= 100) {
            snprintf(st_buf, sizeof(st_buf), "Phase: GENERATING | %.1f tok/s | Cache: %u%%", g_llm_tps, g_llm_cache_hit_pct);
        } else {
            snprintf(st_buf, sizeof(st_buf), "Phase: GENERATING | %.1f tok/s", g_llm_tps);
        }
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_GREEN);
        lv_obj_set_style_border_color(card_llm_banner, C_GREEN, 0);
    } else if (g_llm_status == LLM_STATUS_RUNNING) {
        if ((g_llm_flags & 1) && g_llm_cache_hit_pct <= 100) {
            snprintf(st_buf, sizeof(st_buf), "Active: %s [IDLE] | Last Hit: %u%% [ALERT]", g_llm_model, g_llm_cache_hit_pct);
            set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_AMBER);
            lv_obj_set_style_border_color(card_llm_banner, C_AMBER, 0);
        } else if (g_llm_tps > 0.0f) {
            snprintf(st_buf, sizeof(st_buf), "Active: %s (%.1f tok/s)", g_llm_model, g_llm_tps);
            set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_GREEN);
            lv_obj_set_style_border_color(card_llm_banner, C_GREEN, 0);
        } else {
            snprintf(st_buf, sizeof(st_buf), "Active: %s [RUNNING]", g_llm_model);
            set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_GREEN);
            lv_obj_set_style_border_color(card_llm_banner, C_GREEN, 0);
        }
    } else if (g_llm_status == LLM_STATUS_STARTING) {
        snprintf(st_buf, sizeof(st_buf), "Loading: %s...", g_llm_model);
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_AMBER);
        lv_obj_set_style_border_color(card_llm_banner, C_AMBER, 0);
    } else if (g_llm_status == LLM_STATUS_IDLE) {
        snprintf(st_buf, sizeof(st_buf), "Status: IDLE (Tap card to select profile)");
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_style_border_color(card_llm_banner, C_BORDER, 0);
    } else {
        snprintf(st_buf, sizeof(st_buf), "Status: Offline (llmcontrol disconnected)");
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_DIM);
        lv_obj_set_style_border_color(card_llm_banner, C_BORDER, 0);
    }
    lv_label_set_text(lbl_llm_ovl_status, st_buf);

    int max_pages = (g_llm_models_count + LLM_ITEMS_PER_PAGE - 1) / LLM_ITEMS_PER_PAGE;
    if (max_pages < 1) max_pages = 1;
    if (g_llm_page >= max_pages) g_llm_page = max_pages - 1;
    if (g_llm_page < 0) g_llm_page = 0;

    char page_buf[32];
    snprintf(page_buf, sizeof(page_buf), "Page %d of %d", g_llm_page + 1, max_pages);
    lv_label_set_text(lbl_page_num, page_buf);

    for (int slot = 0; slot < 6; slot++) {
        int idx = g_llm_page * LLM_ITEMS_PER_PAGE + slot;
        if (idx < g_llm_models_count) {
            lv_obj_clear_flag(btn_llm_cards[slot], LV_OBJ_FLAG_HIDDEN);
            char name_buf[32];
            const char *star = g_llm_models[idx].is_fav ? "*" : "";
            snprintf(name_buf, sizeof(name_buf), "%s%s", star, g_llm_models[idx].id);
            lv_label_set_text(lbl_llm_card_name[slot], name_buf);

            if (g_llm_models[idx].status == LLM_STATUS_RUNNING) {
                char stat_buf[32];
                if (g_llm_tps > 0.0f) {
                    snprintf(stat_buf, sizeof(stat_buf), "RUNNING %.1f t/s", g_llm_tps);
                } else {
                    snprintf(stat_buf, sizeof(stat_buf), "RUNNING");
                }
                lv_label_set_text(lbl_llm_card_status[slot], stat_buf);
                set_label_font(lbl_llm_card_status[slot], &lv_font_montserrat_12, C_GREEN);
                lv_obj_set_style_border_color(btn_llm_cards[slot], C_GREEN, 0);
                lv_obj_set_style_border_width(btn_llm_cards[slot], 2, 0);
            } else if (g_llm_models[idx].status == LLM_STATUS_STARTING) {
                lv_label_set_text(lbl_llm_card_status[slot], "STARTING...");
                set_label_font(lbl_llm_card_status[slot], &lv_font_montserrat_12, C_AMBER);
                lv_obj_set_style_border_color(btn_llm_cards[slot], C_AMBER, 0);
                lv_obj_set_style_border_width(btn_llm_cards[slot], 2, 0);
            } else {
                lv_label_set_text(lbl_llm_card_status[slot], "STOPPED");
                set_label_font(lbl_llm_card_status[slot], &lv_font_montserrat_12, C_MUTED);
                lv_obj_set_style_border_color(btn_llm_cards[slot], C_BORDER, 0);
                lv_obj_set_style_border_width(btn_llm_cards[slot], 1, 0);
            }
        } else {
            lv_obj_add_flag(btn_llm_cards[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_set_llm_models(const uint8_t *data, uint16_t len)
{
    if (!data || len < 1) return;
    uint8_t count = data[0];
    int avail = (len - 1) / 16;
    if (count > avail) count = avail;
    if (count > 16) count = 16;
    g_llm_models_count = count;

    const uint8_t *p = data + 1;
    for (int i = 0; i < count; i++) {
        memcpy(g_llm_models[i].id, p, 14);
        g_llm_models[i].id[14] = '\0';
        g_llm_models[i].is_fav = p[14];
        g_llm_models[i].status = p[15];
        p += 16;
    }

    if (ovl_llm && !lv_obj_has_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_llm_model_buttons();
    }
}

void ui_set_llm_profiles(const uint8_t *data, uint16_t len)
{
    if (!data || len < 15) return;
    char model_id[15] = {0};
    memcpy(model_id, data, 14);
    model_id[14] = '\0';
    uint8_t count = data[14];
    if (count > 6) count = 6;

    // Strip trailing spaces / nulls
    for (int k = 13; k >= 0; k--) {
        if (model_id[k] == ' ' || model_id[k] == '\0') model_id[k] = '\0';
        else break;
    }
    char cur_id[15] = {0};
    strncpy(cur_id, g_current_profile_model_id, 14);
    for (int k = 13; k >= 0; k--) {
        if (cur_id[k] == ' ' || cur_id[k] == '\0') cur_id[k] = '\0';
        else break;
    }

    if (cur_id[0] != '\0' && strcmp(model_id, cur_id) != 0 && strncmp(model_id, cur_id, strlen(cur_id)) != 0) {
        return;
    }

    const uint8_t *p = data + 15;
    int avail = (len - 15) / 34;
    if (count > avail) count = avail;
    g_current_profiles_count = count;

    for (int i = 0; i < count; i++) {
        memcpy(g_current_profiles[i].name, p, 12);
        g_current_profiles[i].name[12] = '\0';
        g_current_profiles[i].ctx_size = rd_u32(p + 12);
        memcpy(g_current_profiles[i].desc, p + 16, 18);
        g_current_profiles[i].desc[18] = '\0';
        p += 34;
    }

    if (ovl_llm_profiles && !lv_obj_has_flag(ovl_llm_profiles, LV_OBJ_FLAG_HIDDEN)) {
        if (g_selected_model_idx >= 0 && g_selected_model_idx < g_llm_models_count &&
            (g_llm_models[g_selected_model_idx].status == LLM_STATUS_RUNNING ||
             g_llm_models[g_selected_model_idx].status == LLM_STATUS_STARTING)) {
            lv_label_set_text(lbl_prof_subtitle, "Status: RUNNING. Switch profile or Stop:");
        } else {
            lv_label_set_text(lbl_prof_subtitle, "Select profile to launch:");
        }

        for (int i = 0; i < 6; i++) {
            if (i < g_current_profiles_count) {
                lv_label_set_text(lbl_profiles_title[i], g_current_profiles[i].name);
                lv_label_set_text(lbl_profiles_desc[i], g_current_profiles[i].desc);
                lv_obj_clear_flag(btn_profiles[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(btn_profiles[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
