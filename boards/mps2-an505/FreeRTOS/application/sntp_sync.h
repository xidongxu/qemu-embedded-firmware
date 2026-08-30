/*
 * sntp_sync.h - boot-time SNTP time sync for the guest RTC epoch.
 *
 * The board has no RTC; without a real clock mbedtls X.509 certificate
 * validity checks see 1970 and the TLS handshake fails.  lwIP's SNTP client
 * pulls the real time from the host NTP server and this module publishes the
 * synced epoch (seconds since 1970-01-01 UTC) for mbedtls_port.c.
 */
#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <time.h>

/* Get the synchronized RTC epoch (seconds since 1970-01-01 UTC).
 * Returns 0 until the first SNTP response arrives. */
time_t sntp_sync_get_epoch(void);
/* Start the SNTP client (call once lwIP/tcpip is up, before pjsua/TLS). */
void sntp_sync_init(void);
/* SNTP callback (lwIP context): store the new epoch. */
void sntp_sync_set_system_time(unsigned int sec);

#endif /* SNTP_SYNC_H */
