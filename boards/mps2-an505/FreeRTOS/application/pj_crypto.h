/*
 * pj_crypto.h - SIP credential encryption + strong random source.
 *
 * The SIP registration password is stored encrypted (AES-128-CBC) instead
 * of as plaintext; cred_get_password() returns the decrypted value for
 * pjsua's account config.  Key/IV are baked into the firmware - this keeps
 * the plaintext out of the binary strings but is not a hardware secret.
 *
 * cred_random_bytes() draws from the mbedtls PSA RNG (the same entropy
 * source used by the TLS handshake), used e.g. for SDES-SRTP master keys
 * instead of pjlib's pj_rand()-based fallback.
 */
#ifndef PJ_CRYPTO_H
#define PJ_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* Get the decrypted SIP registration password (static buffer). */
const char *cred_get_password(void);

/* Fill buf with 'len' bytes of randomness from the mbedtls PSA RNG.
 * Returns 0 on success, -1 on failure. */
int cred_random_bytes(uint8_t *buf, size_t len);

#endif /* PJ_CRYPTO_H */
