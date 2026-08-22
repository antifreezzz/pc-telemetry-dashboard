#include "ui.h"
#include "panel.h"
#include "protocol.h"
#include "touch.h"
#include <Arduino.h>
#include <string.h>

// ---------- display ----------
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *buf1 = NULL;

// ---------- colours ----------
static const lv_color_t C_BG = lv_color_hex(0x000000);
static const lv_color_t C_CARD = lv_color_hex(0x14171c);
static const lv_color_t C_ARC_BG = lv_color_hex(0x2a2f38);
static const lv_color_t C_CYAN = lv_color_hex(0x00eaff);
static const lv_color_t C_GREEN = lv_color_hex(0x00ff9c);
static const lv_color_t C_AMBER = lv_color_hex(0xffb020);
static const lv_color_t C_TEXT = lv_color_hex(0xeaf0ff);
static const lv_color_t C_MUTED = lv_color_hex(0x9aa3b2);
static const lv_color_t C_DIM = lv_color_hex(0x445566);
static const lv_color_t C_GRAY = lv_color_hex(0x6a7280);

// ---------- FPS accounting ----------
static volatile uint32_t g_flush_cnt = 0;

// ---------- header ----------
static lv_obj_t *lbl_hostname;
static lv_obj_t *lbl_uptime;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_fps;
static lv_obj_t *btn_proc;

static char g_hostname_local[24];
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

// ---------- NET ----------
static lv_obj_t *chart_net;
static lv_chart_series_t *ser_rx;
static lv_chart_series_t *ser_tx;
static lv_obj_t *lbl_rx;
static lv_obj_t *lbl_tx;

// ---------- DISK ----------
static lv_obj_t *bar_disk;
static lv_obj_t *lbl_disk;

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

// views: what the overlay displays (DISK combines DISK_RD + DISK_WR slots)
enum { VIEW_CPU = 0, VIEW_RAM, VIEW_GPU, VIEW_DISK };
static int g_active_kind = VIEW_CPU;

// cards (tappable -> proc breakdown)
static lv_obj_t *card_cpu;
static lv_obj_t *card_gpu;
static lv_obj_t *card_ram;
static lv_obj_t *card_disk;
static lv_obj_t *card_llm;
static lv_obj_t *lbl_llm;
static uint8_t g_llm_status = 255;
static float g_llm_tps = 0.0f;
static char g_llm_model[25] = {0};

// ---------- LLM models list & overlay ----------
struct LlmModelItem {
    char id[17];
    uint8_t is_fav;
    uint8_t status;
};
static LlmModelItem g_llm_models[16];
static int g_llm_models_count = 0;

static lv_obj_t *ovl_llm;
static lv_obj_t *lbl_llm_ovl_status;
static lv_obj_t *cont_llm_models;
static lv_obj_t *btn_llm_models[16];
static lv_obj_t *lbl_llm_models[16];

static lv_style_t style_card;
static lv_style_t style_arc_bg;
static lv_style_t style_arc_indic;
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
#ifdef PROTO_DEBUG
    static uint32_t flush_cnt = 0;
    static uint32_t last_rep = 0;
    flush_cnt++;
    uint32_t now = millis();
    if (now - last_rep >= 1000) {
        Serial.printf("FLUSH %u area=%d,%u %dx%d\n", flush_cnt, a->x1, a->y1, w, h);
        last_rep = now;
    }
#endif
}

static void style_init()
{
    lv_style_set_bg_color(&style_card, C_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 18);
    lv_style_set_pad_all(&style_card, 12);
    lv_style_set_border_width(&style_card, 0);

    lv_style_set_arc_color(&style_arc_bg, C_ARC_BG);
    lv_style_set_arc_width(&style_arc_bg, 12);
    lv_style_set_arc_rounded(&style_arc_bg, true);

    lv_style_set_arc_color(&style_arc_indic, C_CYAN);
    lv_style_set_arc_width(&style_arc_indic, 12);
    lv_style_set_arc_rounded(&style_arc_indic, true);

    lv_style_set_text_color(&style_caption, C_MUTED);
    lv_style_set_text_font(&style_caption, &lv_font_montserrat_14);

    // tappable buttons: wide rounded target, subtle border, bright feedback on press
    lv_style_set_bg_color(&style_btn, C_CARD);
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_radius(&style_btn, 12);
    lv_style_set_border_width(&style_btn, 1);
    lv_style_set_border_color(&style_btn, C_ARC_BG);
    lv_style_set_pad_hor(&style_btn, 12);
    lv_style_set_text_color(&style_btn, C_MUTED);

    lv_style_set_bg_color(&style_btn_pr, C_CYAN);
    lv_style_set_border_color(&style_btn_pr, C_CYAN);
    lv_style_set_text_color(&style_btn_pr, C_BG);
    lv_style_set_translate_y(&style_btn_pr, 1);

    // red action button
    lv_style_init(&style_btn_red);
    lv_style_set_bg_color(&style_btn_red, lv_color_hex(0x381216));
    lv_style_set_border_color(&style_btn_red, lv_color_hex(0xef4444));
    lv_style_set_border_width(&style_btn_red, 1);
    lv_style_set_radius(&style_btn_red, 10);
    lv_style_set_text_color(&style_btn_red, lv_color_hex(0xef4444));

    // green action button
    lv_style_init(&style_btn_green);
    lv_style_set_bg_color(&style_btn_green, lv_color_hex(0x113322));
    lv_style_set_border_color(&style_btn_green, lv_color_hex(0x10b981));
    lv_style_set_border_width(&style_btn_green, 1);
    lv_style_set_radius(&style_btn_green, 10);
    lv_style_set_text_color(&style_btn_green, lv_color_hex(0x10b981));
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, const char *caption)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, caption);
    lv_obj_set_style_text_color(lbl, C_MUTED, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    return card;
}

static void set_label_font(lv_obj_t *lbl, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
}

// Cards are clickable by default (lv_obj base flag) and hold only display-only
// children, so the press lands on the card itself. The card's children must
// have LV_OBJ_FLAG_CLICKABLE cleared, otherwise a tap on an arc/bar/container
// would hit that child (no handler) instead of bubbling to the card.
static void card_tap_cb(lv_event_t *e);

static void make_card_tappable(lv_obj_t *card, int view)
{
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_tap_cb, LV_EVENT_PRESSED, (void *)(intptr_t)view);
}

// ---------- header ----------
static void fmt_uptime(char *buf, size_t sz, uint32_t sec)
{
    uint32_t d = sec / 86400;
    uint32_t h = (sec % 86400) / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    snprintf(buf, sz, "up %u %02u:%02u:%02u", d, h, m, s);
}

static void fmt_clock(char *buf, size_t sz, uint32_t epoch)
{
    uint32_t sec = (epoch + 3 * 3600) % 86400;
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    snprintf(buf, sz, "%02u:%02u:%02u", h, m, s);
}

// ---------- per-core mini bars ----------
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

// ---------- PROC overlay ----------
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
    lv_obj_add_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);
}

static void ovl_open_view(int view)
{
    static const char *titles[] = {"TOP CPU", "TOP RAM", "TOP GPU", "TOP DISK (KiB/s)"};
    if (view > VIEW_DISK) view = VIEW_CPU;
    g_active_kind = view;
    lv_label_set_text(lbl_ovl_title, titles[view]);
    rebuild_proc_rows();
    lv_obj_clear_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);
#ifdef PROTO_DEBUG
    Serial.printf("ovl open view=%d\n", view);
#endif
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

// ---------- LLM overlay helpers ----------
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

static void ovl_llm_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (ovl_llm && lv_obj_has_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN))
        ovl_open_llm();
    else
        ovl_close_llm();
}

static void ovl_llm_close_cb(lv_event_t *e)
{
    (void)e;
    ovl_close_llm();
}

static void btn_stop_all_cb(lv_event_t *e)
{
    (void)e;
    protocol_send_cmd(CMD_STOP_ALL);
}

static void btn_start_fav_cb(lv_event_t *e)
{
    (void)e;
    protocol_send_cmd(CMD_START_FAVORITE);
}

static void model_btn_tap_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < g_llm_models_count) {
        if (g_llm_models[idx].status == LLM_STATUS_RUNNING || g_llm_models[idx].status == LLM_STATUS_STARTING) {
            protocol_send_cmd(CMD_STOP_ALL);
        } else {
            protocol_send_start_model(g_llm_models[idx].id);
        }
    }
}

static void card_tap_cb(lv_event_t *e)
{
    int view = (int)(intptr_t)lv_event_get_user_data(e);
#ifdef PROTO_DEBUG
    Serial.printf("card tap view=%d\n", view);
#endif
    ovl_open_view(view);
}

static void slider_bright_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    ledcWrite(0, (int)lv_slider_get_value(sl));
}

// ---------- periodic refresh (clock / uptime / FPS) ----------
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

    static uint32_t last_flush = 0;
    uint32_t fps = (g_flush_cnt - last_flush) * 2;  // timer runs @500ms -> x2 for per-second
    last_flush = g_flush_cnt;
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "FPS %u", fps);
    lv_label_set_text(lbl_fps, fbuf);
}

// ---------- touch indev ----------
// touch_read() returns the debounced cached state filled by touch_poll() in the
// main loop, so the LVGL timer never blocks on I2C/GT911.
static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int16_t x, y;
    if (touch_read(x, y)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;
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

    // ---------------- header ----------------
    lbl_hostname = lv_label_create(scr);
    lv_label_set_text(lbl_hostname, "--");
    set_label_font(lbl_hostname, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lbl_hostname, LV_ALIGN_TOP_LEFT, 14, 7);

    lbl_uptime = lv_label_create(scr);
    lv_label_set_text(lbl_uptime, "up --:--:--");
    set_label_font(lbl_uptime, &lv_font_montserrat_14, C_DIM);
    lv_obj_align(lbl_uptime, LV_ALIGN_TOP_LEFT, 14, 26);

    lbl_clock = lv_label_create(scr);
    lv_label_set_text(lbl_clock, "--:--:--");
    set_label_font(lbl_clock, &lv_font_montserrat_16, C_TEXT);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -112, 5);

    lbl_fps = lv_label_create(scr);
    lv_label_set_text(lbl_fps, "FPS --");
    set_label_font(lbl_fps, &lv_font_montserrat_14, C_DIM);
    lv_obj_align(lbl_fps, LV_ALIGN_TOP_RIGHT, -112, 26);

    btn_proc = lv_btn_create(scr);
    lv_obj_set_size(btn_proc, 96, 38);
    lv_obj_align(btn_proc, LV_ALIGN_TOP_RIGHT, -6, 5);
    lv_obj_add_style(btn_proc, &style_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_proc, &style_btn_pr, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_proc, ovl_llm_toggle_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(btn_proc);
    lv_label_set_text(btn_lbl, "LLM");
    set_label_font(btn_lbl, &lv_font_montserrat_14, C_CYAN);
    lv_obj_center(btn_lbl);

    // header hostname/left area toggles the proc overlay
    lv_obj_add_event_cb(lbl_hostname, ovl_toggle_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lbl_uptime, ovl_toggle_cb, LV_EVENT_PRESSED, NULL);

    // ---------------- 2x2 grid ----------------
    int top = 52;
    int margin = 10;
    int gap = 10;
    int card_w = (w - 2 * margin - gap) / 2;
    int grid_bottom = 480 - 62;
    int card_h = (grid_bottom - top - gap) / 2;

    // CPU card
    card_cpu = make_card(scr, margin, top, card_w, card_h, "CPU");
    lv_obj_t *cpu = card_cpu;
    arc_cpu = lv_arc_create(cpu);
    lv_obj_set_size(arc_cpu, 120, 120);
    lv_arc_set_range(arc_cpu, 0, 100);
    lv_arc_set_value(arc_cpu, 0);
    lv_obj_clear_flag(arc_cpu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(arc_cpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_cpu, &style_arc_indic, LV_PART_INDICATOR);
    lv_obj_align(arc_cpu, LV_ALIGN_CENTER, 0, -16);

    lbl_cpu_pct = lv_label_create(arc_cpu);
    lv_label_set_text(lbl_cpu_pct, "0%");
    set_label_font(lbl_cpu_pct, &lv_font_montserrat_16, C_CYAN);
    lv_obj_center(lbl_cpu_pct);

    cores_cont = lv_obj_create(cpu);
    lv_obj_set_size(cores_cont, card_w - 24, 34);
    lv_obj_set_style_bg_opa(cores_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cores_cont, 0, 0);
    lv_obj_set_style_pad_all(cores_cont, 0, 0);
    lv_obj_set_layout(cores_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cores_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cores_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cores_cont, 2, 0);
    lv_obj_align(cores_cont, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(cores_cont, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 32; i++) {
        bar_cores[i] = lv_bar_create(cores_cont);
        lv_obj_set_size(bar_cores[i], 5, 30);
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

    // GPU card
    card_gpu = make_card(scr, margin + card_w + gap, top, card_w, card_h, "GPU");
    lv_obj_t *gpu = card_gpu;
    arc_gpu = lv_arc_create(gpu);
    lv_obj_set_size(arc_gpu, 120, 120);
    lv_arc_set_range(arc_gpu, 0, 100);
    lv_arc_set_value(arc_gpu, 0);
    lv_obj_clear_flag(arc_gpu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(arc_gpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_gpu, &style_arc_indic, LV_PART_INDICATOR);
    lv_obj_align(arc_gpu, LV_ALIGN_CENTER, 0, -18);

    lbl_gpu_pct = lv_label_create(arc_gpu);
    lv_label_set_text(lbl_gpu_pct, "0%");
    set_label_font(lbl_gpu_pct, &lv_font_montserrat_16, C_CYAN);
    lv_obj_center(lbl_gpu_pct);

    bar_vram = lv_bar_create(gpu);
    lv_obj_set_size(bar_vram, card_w - 40, 12);
    lv_obj_align(bar_vram, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_bar_set_range(bar_vram, 0, 100);
    lv_bar_set_value(bar_vram, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar_vram, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_vram, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vram, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vram, 6, 0);

    lbl_vram = lv_label_create(gpu);
    lv_label_set_text(lbl_vram, "0 / 0 MB");
    set_label_font(lbl_vram, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_vram, LV_ALIGN_BOTTOM_MID, 0, 0);
    make_card_tappable(card_gpu, VIEW_GPU);

    // RAM card
    card_ram = make_card(scr, margin, top + card_h + gap, card_w, card_h, "RAM");
    lv_obj_t *ram = card_ram;
    bar_ram = lv_bar_create(ram);
    lv_obj_set_size(bar_ram, card_w - 30, 36);
    lv_obj_align(bar_ram, LV_ALIGN_CENTER, 0, -14);
    lv_bar_set_range(bar_ram, 0, 100);
    lv_bar_set_value(bar_ram, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar_ram, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_ram, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_ram, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_ram, 8, 0);

    lbl_ram = lv_label_create(ram);
    lv_label_set_text(lbl_ram, "0 / 0 MB");
    set_label_font(lbl_ram, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(lbl_ram, LV_ALIGN_BOTTOM_MID, 0, -2);
    make_card_tappable(card_ram, VIEW_RAM);

    // NET card
    lv_obj_t *net = make_card(scr, margin + card_w + gap, top + card_h + gap, card_w, card_h, "NET");
    chart_net = lv_chart_create(net);
    lv_obj_set_size(chart_net, card_w - 30, card_h - 78);
    lv_obj_align(chart_net, LV_ALIGN_CENTER, 0, 10);
    lv_chart_set_type(chart_net, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart_net, LV_CHART_AXIS_PRIMARY_Y, 0, 512);
    lv_chart_set_point_count(chart_net, 60);
    lv_chart_set_update_mode(chart_net, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart_net, 0, 4);
    lv_obj_set_style_bg_color(chart_net, C_CARD, 0);
    ser_rx = lv_chart_add_series(chart_net, C_CYAN, LV_CHART_AXIS_PRIMARY_Y);
    ser_tx = lv_chart_add_series(chart_net, C_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_color(chart_net, C_DIM, LV_PART_MAIN);

    lbl_rx = lv_label_create(net);
    lv_label_set_text(lbl_rx, "RX --");
    set_label_font(lbl_rx, &lv_font_montserrat_12, C_CYAN);
    lv_obj_align(lbl_rx, LV_ALIGN_TOP_LEFT, 6, 24);

    lbl_tx = lv_label_create(net);
    lv_label_set_text(lbl_tx, "TX --");
    set_label_font(lbl_tx, &lv_font_montserrat_12, C_GREEN);
    lv_obj_align(lbl_tx, LV_ALIGN_TOP_RIGHT, -6, 24);

    // ---------------- bottom strip: DISK & LLM ----------------
    card_disk = make_card(scr, margin, 480 - 56, card_w, 46, "DISK");
    lv_obj_t *disk = card_disk;
    lv_obj_set_style_radius(disk, 12, 0);

    bar_disk = lv_bar_create(disk);
    lv_obj_set_size(bar_disk, card_w - 30, 8);
    lv_obj_align(bar_disk, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_bar_set_range(bar_disk, 0, 100);
    lv_bar_set_value(bar_disk, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(bar_disk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bar_disk, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_disk, C_AMBER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_disk, 4, 0);

    lbl_disk = lv_label_create(disk);
    lv_label_set_text(lbl_disk, "0.0 / 0.0 MB/s");
    set_label_font(lbl_disk, &lv_font_montserrat_12, C_TEXT);
    lv_obj_align(lbl_disk, LV_ALIGN_TOP_MID, 0, 14);
    make_card_tappable(card_disk, VIEW_DISK);

    // LLM card (tappable: stop if active, start favorite if idle)
    card_llm = make_card(scr, margin + card_w + gap, 480 - 56, card_w, 46, "LLM");
    lv_obj_set_style_radius(card_llm, 12, 0);

    lbl_llm = lv_label_create(card_llm);
    lv_label_set_text(lbl_llm, "IDLE (Tap: Fav)");
    set_label_font(lbl_llm, &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(lbl_llm, LV_ALIGN_CENTER, 0, 4);

    lv_obj_add_event_cb(card_llm, [](lv_event_t *e) {
        ovl_open_llm();
    }, LV_EVENT_PRESSED, NULL);

    // ---------------- LLM overlay ----------------
    ovl_llm = lv_obj_create(scr);
    lv_obj_set_size(ovl_llm, w, h);
    lv_obj_set_pos(ovl_llm, 0, 0);
    lv_obj_set_style_bg_color(ovl_llm, lv_color_hex(0x0a0d12), 0);
    lv_obj_set_style_radius(ovl_llm, 0, 0);
    lv_obj_set_style_border_width(ovl_llm, 0, 0);
    lv_obj_clear_flag(ovl_llm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_llm_title = lv_label_create(ovl_llm);
    lv_label_set_text(lbl_llm_title, "LLM CONTROL");
    set_label_font(lbl_llm_title, &lv_font_montserrat_16, C_CYAN);
    lv_obj_align(lbl_llm_title, LV_ALIGN_TOP_LEFT, 18, 14);

    lv_obj_t *btn_llm_close = lv_btn_create(ovl_llm);
    lv_obj_set_size(btn_llm_close, 90, 36);
    lv_obj_align(btn_llm_close, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_style(btn_llm_close, &style_btn, LV_PART_MAIN);
    lv_obj_add_style(btn_llm_close, &style_btn_pr, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_llm_close, ovl_llm_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_close = lv_label_create(btn_llm_close);
    lv_label_set_text(lbl_close, "CLOSE");
    set_label_font(lbl_close, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(lbl_close);

    // Stop All button
    lv_obj_t *btn_stop = lv_btn_create(ovl_llm);
    lv_obj_set_size(btn_stop, 130, 36);
    lv_obj_align(btn_stop, LV_ALIGN_TOP_LEFT, 18, 50);
    lv_obj_add_style(btn_stop, &style_btn_red, 0);
    lv_obj_add_event_cb(btn_stop, btn_stop_all_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_s = lv_label_create(btn_stop);
    lv_label_set_text(lbl_s, "STOP ALL");
    set_label_font(lbl_s, &lv_font_montserrat_14, lv_color_hex(0xef4444));
    lv_obj_center(lbl_s);

    // Start Favorite button
    lv_obj_t *btn_fav = lv_btn_create(ovl_llm);
    lv_obj_set_size(btn_fav, 130, 36);
    lv_obj_align(btn_fav, LV_ALIGN_TOP_LEFT, 158, 50);
    lv_obj_add_style(btn_fav, &style_btn_green, 0);
    lv_obj_add_event_cb(btn_fav, btn_start_fav_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl_f = lv_label_create(btn_fav);
    lv_label_set_text(lbl_f, "START FAV");
    set_label_font(lbl_f, &lv_font_montserrat_14, lv_color_hex(0x10b981));
    lv_obj_center(lbl_f);

    // Status line
    lbl_llm_ovl_status = lv_label_create(ovl_llm);
    lv_label_set_text(lbl_llm_ovl_status, "Status: IDLE");
    set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(lbl_llm_ovl_status, LV_ALIGN_TOP_LEFT, 18, 94);

    // Models container (scrollable flex grid)
    cont_llm_models = lv_obj_create(ovl_llm);
    lv_obj_set_size(cont_llm_models, 444, 290);
    lv_obj_align(cont_llm_models, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_opa(cont_llm_models, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont_llm_models, 0, 0);
    lv_obj_set_style_pad_all(cont_llm_models, 2, 0);
    lv_obj_set_style_pad_row(cont_llm_models, 6, 0);
    lv_obj_set_style_pad_column(cont_llm_models, 8, 0);
    lv_obj_set_layout(cont_llm_models, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont_llm_models, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cont_llm_models, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (int i = 0; i < 16; i++) {
        btn_llm_models[i] = lv_btn_create(cont_llm_models);
        lv_obj_set_size(btn_llm_models[i], 210, 42);
        lv_obj_add_style(btn_llm_models[i], &style_btn, LV_PART_MAIN);
        lv_obj_add_style(btn_llm_models[i], &style_btn_pr, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn_llm_models[i], model_btn_tap_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lbl_llm_models[i] = lv_label_create(btn_llm_models[i]);
        lv_label_set_text(lbl_llm_models[i], "");
        set_label_font(lbl_llm_models[i], &lv_font_montserrat_14, C_TEXT);
        lv_obj_center(lbl_llm_models[i]);
        lv_obj_add_flag(btn_llm_models[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Brightness slider at bottom of LLM overlay
    lv_obj_t *ovl_llm_brlbl = lv_label_create(ovl_llm);
    lv_label_set_text(ovl_llm_brlbl, "BRIGHTNESS");
    set_label_font(ovl_llm_brlbl, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(ovl_llm_brlbl, LV_ALIGN_BOTTOM_LEFT, 18, -14);

    lv_obj_t *ovl_llm_slider = lv_slider_create(ovl_llm);
    lv_obj_set_size(ovl_llm_slider, w - 160, 14);
    lv_obj_align(ovl_llm_slider, LV_ALIGN_BOTTOM_LEFT, 138, -14);
    lv_slider_set_range(ovl_llm_slider, 0, 255);
    lv_slider_set_value(ovl_llm_slider, 150, LV_ANIM_OFF);
    lv_obj_add_event_cb(ovl_llm_slider, slider_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ovl_llm_slider, C_TEXT, LV_PART_KNOB);

    // ---------------- PROC overlay ----------------
    ovl_proc = lv_obj_create(scr);
    lv_obj_set_size(ovl_proc, w, h);
    lv_obj_set_pos(ovl_proc, 0, 0);
    lv_obj_set_style_bg_color(ovl_proc, lv_color_hex(0x0a0d12), 0);
    lv_obj_set_style_radius(ovl_proc, 0, 0);
    lv_obj_set_style_border_width(ovl_proc, 0, 0);
    lv_obj_clear_flag(ovl_proc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_proc, LV_OBJ_FLAG_HIDDEN);

    lbl_ovl_title = lv_label_create(ovl_proc);
    lv_label_set_text(lbl_ovl_title, "TOP CPU");
    set_label_font(lbl_ovl_title, &lv_font_montserrat_16, C_TEXT);
    lv_obj_align(lbl_ovl_title, LV_ALIGN_TOP_MID, 0, 12);

    for (int i = 0; i < 10; i++) {
        proc_rows[i] = lv_label_create(ovl_proc);
        lv_label_set_text(proc_rows[i], "");
        set_label_font(proc_rows[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(proc_rows[i], LV_ALIGN_TOP_LEFT, 18, 48 + i * 26);
    }

    lv_obj_t *ovl_brlbl = lv_label_create(ovl_proc);
    lv_label_set_text(ovl_brlbl, "BRIGHTNESS");
    set_label_font(ovl_brlbl, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(ovl_brlbl, LV_ALIGN_BOTTOM_LEFT, 18, -16);

    lv_obj_t *ovl_slider = lv_slider_create(ovl_proc);
    lv_obj_set_size(ovl_slider, w - 90, 14);
    lv_obj_align(ovl_slider, LV_ALIGN_BOTTOM_LEFT, 128, -16);
    lv_slider_set_range(ovl_slider, 0, 255);
    lv_slider_set_value(ovl_slider, 150, LV_ANIM_OFF);
    lv_obj_add_event_cb(ovl_slider, slider_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(ovl_slider, C_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ovl_slider, C_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ovl_slider, C_TEXT, LV_PART_KNOB);

    lv_obj_t *ovl_close = lv_btn_create(ovl_proc);
    lv_obj_set_size(ovl_close, 96, 38);
    lv_obj_align(ovl_close, LV_ALIGN_TOP_RIGHT, -6, 8);
    lv_obj_add_style(ovl_close, &style_btn, LV_PART_MAIN);
    lv_obj_add_style(ovl_close, &style_btn_pr, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ovl_close, ovl_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *close_lbl = lv_label_create(ovl_close);
    lv_label_set_text(close_lbl, "CLOSE");
    set_label_font(close_lbl, &lv_font_montserrat_14, C_MUTED);
    lv_obj_center(close_lbl);

    // ---------------- touch indev ----------------
    touch_init();  // (re)initializes the touch chip so this TU's static instance is ready
    static lv_indev_drv_t indev_drv;  // must outlive ui_init: lv_indev_drv_register keeps a pointer
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
    lv_arc_set_value(arc_cpu, pct);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_cpu_pct, buf);
    sync_cores();
}

void ui_set_gpu_pct(int pct, int vram_pct, uint32_t vram_used_mb, uint32_t vram_total_mb)
{
    if (pct == 255) {
        lv_arc_set_value(arc_gpu, 0);
        lv_obj_set_style_arc_color(arc_gpu, C_GRAY, LV_PART_INDICATOR);
        lv_label_set_text(lbl_gpu_pct, "--");
        lv_obj_set_style_text_color(lbl_gpu_pct, C_MUTED, 0);
    } else {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_arc_set_value(arc_gpu, pct);
        lv_obj_set_style_arc_color(arc_gpu, C_CYAN, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_gpu_pct, C_CYAN, 0);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(lbl_gpu_pct, buf);
    }

    if (vram_pct < 0) vram_pct = 0;
    if (vram_pct > 100) vram_pct = 100;
    lv_bar_set_value(bar_vram, vram_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_vram, vram_pct > 70 ? C_AMBER : C_GREEN, LV_PART_INDICATOR);
    char buf[32];
    snprintf(buf, sizeof(buf), "VRAM %u / %u MB", vram_used_mb, vram_total_mb);
    lv_label_set_text(lbl_vram, buf);
}

void ui_set_ram_pct(int pct, uint32_t used_mb, uint32_t total_mb)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bar_ram, pct, LV_ANIM_OFF);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u / %u MB", used_mb, total_mb);
    lv_label_set_text(lbl_ram, buf);
}

void ui_set_net(uint32_t rx_kibs, uint32_t tx_kibs)
{
    if (rx_kibs > 512) rx_kibs = 512;
    if (tx_kibs > 512) tx_kibs = 512;
    lv_chart_set_next_value(chart_net, ser_rx, rx_kibs);
    lv_chart_set_next_value(chart_net, ser_tx, tx_kibs);
    char buf[16];
    snprintf(buf, sizeof(buf), "RX %u", rx_kibs);
    lv_label_set_text(lbl_rx, buf);
    snprintf(buf, sizeof(buf), "TX %u", tx_kibs);
    lv_label_set_text(lbl_tx, buf);
}

void ui_set_disk(uint32_t rd_kibs, uint32_t wr_kibs, int used_pct)
{
    if (used_pct < 0) used_pct = 0;
    if (used_pct > 100) used_pct = 100;
    lv_bar_set_value(bar_disk, used_pct, LV_ANIM_OFF);
    float rd = rd_kibs / 1024.0f;
    float wr = wr_kibs / 1024.0f;
    char buf[40];
    snprintf(buf, sizeof(buf), "rd %.1f / wr %.1f MB/s", rd, wr);
    lv_label_set_text(lbl_disk, buf);
}

void ui_set_header(uint32_t uptime_sec, uint32_t epoch_sec, const char *hostname)
{
    if (!g_hdr_init) {
        g_hdr_init = true;
        g_boot_ms = millis();
        g_wallclock_epoch = epoch_sec;
        g_uptime_base = uptime_sec;
    }
    strncpy(g_hostname_local, hostname, sizeof(g_hostname_local) - 1);
    g_hostname_local[sizeof(g_hostname_local) - 1] = '\0';
    lv_label_set_text(lbl_hostname, g_hostname_local);

    // uptime/clock are rendered only by ui_periodic from the local monotonic
    // counter. Writing the raw host snapshot here too would fight with it:
    // the two values differ by the serial transport delay and the labels
    // would flicker a second back and forth each frame.
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

void ui_set_llm(uint8_t status, float tps, const char *model)
{
    g_llm_status = status;
    g_llm_tps = tps;
    if (model) {
        strncpy(g_llm_model, model, sizeof(g_llm_model) - 1);
        g_llm_model[sizeof(g_llm_model) - 1] = '\0';
    } else {
        g_llm_model[0] = '\0';
    }

    if (!lbl_llm) return;

    char buf[40];
    if (status == LLM_STATUS_RUNNING) {
        if (tps > 0.0f) {
            snprintf(buf, sizeof(buf), "%s (%.1f t/s)", g_llm_model, tps);
        } else {
            snprintf(buf, sizeof(buf), "%s [ON]", g_llm_model);
        }
        set_label_font(lbl_llm, &lv_font_montserrat_12, C_GREEN);
    } else if (status == LLM_STATUS_STARTING) {
        snprintf(buf, sizeof(buf), "%s...", g_llm_model);
        set_label_font(lbl_llm, &lv_font_montserrat_12, C_AMBER);
    } else if (status == LLM_STATUS_IDLE) {
        snprintf(buf, sizeof(buf), "IDLE (Tap: Fav)");
        set_label_font(lbl_llm, &lv_font_montserrat_12, C_MUTED);
    } else {
        snprintf(buf, sizeof(buf), "Offline");
        set_label_font(lbl_llm, &lv_font_montserrat_12, C_DIM);
    }
    lv_label_set_text(lbl_llm, buf);

    if (ovl_llm && !lv_obj_has_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_llm_model_buttons();
    }
}

static void rebuild_llm_model_buttons()
{
    if (!cont_llm_models || !lbl_llm_ovl_status) return;

    char st_buf[64];
    if (g_llm_status == LLM_STATUS_RUNNING) {
        if (g_llm_tps > 0.0f) {
            snprintf(st_buf, sizeof(st_buf), "Active: %s (%.1f tok/s)", g_llm_model, g_llm_tps);
        } else {
            snprintf(st_buf, sizeof(st_buf), "Active: %s [RUNNING]", g_llm_model);
        }
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_GREEN);
    } else if (g_llm_status == LLM_STATUS_STARTING) {
        snprintf(st_buf, sizeof(st_buf), "Loading: %s...", g_llm_model);
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_AMBER);
    } else if (g_llm_status == LLM_STATUS_IDLE) {
        snprintf(st_buf, sizeof(st_buf), "Status: IDLE (Tap model to start)");
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_MUTED);
    } else {
        snprintf(st_buf, sizeof(st_buf), "Status: Offline (llmcontrol disconnected)");
        set_label_font(lbl_llm_ovl_status, &lv_font_montserrat_14, C_DIM);
    }
    lv_label_set_text(lbl_llm_ovl_status, st_buf);

    for (int i = 0; i < 16; i++) {
        if (i < g_llm_models_count) {
            lv_obj_clear_flag(btn_llm_models[i], LV_OBJ_FLAG_HIDDEN);
            char btn_text[32];
            const char *prefix = g_llm_models[i].is_fav ? "*" : "";
            if (g_llm_models[i].status == LLM_STATUS_RUNNING) {
                snprintf(btn_text, sizeof(btn_text), "> %s%s", prefix, g_llm_models[i].id);
                lv_obj_set_style_border_color(btn_llm_models[i], C_GREEN, 0);
                lv_obj_set_style_border_width(btn_llm_models[i], 2, 0);
                set_label_font(lbl_llm_models[i], &lv_font_montserrat_14, C_GREEN);
            } else if (g_llm_models[i].status == LLM_STATUS_STARTING) {
                snprintf(btn_text, sizeof(btn_text), "... %s%s", prefix, g_llm_models[i].id);
                lv_obj_set_style_border_color(btn_llm_models[i], C_AMBER, 0);
                lv_obj_set_style_border_width(btn_llm_models[i], 2, 0);
                set_label_font(lbl_llm_models[i], &lv_font_montserrat_14, C_AMBER);
            } else {
                snprintf(btn_text, sizeof(btn_text), "%s%s", prefix, g_llm_models[i].id);
                lv_obj_set_style_border_color(btn_llm_models[i], C_ARC_BG, 0);
                lv_obj_set_style_border_width(btn_llm_models[i], 1, 0);
                set_label_font(lbl_llm_models[i], &lv_font_montserrat_14, g_llm_models[i].is_fav ? C_AMBER : C_TEXT);
            }
            lv_label_set_text(lbl_llm_models[i], btn_text);
        } else {
            lv_obj_add_flag(btn_llm_models[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_set_llm_models(const uint8_t *data, uint16_t len)
{
    if (!data || len < 1) return;
    uint8_t count = data[0];
    int avail = (len - 1) / 18;
    if (count > avail) count = avail;
    if (count > 16) count = 16;
    g_llm_models_count = count;

    const uint8_t *p = data + 1;
    for (int i = 0; i < count; i++) {
        memcpy(g_llm_models[i].id, p, 16);
        g_llm_models[i].id[15] = '\0';
        g_llm_models[i].is_fav = p[16];
        g_llm_models[i].status = p[17];
        p += 18;
    }

    if (ovl_llm && !lv_obj_has_flag(ovl_llm, LV_OBJ_FLAG_HIDDEN)) {
        rebuild_llm_model_buttons();
    }
}

