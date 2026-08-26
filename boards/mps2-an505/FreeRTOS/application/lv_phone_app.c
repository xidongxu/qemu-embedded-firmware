/*
 * lv_phone_app.c - LVGL phone application (dial pad + answer/hangup UI).
 *
 * Built on the project's LVGL demo infra (lv_disp.c provides the display,
 * the touch input device and the LVGL task).  This app talks to the standard
 * phone API in pj_phone.h.
 *
 * Layout (450x450):
 *   [ status ]        registration + call state
 *   [ number ]        dialed digits (big, center)
 *   [ 1 2 3 ]         dial pad (3x4)
 *   [ 4 5 6 ]
 *   [ 7 8 9 ]
 *   [ * 0 # ]
 *   [ A ][ B ][ C ]   context actions
 *       idle:      CLR | DEL | CALL
 *       incoming:  REJ |  -  | ANS
 *       active:     -  |  -  | HANG
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

/* UI state. */
static char s_number[32] = "";
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_state;
static lv_obj_t *s_lbl_number;
static lv_obj_t *s_btn_a;
static lv_obj_t *s_btn_b;
static lv_obj_t *s_btn_c;
static lv_obj_t *s_lbl_a;
static lv_obj_t *s_lbl_b;
static lv_obj_t *s_lbl_c;

/* Set by the phone notify callback. */
static volatile int s_dirty = 1;

/* Host-settings mode: fix the dial host from the keypad when the host's DHCP
 * IP changed.  The keypad edits an IP ('*' = dot), [C] saves and re-registers. */
static int s_setting_host = 0;

/* lv_tick when a one-shot message started. */
static uint32_t s_feedback_at = 0;

/* lv_tick when the UI last transitioned into idle with a non-normal end
 * reason; used to show "Ended: ..." for a few seconds. */
static uint32_t s_end_reason_at = 0;

static void enter_setting_mode(void);
static void exit_setting_mode(void);
static void apply_host_setting(void);

/* Create a button. */
static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_color_t bg) {
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

/* Create a centered label on a button. */
static lv_obj_t *button_label(lv_obj_t *btn, const char *txt) {
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return l;
}

/* Append a character to the number buffer. */
static void number_append(char ch) {
    size_t n = strlen(s_number);
    if (n + 1 < sizeof(s_number)) {
        s_number[n] = ch;
        s_number[n + 1] = '\0';
        lv_label_set_text(s_lbl_number, s_number);
    }
}

/* Keypad event handler. */
static void key_event_cb(lv_event_t *e) {
    const char *d = (const char *)lv_event_get_user_data(e);
    char ch = 0;

    if (d == NULL) {
        return;
    }

    if (s_setting_host) {
        /* Settings mode: '#' cancels, digits append, '*' becomes a dot. */
        if (d[0] == '#') {
            exit_setting_mode();
            return;
        }
        ch = (d[0] == '*') ? '.' : d[0];
        if (ch != '.' && (ch < '0' || ch > '9')) {
            return;
        }
    } else {
        /* Normal mode: '#' enters settings while idle, other keys append. */
        if (d[0] == '#' && pj_phone_get_call_state() == PJ_PHONE_CALL_IDLE) {
            enter_setting_mode();
            return;
        }
        ch = d[0];
    }

    number_append(ch);
}

/* Context action button handler. */
static void action_event_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    pj_phone_call_state_t st = pj_phone_get_call_state();
    size_t n = 0;

    /* Host-settings mode: [C]=SAVE+apply, [A]=CLR, [B]=DEL. */
    if (s_setting_host) {
        if (btn == s_btn_c) {
            apply_host_setting();
        } else if (btn == s_btn_a) {
            s_number[0] = '\0';
            lv_label_set_text(s_lbl_number, "");
        } else if (btn == s_btn_b) {
            n = strlen(s_number);
            if (n) {
                s_number[n - 1] = '\0';
                lv_label_set_text(s_lbl_number, s_number);
            }
        }
        return;
    }

    /* Right slot [C]: dial / answer / hangup depending on the call state. */
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

    /* Left slot [A]: reject (incoming) or clear (idle). */
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
        n = strlen(s_number);
        if (n) {
            s_number[n - 1] = '\0';
            lv_label_set_text(s_lbl_number, s_number);
        }
    }
}

/* Enter host-settings mode, pre-filling the current host. */
static void enter_setting_mode(void) {
    const char *cur = pj_phone_get_dial_host();

    s_setting_host = 1;
    s_feedback_at = 0;
    if (cur && *cur) {
        strncpy(s_number, cur, sizeof(s_number) - 1);
        s_number[sizeof(s_number) - 1] = '\0';
    } else {
        s_number[0] = '\0';
    }
    lv_label_set_text(s_lbl_number, s_number[0] ? s_number : ".");
    s_dirty = 1;
}

/* Leave host-settings mode and restore the normal idle state. */
static void exit_setting_mode(void) {
    s_setting_host = 0;
    s_number[0] = '\0';
    lv_label_set_text(s_lbl_number, "Enter number");
    s_dirty = 1;
}

/* Apply the edited host and force re-registration under it. */
static void apply_host_setting(void) {
    if (!s_number[0]) {
        exit_setting_mode();
        return;
    }

    pj_phone_set_dial_host(s_number, 5060);
    pj_phone_reregister();
    s_setting_host = 0;
    s_number[0] = '\0';
    lv_label_set_text(s_lbl_number, "");
    s_feedback_at = lv_tick_get();
    s_dirty = 1;
}

/* Show or hide an action button and update its text/color. */
static void show_action(lv_obj_t *btn, lv_obj_t *lbl, int visible,
                        const char *txt, lv_color_t color) {
    if (!visible) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_bg_color(btn, color, 0);
}

/* Refresh all UI elements from the phone state. */
static void update_ui(void) {
    pj_phone_reg_state_t reg = pj_phone_get_reg_state();
    pj_phone_call_state_t call = pj_phone_get_call_state();
    const char *peer = pj_phone_get_peer_number();
    static pj_phone_call_state_t s_prev_call = PJ_PHONE_CALL_IDLE;
    char buf[64];

    /* Track the previous call state: the moment we leave a call and land on
     * IDLE, start the "why did it end" display window. */
    if (call == PJ_PHONE_CALL_IDLE && s_prev_call != PJ_PHONE_CALL_IDLE) {
        s_end_reason_at = lv_tick_get();
    }
    s_prev_call = call;

    /* Host-settings mode renders its own fixed layout and returns. */
    if (s_setting_host) {
        lv_label_set_text(s_lbl_status, "SET HOST (dot=*, #=exit)");
        lv_label_set_text(s_lbl_state, "Enter host IP:");
        show_action(s_btn_a, s_lbl_a, 1, "CLR", lv_palette_main(LV_PALETTE_GREY));
        show_action(s_btn_b, s_lbl_b, 1, "DEL", lv_palette_main(LV_PALETTE_BLUE));
        show_action(s_btn_c, s_lbl_c, 1, "SAVE", lv_palette_main(LV_PALETTE_GREEN));
        return;
    }

    /* Registration line (ASCII only - the bundled fonts have no CJK glyphs). */
    switch (reg) {
    case PJ_PHONE_REG_REGISTERED:
        snprintf(buf, sizeof(buf), "1000 [REG]");
        break;
    case PJ_PHONE_REG_REGISTERING:
        snprintf(buf, sizeof(buf), "1000 [REGISTERING]");
        break;
    case PJ_PHONE_REG_FAILED:
        snprintf(buf, sizeof(buf), "1000 [REG FAIL]");
        break;
    default:
        snprintf(buf, sizeof(buf), "1000 [OFFLINE]");
        break;
    }
    lv_label_set_text(s_lbl_status, buf);

    /* Call state line. */
    switch (call) {
    case PJ_PHONE_CALL_DIALING:
        snprintf(buf, sizeof(buf), "Calling %s ...", peer[0] ? peer : "?");
        break;
    case PJ_PHONE_CALL_INCOMING:
        snprintf(buf, sizeof(buf), "Incoming: %s", peer[0] ? peer : "?");
        break;
    case PJ_PHONE_CALL_ACTIVE: {
        unsigned long ms = pj_phone_get_call_duration_ms();
        if (pj_phone_get_media_stall()) {
            /* Inbound media stalled: warn on the UI; the phone auto-hangups
             * after the stall timeout. */
            snprintf(buf, sizeof(buf), "No audio! %02lu:%02lu",
                     ms / 60000UL, (ms / 1000UL) % 60UL);
        } else {
            snprintf(buf, sizeof(buf), "In call %02lu:%02lu",
                     ms / 60000UL, (ms / 1000UL) % 60UL);
        }
        break;
    }
    default: {
        int last = pj_phone_get_last_call_status();
        if (s_feedback_at != 0 && (lv_tick_get() - s_feedback_at) < 3000) {
            /* Brief one-shot feedback, e.g. "Host set!". */
            snprintf(buf, sizeof(buf), "Host set!");
        } else if (last != 0 && last != 200 &&
                   (lv_tick_get() - s_end_reason_at) < 3000) {
            const char *t = pj_phone_get_last_call_status_text();
            snprintf(buf, sizeof(buf), "Ended: %s",
                     (t && *t) ? t : "FAILED");
        } else {
            snprintf(buf, sizeof(buf), "IDLE");
        }
        break;
    }
    }
    lv_label_set_text(s_lbl_state, buf);

    /* Context action buttons. */
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

/* Phone notify callback (pjsua worker thread) - only set the dirty flag. */
static void phone_notify_cb(void *user_data) {
    (void)user_data;
    s_dirty = 1;
}

/* Poll the phone state and refresh the UI when needed. */
void lv_phone_app_update(void) {
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    int in_reason_window = (s_end_reason_at != 0 &&
                            (now - s_end_reason_at) < 3000);
    int in_feedback_window = (s_feedback_at != 0 &&
                              (now - s_feedback_at) < 3000);

    /* Refresh on a phone event, every 250 ms while in a call, and while a
     * one-shot reason / feedback message is showing so it can time out. */
    if (s_dirty ||
        (pj_phone_get_call_state() == PJ_PHONE_CALL_ACTIVE &&
         now - last >= 250) ||
        (in_reason_window && now - last >= 250) ||
        (in_feedback_window && now - last >= 250)) {
        s_dirty = 0;
        last = now;
        update_ui();
    }
}

/* Create the phone UI. */
void lv_phone_app_create(void) {
    static const char *keys[12] =
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    lv_obj_t *scr = lv_screen_active();
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int i = 0;

    /* Status and state labels. */
    s_lbl_status = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_status, 10, 8);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lbl_status, "1000 [OFFLINE]");

    s_lbl_state = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_state, 10, 30);
    lv_obj_set_style_text_font(s_lbl_state, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lbl_state, "IDLE");

    /* Number display. */
    s_lbl_number = lv_label_create(scr);
    lv_obj_set_pos(s_lbl_number, 10, 70);
    lv_obj_set_size(s_lbl_number, W - 20, 40);
    lv_obj_set_style_text_font(s_lbl_number, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_align(s_lbl_number, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_number, "Enter number");

    /* Dial pad: 3 columns x 4 rows. */
    w = (W - 20 - 16) / 3;
    h = 52;
    x = 10;
    y = 130;
    for (i = 0; i < 12; i++) {
        int r = i / 3;
        int c = i % 3;
        lv_obj_t *b = make_button(scr,
                                  x + c * (w + 8), y + r * (h + 8), w, h,
                                  lv_palette_main(LV_PALETTE_BLUE));
        button_label(b, keys[i]);
        lv_obj_add_event_cb(b, key_event_cb, LV_EVENT_CLICKED,
                            (void *)keys[i]);
    }

    /* Action bar. */
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

    /* Be notified of phone state changes (from the pjsua worker thread). */
    pj_phone_set_callback(phone_notify_cb, NULL);

    update_ui();
}
