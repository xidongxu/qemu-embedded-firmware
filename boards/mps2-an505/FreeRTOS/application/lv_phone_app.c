/*
 * lv_phone_app.c - LVGL phone application (dial pad + answer/hangup UI).
 *
 * Built on the project's LVGL demo infra (lv_disp.c provides the display,
 * the touch input device and the LVGL task).  This app talks to the standard
 * phone API in pj_phone.h:
 *
 *   dial (number) / answer / reject / hangup  +  state query / notification.
 *
 * Layout (450x450):
 *   [ status ]        registration + call state
 *   [ number ]        dialed digits (big, center)
 *   [ 1 2 3 ]         dial pad (3x4)
 *   [ 4 5 6 ]
 *   [ 7 8 9 ]
 *   [ * 0 # ]
 *   [ A ][ B ][ C ]   context actions
 *       idle:      清空 | 删除 | 拨号
 *       incoming:  拒接 |  -  | 接听
 *       active:     -   |  -  | 挂断
 *
 * Threading: pj_phone callbacks run on the pjsua worker thread; they only set
 * s_dirty.  All LVGL calls happen in this task's update()/event handlers.
 */
#include <stdint.h>
#include <string.h>
#include "lvgl.h"
#include "lcd.h"
#include "printf.h"
#include "pj_phone.h"
#include "lv_phone_app.h"

#define W LCD_WIDTH_PIXELS
#define H LCD_HEIGHT_PIXELS

/* ---- UI state --------------------------------------------------- */
static char s_number[32] = "";          /* dialed digits */
static lv_obj_t *s_lbl_status;          /* registration line */
static lv_obj_t *s_lbl_state;           /* call state line   */
static lv_obj_t *s_lbl_number;          /* dialed digits     */
static lv_obj_t *s_btn_a, *s_btn_b, *s_btn_c;   /* action slots */
static lv_obj_t *s_lbl_a, *s_lbl_b, *s_lbl_c;

static volatile int s_dirty = 1;        /* set by the phone notify callback */

/* ---- helpers ---------------------------------------------------- */
static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_color_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_20, 0);
    return b;
}

static lv_obj_t *button_label(lv_obj_t *btn, const char *txt)
{
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return l;
}

/* ---- input handlers --------------------------------------------- */
static void key_event_cb(lv_event_t *e)
{
    const char *d = (const char *)lv_event_get_user_data(e);
    size_t n = strlen(s_number);
    if (d && n + 1 < sizeof(s_number)) {
        s_number[n] = d[0];
        s_number[n + 1] = '\0';
        lv_label_set_text(s_lbl_number, s_number);
    }
}

static void action_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    pj_phone_call_state_t st = pj_phone_get_call_state();

    /* Right slot [C]: 拨号 / 接听 / 挂断  depending on the call state. */
    if (btn == s_btn_c) {
        if (st == PJ_PHONE_CALL_INCOMING) {
            pj_phone_answer();
        } else if (st == PJ_PHONE_CALL_DIALING ||
                   st == PJ_PHONE_CALL_ACTIVE) {
            pj_phone_hangup();
        } else if (s_number[0]) {
            pj_phone_dial(s_number);
        }
        return;
    }
    /* Left slot [A]: 拒接 (incoming) or 清空 (idle). */
    if (btn == s_btn_a) {
        if (st == PJ_PHONE_CALL_INCOMING) {
            pj_phone_reject();
        } else {
            s_number[0] = '\0';
            lv_label_set_text(s_lbl_number, "Enter number");
        }
        return;
    }
    /* Middle slot [B]: backspace (only shown in idle). */
    if (btn == s_btn_b) {
        size_t n = strlen(s_number);
        if (n) {
            s_number[n - 1] = '\0';
            lv_label_set_text(s_lbl_number, s_number);
        }
    }
}

/* ---- UI refresh ------------------------------------------------- */
static void show_action(lv_obj_t *btn, lv_obj_t *lbl, int visible,
                        const char *txt, lv_color_t color)
{
    if (!visible) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_bg_color(btn, color, 0);
}

static void update_ui(void)
{
    pj_phone_reg_state_t  reg  = pj_phone_get_reg_state();
    pj_phone_call_state_t call = pj_phone_get_call_state();
    const char *peer = pj_phone_get_peer_number();
    char buf[64];

    /* registration line (ASCII only - the bundled fonts have no CJK glyphs) */
    switch (reg) {
    case PJ_PHONE_REG_REGISTERED:  snprintf(buf, sizeof(buf), "1000 [REG]");        break;
    case PJ_PHONE_REG_REGISTERING: snprintf(buf, sizeof(buf), "1000 [REGISTERING]"); break;
    case PJ_PHONE_REG_FAILED:      snprintf(buf, sizeof(buf), "1000 [REG FAIL]");    break;
    default:                       snprintf(buf, sizeof(buf), "1000 [OFFLINE]");     break;
    }
    lv_label_set_text(s_lbl_status, buf);

    /* call state line */
    switch (call) {
    case PJ_PHONE_CALL_DIALING:
        snprintf(buf, sizeof(buf), "Calling %s ...", peer[0] ? peer : "?");
        break;
    case PJ_PHONE_CALL_INCOMING:
        snprintf(buf, sizeof(buf), "Incoming: %s", peer[0] ? peer : "?");
        break;
    case PJ_PHONE_CALL_ACTIVE: {
        unsigned long ms = pj_phone_get_call_duration_ms();
        snprintf(buf, sizeof(buf), "In call %02lu:%02lu",
                 ms / 60000UL, (ms / 1000UL) % 60UL);
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "IDLE");
        break;
    }
    lv_label_set_text(s_lbl_state, buf);

    /* context action buttons */
    switch (call) {
    case PJ_PHONE_CALL_INCOMING:
        show_action(s_btn_a, s_lbl_a, 1, "REJ", lv_palette_main(LV_PALETTE_RED));
        show_action(s_btn_b, s_lbl_b, 0, "", lv_color_black());
        show_action(s_btn_c, s_lbl_c, 1, "ANS", lv_palette_main(LV_PALETTE_GREEN));
        break;
    case PJ_PHONE_CALL_DIALING:
    case PJ_PHONE_CALL_ACTIVE:
        show_action(s_btn_a, s_lbl_a, 0, "", lv_color_black());
        show_action(s_btn_b, s_lbl_b, 0, "", lv_color_black());
        show_action(s_btn_c, s_lbl_c, 1, "HANG", lv_palette_main(LV_PALETTE_RED));
        break;
    default:
        show_action(s_btn_a, s_lbl_a, 1, "CLR", lv_palette_main(LV_PALETTE_GREY));
        show_action(s_btn_b, s_lbl_b, 1, "DEL", lv_palette_main(LV_PALETTE_BLUE));
        show_action(s_btn_c, s_lbl_c, 1, "CALL", lv_palette_main(LV_PALETTE_GREEN));
        break;
    }
}

static void phone_notify_cb(void *user_data)
{
    (void)user_data;
    s_dirty = 1;
}

void lv_phone_app_update(void)
{
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();

    /* Refresh on a phone event, and every 250 ms while in a call so the
     * duration timer keeps ticking. */
    if (s_dirty ||
        (pj_phone_get_call_state() == PJ_PHONE_CALL_ACTIVE &&
         now - last >= 250))
    {
        s_dirty = 0;
        last = now;
        update_ui();
    }
}

/* ---- app entry -------------------------------------------------- */
void lv_phone_app_create(void)
{
    static const char *keys[12] =
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    lv_obj_t *scr = lv_screen_active();
    int x, y, w, h, i;

    /* status + state labels */
    s_lbl_status = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_status, 10, 8);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lbl_status, "1000 [OFFLINE]");

    s_lbl_state = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_state, 10, 30);
    lv_obj_set_style_text_font(s_lbl_state, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lbl_state, "IDLE");

    /* number display */
    s_lbl_number = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_number, 10, 70);
    lv_obj_set_size(s_lbl_number, W - 20, 40);
    lv_obj_set_style_text_font(s_lbl_number, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_align(s_lbl_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_number, "Enter number");

    /* dial pad: 3 columns x 4 rows */
    w = (W - 20 - 16) / 3;   /* 10px margins + 8px gaps */
    h = 52;
    x = 10;
    y = 130;
    for (i = 0; i < 12; i++) {
        int r = i / 3, c = i % 3;
        lv_obj_t *b = make_button(scr,
                                  x + c * (w + 8), y + r * (h + 8), w, h,
                                  lv_palette_main(LV_PALETTE_BLUE));
        button_label(b, keys[i]);
        lv_obj_add_event_cb(b, key_event_cb, LV_EVENT_CLICKED,
                            (void *)keys[i]);
    }

    /* action bar (y = 372, height 64) */
    y = 372;
    h = 64;
    s_btn_a = make_button(scr, 10, y, w, h, lv_palette_main(LV_PALETTE_GREY));
    s_lbl_a = button_label(s_btn_a, "CLR");
    s_btn_b = make_button(scr, 10 + (w + 8), y, w, h,
                          lv_palette_main(LV_PALETTE_BLUE));
    s_lbl_b = button_label(s_btn_b, "DEL");
    s_btn_c = make_button(scr, 10 + 2 * (w + 8), y, w, h,
                          lv_palette_main(LV_PALETTE_GREEN));
    s_lbl_c = button_label(s_btn_c, "CALL");
    lv_obj_add_event_cb(s_btn_a, action_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_b, action_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_c, action_event_cb, LV_EVENT_CLICKED, NULL);

    /* be notified of phone state changes (from the pjsua worker thread) */
    pj_phone_set_callback(phone_notify_cb, NULL);

    update_ui();
}
