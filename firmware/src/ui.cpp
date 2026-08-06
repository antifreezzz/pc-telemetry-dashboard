#include "ui.h"
#include "panel.h"

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *buf1;

static lv_obj_t *arc_cpu;
static lv_obj_t *arc_gpu;
static lv_obj_t *bar_ram;
static lv_obj_t *lbl_ram;
static lv_obj_t *chart_net;
static lv_obj_t *lbl_fps;

static void my_disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c)
{
    int w = a->x2 - a->x1 + 1;
    int h = a->y2 - a->y1 + 1;
    gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&c->full, w, h);
    lv_disp_flush_ready(d);
}

static lv_style_t style_card;
static lv_style_t style_arc_bg;
static lv_style_t style_arc_indic;
static lv_style_t style_caption;

static void style_init()
{
    lv_style_set_bg_color(&style_card, lv_color_hex(0x14171c));
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 18);
    lv_style_set_pad_all(&style_card, 12);
    lv_style_set_border_width(&style_card, 0);

    lv_style_set_arc_color(&style_arc_bg, lv_color_hex(0x2a2f38));
    lv_style_set_arc_width(&style_arc_bg, 12);
    lv_style_set_arc_rounded(&style_arc_bg, true);

    lv_style_set_arc_color(&style_arc_indic, lv_color_hex(0x00eaff));
    lv_style_set_arc_width(&style_arc_indic, 12);
    lv_style_set_arc_rounded(&style_arc_indic, true);

    lv_style_set_text_color(&style_caption, lv_color_hex(0x9aa3b2));
    lv_style_set_text_font(&style_caption, &lv_font_montserrat_14);
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
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x9aa3b2), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    return card;
}

void ui_init(int w, int h)
{
    lv_init();
    buf1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * w * 200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, w * 200);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = w;
    disp_drv.ver_res = h;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    style_init();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // 2x2 grid of cards
    int card_w = (w - 30) / 2;
    int card_h = (h - 30) / 2;

    // CPU card
    lv_obj_t *cpu = make_card(scr, 10, 10, card_w, card_h, "CPU");
    arc_cpu = lv_arc_create(cpu);
    lv_obj_set_size(arc_cpu, card_w - 70, card_w - 70);
    lv_arc_set_range(arc_cpu, 0, 100);
    lv_arc_set_value(arc_cpu, 0);
    lv_obj_add_style(arc_cpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_cpu, &style_arc_indic, LV_PART_INDICATOR);
    lv_obj_center(arc_cpu);

    // GPU card
    lv_obj_t *gpu = make_card(scr, 20 + card_w, 10, card_w, card_h, "GPU");
    arc_gpu = lv_arc_create(gpu);
    lv_obj_set_size(arc_gpu, card_w - 70, card_w - 70);
    lv_arc_set_range(arc_gpu, 0, 100);
    lv_arc_set_value(arc_gpu, 0);
    lv_obj_add_style(arc_gpu, &style_arc_bg, LV_PART_MAIN);
    lv_obj_add_style(arc_gpu, &style_arc_indic, LV_PART_INDICATOR);
    lv_obj_center(arc_gpu);

    // RAM card
    lv_obj_t *ram = make_card(scr, 10, 20 + card_h, card_w, card_h, "RAM");
    bar_ram = lv_bar_create(ram);
    lv_obj_set_size(bar_ram, card_w - 30, 36);
    lv_obj_align(bar_ram, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(bar_ram, 0, 100);
    lv_bar_set_value(bar_ram, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_ram, lv_color_hex(0x2a2f38), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_ram, lv_color_hex(0x00ff9c), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_ram, 8, 0);

    lbl_ram = lv_label_create(ram);
    lv_obj_set_style_text_color(lbl_ram, lv_color_hex(0xeaf0ff), 0);
    lv_obj_set_style_text_font(lbl_ram, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_ram, "0 / 0 MB");
    lv_obj_align(lbl_ram, LV_ALIGN_BOTTOM_MID, 0, 0);

    // NET card
    lv_obj_t *net = make_card(scr, 20 + card_w, 20 + card_h, card_w, card_h, "NET");
    chart_net = lv_chart_create(net);
    lv_obj_set_size(chart_net, card_w - 30, card_h - 70);
    lv_obj_align(chart_net, LV_ALIGN_CENTER, 0, -10);
    lv_chart_set_type(chart_net, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart_net, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(chart_net, 60);
    lv_chart_set_update_mode(chart_net, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_add_series(chart_net, lv_color_hex(0x00eaff), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_bg_color(chart_net, lv_color_hex(0x14171c), 0);
    lv_obj_set_style_line_color(chart_net, lv_color_hex(0x00eaff), LV_PART_ITEMS);

    lbl_fps = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_fps, lv_color_hex(0x445566), 0);
    lv_obj_set_style_text_font(lbl_fps, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_fps, "FPS --");
    lv_obj_align(lbl_fps, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
}

void ui_set_cpu_pct(int pct) { lv_arc_set_value(arc_cpu, pct); }
void ui_set_gpu_pct(int pct) { lv_arc_set_value(arc_gpu, pct); }

void ui_set_ram_pct(int pct, int used_mb, int total_mb)
{
    lv_bar_set_value(bar_ram, pct, LV_ANIM_OFF);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d MB", used_mb, total_mb);
    lv_label_set_text(lbl_ram, buf);
}

void ui_set_net(uint32_t rx_kbps, uint32_t tx_kbps)
{
    uint32_t v = rx_kbps > tx_kbps ? rx_kbps : tx_kbps;
    if (v > 100) v = 100;
    lv_chart_set_next_value(chart_net, lv_chart_get_series_next(chart_net, NULL), v);
}
