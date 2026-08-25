/*
 * pj_phone.h - PJSUA high-level phone application (UI-facing API).
 *
 * Exposes a small, UI-friendly standard phone interface on top of pjsua:
 *   init / dial / answer / reject / hangup  +  state query + notification.
 * No pjsua types leak into this header so a GUI (LVGL) can use it directly.
 */
#ifndef PJ_PHONE_H
#define PJ_PHONE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- registration state ---- */
typedef enum {
    PJ_PHONE_REG_UNREGISTERED = 0,
    PJ_PHONE_REG_REGISTERING,
    PJ_PHONE_REG_REGISTERED,
    PJ_PHONE_REG_FAILED
} pj_phone_reg_state_t;

/* ---- call state ---- */
typedef enum {
    PJ_PHONE_CALL_IDLE = 0,    /* no call */
    PJ_PHONE_CALL_DIALING,     /* outgoing: CALLING / EARLY (ringing) */
    PJ_PHONE_CALL_INCOMING,    /* incoming, waiting for answer/reject */
    PJ_PHONE_CALL_ACTIVE       /* connected, media up */
} pj_phone_call_state_t;

/* UI notification callback.  Called from the pjsua worker thread - only set
 * a flag / post a message, do NOT touch the UI (LVGL) directly. */
typedef void (*pj_phone_cb_t)(void *user_data);

/* ---- lifecycle ---- */
/* Initialise pjsua + register the local account.  Does NOT auto-dial; the UI
 * drives calls via pj_phone_dial()/answer()/hangup(). */
int  pj_phone_init(void);

/* Register a callback invoked on any registration/call state change. */
void pj_phone_set_callback(pj_phone_cb_t cb, void *user_data);

/* ---- configuration (runtime overrides; host IP changes need no rebuild) ---- */
/* Set the SIP host used to build dial URIs (default: PJ_PHONE_DIAL_HOST). */
void pj_phone_set_dial_host(const char *host, unsigned port);

/* ---- call control (may be called from any task; pjsua is thread-safe) ---- */
/* Dial an extension ("1005") or a full SIP URI ("sip:user@host:port"). */
int  pj_phone_dial(const char *number);
int  pj_phone_answer(void);   /* answer the pending incoming call (200) */
int  pj_phone_reject(void);   /* reject the pending incoming call (486) */
int  pj_phone_hangup(void);   /* hang up the active / ringing call */

/* ---- state query (poll from the UI loop) ---- */
pj_phone_reg_state_t  pj_phone_get_reg_state(void);
pj_phone_call_state_t pj_phone_get_call_state(void);
const char           *pj_phone_get_peer_number(void);       /* remote user */
unsigned long         pj_phone_get_call_duration_ms(void);  /* 0 if not active */

#ifdef __cplusplus
}
#endif

#endif /* PJ_PHONE_H */
