/* OpenSSL DES declarations for building pspdecrypt without OpenSSL. pspdecrypt
 * uses DES only for PSAR firmware-update tables, which PSPRecomp never needs.
 * This build deliberately does not implement DES: reaching it stops the
 * decryptor with an explicit error instead of producing undecrypted data.
 * EBOOT/PRX decryption does not use these functions. */
#ifndef PSPRECOMP_OPENSSL_DES_SHIM_H
#define PSPRECOMP_OPENSSL_DES_SHIM_H
#include <stdio.h>
#include <stdlib.h>

#define DES_DECRYPT 0
#define DES_ENCRYPT 1
typedef unsigned char DES_cblock[8];
typedef struct { unsigned char unused; } DES_key_schedule;

static inline void psprecomp_des_unavailable(void) {
    fputs("pspdecrypt: PSAR DES table decryption is not available in this Android build\n", stderr);
    exit(2);
}
static inline void DES_set_key_unchecked(DES_cblock *key, DES_key_schedule *schedule) {
    (void)key; (void)schedule; psprecomp_des_unavailable();
}
static inline void DES_cbc_encrypt(const unsigned char *in, unsigned char *out, long length,
                                   DES_key_schedule *schedule, DES_cblock *iv, int enc) {
    (void)in; (void)out; (void)length; (void)schedule; (void)iv; (void)enc; psprecomp_des_unavailable();
}
#endif
