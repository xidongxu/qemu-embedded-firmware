/*
 * lv_phone_app.h - LVGL phone application (dial pad + answer/hangup UI).
 */
#ifndef LV_PHONE_APP_H
#define LV_PHONE_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the phone UI on the active screen. */
void lv_phone_app_create(void);
/* Refresh the UI from the phone state. */
void lv_phone_app_update(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PHONE_APP_H */
