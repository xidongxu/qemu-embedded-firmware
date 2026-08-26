/*
 * pj_phone.h - PJSUA high-level phone application (UI-facing API).
 *
 * Exposes a small, UI-friendly standard phone interface on top of pjsua:
 * init / dial / answer / reject / hangup + state query + notification.
 * No pjsua types leak into this header so a GUI (LVGL) can use it directly.
 */
#ifndef PJ_PHONE_H
#define PJ_PHONE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registration state. */
typedef enum {
    PJ_PHONE_REG_UNREGISTERED = 0,
    PJ_PHONE_REG_REGISTERING,
    PJ_PHONE_REG_REGISTERED,
    PJ_PHONE_REG_FAILED
} pj_phone_reg_state_t;

/* Call state. */
typedef enum {
    PJ_PHONE_CALL_IDLE = 0,
    PJ_PHONE_CALL_DIALING,
    PJ_PHONE_CALL_INCOMING,
    PJ_PHONE_CALL_ACTIVE
} pj_phone_call_state_t;

/* UI notification callback, called from the pjsua worker thread. */
typedef void (*pj_phone_cb_t)(void *user_data);

/* Initialise pjsua and register the local account. */
int pj_phone_init(void);
/* Register a callback invoked on any registration/call state change. */
void pj_phone_set_callback(pj_phone_cb_t cb, void *user_data);
/* Set the SIP host used to build dial URIs. */
void pj_phone_set_dial_host(const char *host, unsigned port);
/* Get the current dial host. */
const char *pj_phone_get_dial_host(void);
/* Dial an extension or a full SIP URI. */
int pj_phone_dial(const char *number);
/* Answer the pending incoming call. */
int pj_phone_answer(void);
/* Reject the pending incoming call. */
int pj_phone_reject(void);
/* Hang up the active or ringing call. */
int pj_phone_hangup(void);
/* Send DTMF digits during an active call (RFC 2833). */
int pj_phone_send_dtmf(const char *digits);
/* Get the most recently received DTMF digits (returns the length). */
int pj_phone_get_rx_dtmf(char *buf, int size);
/* Get the registration state. */
pj_phone_reg_state_t pj_phone_get_reg_state(void);
/* Get the call state. */
pj_phone_call_state_t pj_phone_get_call_state(void);
/* Get the remote user. */
const char *pj_phone_get_peer_number(void);
/* Get the call duration in ms (0 if not active). */
unsigned long pj_phone_get_call_duration_ms(void);
/* Get the SIP status code of the last ended call (0 if none). */
int pj_phone_get_last_call_status(void);
/* Get the text of the last ended call. */
const char *pj_phone_get_last_call_status_text(void);
/* Force re-registration under the current dial host. */
int pj_phone_reregister(void);
/* Return 1 if inbound media has stalled while in a call. */
int pj_phone_get_media_stall(void);

#ifdef __cplusplus
}
#endif

#endif /* PJ_PHONE_H */
