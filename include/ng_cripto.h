#ifndef NG_CRIPTO_H
#define NG_CRIPTO_H
#include <stddef.h>
#include <stdint.h>
/* Registra o modulo nativo @cripto. */
void ng_cripto_module_register(void);

/* Primitivas expostas para outros modulos nativos (ex.: @zap). */
void sha256(const uint8_t *d, size_t n, uint8_t out[32]);
void hmac256(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t out[32]);
int hkdf256(const uint8_t *ikm, size_t il, const uint8_t *salt, size_t sl, const uint8_t *info, size_t nl, uint8_t *out, size_t len);
void x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]);
/* encrypt=1: out = dados cifrados, tag gravada em tag[16]. encrypt=0: out = claro, tag = tag calculada. */
int gcm_run(const uint8_t *key, size_t kl, const uint8_t *iv, size_t ivl, const uint8_t *aad, size_t al,
            const uint8_t *d, size_t n, uint8_t *out, uint8_t tag[16], int encrypt);
#endif
