/*
 * pj_crypto.h - SIP credential encryption (obfuscation-level).
 *
 * The SIP registration password is stored encrypted (AES-128-CBC) instead
 * of as plaintext; cred_get_password() returns the decrypted value for
 * pjsua's account config.  Key/IV are baked into the firmware - this keeps
 * the plaintext out of the binary strings but is not a hardware secret.
 */
#ifndef PJ_CRYPTO_H
#define PJ_CRYPTO_H

/* Get the decrypted SIP registration password (static buffer). */
const char *cred_get_password(void);

#endif /* PJ_CRYPTO_H */
