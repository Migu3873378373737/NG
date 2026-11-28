/* ng_cripto.c - Biblioteca nativa @cripto da NG (C11 puro, sem OpenSSL).
 *
 * Tudo que e binario entra e sai como texto HEXADECIMAL (a string da NG nao guarda bytes 0).
 *
 *   Importar @cripto
 *   Cripto.Sha256(hex)                 -> hex          Cripto.Sha512(hex)
 *   Cripto.Hmac256(chaveHex, dadosHex) -> hex          Cripto.Hmac512(chaveHex, dadosHex)
 *   Cripto.Hkdf(ikmHex, saltHex, infoHex, tamanho)     -> hex (HKDF-SHA256)
 *   Cripto.AesGcmCifrar(chaveHex, ivHex, dadosHex, aadHex)    -> hex (cifrado + tag de 16 bytes)
 *   Cripto.AesGcmDecifrar(chaveHex, ivHex, dadosHex, aadHex)  -> hex, ou null se a tag nao confere
 *   Cripto.AesCbcCifrar(chaveHex, ivHex, dadosHex)   -> hex (padding PKCS7)
 *   Cripto.AesCbcDecifrar(chaveHex, ivHex, dadosHex) -> hex, ou null
 *   Cripto.X25519(privHex, pubHex) -> hex    Cripto.X25519Publica(privHex) -> hex
 *   Cripto.Aleatorio(n)  -> hex de n bytes (/dev/urandom)
 *   Cripto.TextoParaHex(txt) / Cripto.HexParaTexto(hex)
 *   Cripto.HexParaBase64(hex) / Cripto.Base64ParaHex(b64)
 */
#include "ng_cripto.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ utilitarios hex/base64 ============================ */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* decodifica hex -> bytes (malloc). Retorna NULL se invalido. */
static uint8_t *hex_decode(const char *h, size_t *outlen) {
    size_t n = strlen(h);
    if (n % 2) return NULL;
    uint8_t *b = (uint8_t *)malloc(n / 2 + 1);
    if (!b) return NULL;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(h[2 * i]), c = hexval(h[2 * i + 1]);
        if (a < 0 || c < 0) { free(b); return NULL; }
        b[i] = (uint8_t)(a * 16 + c);
    }
    *outlen = n / 2;
    return b;
}
static char *hex_encode(const uint8_t *b, size_t n) {
    static const char *d = "0123456789abcdef";
    char *s = (char *)malloc(n * 2 + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) { s[2 * i] = d[b[i] >> 4]; s[2 * i + 1] = d[b[i] & 15]; }
    s[n * 2] = 0;
    return s;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64_encode(const uint8_t *b, size_t n) {
    char *s = (char *)malloc((n + 2) / 3 * 4 + 1);
    size_t p = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        if (i + 1 < n) v |= (uint32_t)b[i + 1] << 8;
        if (i + 2 < n) v |= b[i + 2];
        s[p++] = B64[(v >> 18) & 63];
        s[p++] = B64[(v >> 12) & 63];
        s[p++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        s[p++] = i + 2 < n ? B64[v & 63] : '=';
    }
    s[p] = 0;
    return s;
}
static uint8_t *b64_decode(const char *s, size_t *outlen) {
    size_t n = strlen(s);
    uint8_t *b = (uint8_t *)malloc(n / 4 * 3 + 4);
    size_t p = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const char *q = c == '-' ? strchr(B64, '+') : c == '_' ? strchr(B64, '/') : strchr(B64, c);
        if (!q || !c) { free(b); return NULL; }
        acc = (acc << 6) | (uint32_t)(q - B64);
        bits += 6;
        if (bits >= 8) { bits -= 8; b[p++] = (uint8_t)(acc >> bits); }
    }
    *outlen = p;
    return b;
}

/* ================================== SHA-256 ================================== */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } Sha256;
static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(Sha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha256_init(Sha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha256_update(Sha256 *c, const uint8_t *d, size_t n) {
    c->len += n;
    while (n) {
        size_t t = 64 - c->n; if (t > n) t = n;
        memcpy(c->buf + c->n, d, t); c->n += t; d += t; n -= t;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
static void sha256_final(Sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80; sha256_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, l, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16); out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i]; }
}
void sha256(const uint8_t *d, size_t n, uint8_t out[32]) { Sha256 c; sha256_init(&c); sha256_update(&c, d, n); sha256_final(&c, out); }

/* ================================== SHA-512 ================================== */
typedef struct { uint64_t h[8]; uint64_t len; uint8_t buf[128]; size_t n; } Sha512;
static const uint64_t K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(Sha512 *c, const uint8_t *p) {
    uint64_t w[80], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[8*i+j]; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i-15], 1) ^ ROR64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = ROR64(w[i-2], 19) ^ ROR64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha512_init(Sha512 *c) {
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha512_update(Sha512 *c, const uint8_t *d, size_t n) {
    c->len += n;
    while (n) {
        size_t t = 128 - c->n; if (t > n) t = n;
        memcpy(c->buf + c->n, d, t); c->n += t; d += t; n -= t;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
static void sha512_final(Sha512 *c, uint8_t out[64]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80; sha512_update(c, &pad, 1);
    uint8_t z = 0; while (c->n != 112) sha512_update(c, &z, 1);
    uint8_t l[16] = {0}; for (int i = 0; i < 8; i++) l[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_update(c, l, 16);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) out[8*i+j] = (uint8_t)(c->h[i] >> (56 - 8 * j));
}
static void sha512(const uint8_t *d, size_t n, uint8_t out[64]) { Sha512 c; sha512_init(&c); sha512_update(&c, d, n); sha512_final(&c, out); }

/* ============================ HMAC / HKDF ============================ */
void hmac256(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t out[32]) {
    uint8_t kb[64] = {0}, ip[64], op[64], in[32];
    if (kl > 64) sha256(k, kl, kb); else memcpy(kb, k, kl);
    for (int i = 0; i < 64; i++) { ip[i] = kb[i] ^ 0x36; op[i] = kb[i] ^ 0x5c; }
    Sha256 c; sha256_init(&c); sha256_update(&c, ip, 64); sha256_update(&c, d, dl); sha256_final(&c, in);
    sha256_init(&c); sha256_update(&c, op, 64); sha256_update(&c, in, 32); sha256_final(&c, out);
}
static void hmac512(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t out[64]) {
    uint8_t kb[128] = {0}, ip[128], op[128], in[64];
    if (kl > 128) sha512(k, kl, kb); else memcpy(kb, k, kl);
    for (int i = 0; i < 128; i++) { ip[i] = kb[i] ^ 0x36; op[i] = kb[i] ^ 0x5c; }
    Sha512 c; sha512_init(&c); sha512_update(&c, ip, 128); sha512_update(&c, d, dl); sha512_final(&c, in);
    sha512_init(&c); sha512_update(&c, op, 128); sha512_update(&c, in, 64); sha512_final(&c, out);
}
/* HKDF-SHA256 (RFC 5869); retorna 0 em sucesso (len <= 255*32) */
int hkdf256(const uint8_t *ikm, size_t il, const uint8_t *salt, size_t sl, const uint8_t *info, size_t nl, uint8_t *out, size_t len) {
    if (len > 255 * 32) return -1;
    uint8_t zeros[32] = {0}, prk[32];
    if (sl == 0) hmac256(zeros, 32, ikm, il, prk); else hmac256(salt, sl, ikm, il, prk);
    uint8_t t[32]; size_t tl = 0, pos = 0;
    uint8_t *buf = (uint8_t *)malloc(32 + nl + 1);
    for (uint8_t i = 1; pos < len; i++) {
        memcpy(buf, t, tl); memcpy(buf + tl, info, nl); buf[tl + nl] = i;
        hmac256(prk, 32, buf, tl + nl + 1, t); tl = 32;
        size_t c = len - pos < 32 ? len - pos : 32;
        memcpy(out + pos, t, c); pos += c;
    }
    free(buf);
    return 0;
}

/* ================================== AES ================================== */
static uint8_t SBOX[256], INVSBOX[256];
static int aes_ready = 0;
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}
static void aes_tables(void) {
    if (aes_ready) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        SBOX[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int i = 0; i < 256; i++) INVSBOX[SBOX[i]] = (uint8_t)i;
    aes_ready = 1;
}
typedef struct { uint8_t rk[15][16]; int rounds; } AesKey;
static int aes_setkey(AesKey *k, const uint8_t *key, size_t kl) {
    aes_tables();
    if (kl != 16 && kl != 24 && kl != 32) return -1;
    int nk = (int)kl / 4; k->rounds = nk + 6;
    uint8_t w[60][4];
    for (int i = 0; i < nk; i++) memcpy(w[i], key + 4 * i, 4);
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (k->rounds + 1); i++) {
        uint8_t t[4]; memcpy(t, w[i - 1], 4);
        if (i % nk == 0) {
            uint8_t x = t[0]; t[0] = (uint8_t)(SBOX[t[1]] ^ rcon); t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[x];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = SBOX[t[j]];
        }
        for (int j = 0; j < 4; j++) w[i][j] = (uint8_t)(w[i - nk][j] ^ t[j]);
    }
    for (int r = 0; r <= k->rounds; r++) for (int c = 0; c < 4; c++) memcpy(k->rk[r] + 4 * c, w[4 * r + c], 4);
    return 0;
}
static void aes_encrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ k->rk[0][i];
    for (int r = 1; r <= k->rounds; r++) {
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[i]];
        /* ShiftRows (estado em ordem de coluna) */
        uint8_t u[16];
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) u[4 * c + row] = t[4 * ((c + row) % 4) + row];
        if (r != k->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t *a = u + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                a[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                a[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                a[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = u[i] ^ k->rk[r][i];
    }
    memcpy(out, s, 16);
}
static void aes_decrypt(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], u[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ k->rk[k->rounds][i];
    for (int r = k->rounds - 1; r >= 0; r--) {
        /* InvShiftRows */
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) u[4 * ((c + row) % 4) + row] = s[4 * c + row];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(INVSBOX[u[i]] ^ k->rk[r][i]);
        if (r != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t *a = s + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                a[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                a[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                a[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

/* ---- CBC com PKCS7 ---- */
static uint8_t *cbc_encrypt(const uint8_t *key, size_t kl, const uint8_t iv[16], const uint8_t *d, size_t n, size_t *outlen) {
    AesKey k; if (aes_setkey(&k, key, kl)) return NULL;
    size_t pad = 16 - n % 16, tot = n + pad;
    uint8_t *o = (uint8_t *)malloc(tot);
    uint8_t prev[16], blk[16]; memcpy(prev, iv, 16);
    for (size_t i = 0; i < tot; i += 16) {
        for (int j = 0; j < 16; j++) { size_t idx = i + (size_t)j; blk[j] = (uint8_t)((idx < n ? d[idx] : pad) ^ prev[j]); }
        aes_encrypt(&k, blk, o + i); memcpy(prev, o + i, 16);
    }
    *outlen = tot; return o;
}
static uint8_t *cbc_decrypt(const uint8_t *key, size_t kl, const uint8_t iv[16], const uint8_t *d, size_t n, size_t *outlen) {
    AesKey k; if (aes_setkey(&k, key, kl) || n == 0 || n % 16) return NULL;
    uint8_t *o = (uint8_t *)malloc(n);
    uint8_t prev[16], blk[16]; memcpy(prev, iv, 16);
    for (size_t i = 0; i < n; i += 16) {
        aes_decrypt(&k, d + i, blk);
        for (int j = 0; j < 16; j++) o[i + (size_t)j] = (uint8_t)(blk[j] ^ prev[j]);
        memcpy(prev, d + i, 16);
    }
    uint8_t pad = o[n - 1];
    if (pad == 0 || pad > 16 || pad > n) { free(o); return NULL; }
    for (size_t i = 0; i < pad; i++) if (o[n - 1 - i] != pad) { free(o); return NULL; }
    *outlen = n - pad; return o;
}

/* ---- GCM ---- */
static void gf_mult(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0}, v[16]; memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i / 8] >> (7 - i % 8)) & 1) for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        size_t c = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < c; j++) y[j] ^= d[i + j];
        gf_mult(y, h);
    }
}
static void inc32(uint8_t cb[16]) { for (int i = 15; i >= 12; i--) if (++cb[i]) break; }
/* cifra/decifra (CTR) e calcula a tag; encrypt=1: d e plain -> out cifrado; encrypt=0: d e cifrado */
int gcm_run(const uint8_t *key, size_t kl, const uint8_t *iv, size_t ivl, const uint8_t *aad, size_t al,
                   const uint8_t *d, size_t n, uint8_t *out, uint8_t tag[16], int encrypt) {
    AesKey k; if (aes_setkey(&k, key, kl)) return -1;
    uint8_t h[16] = {0}, zero[16] = {0}; aes_encrypt(&k, zero, h);
    uint8_t j0[16] = {0};
    if (ivl == 12) { memcpy(j0, iv, 12); j0[15] = 1; }
    else {
        uint8_t y[16] = {0}; ghash(y, h, iv, ivl);
        uint8_t l[16] = {0}; uint64_t b = (uint64_t)ivl * 8; for (int i = 0; i < 8; i++) l[8 + i] = (uint8_t)(b >> (56 - 8 * i));
        ghash(y, h, l, 16); memcpy(j0, y, 16);
    }
    uint8_t cb[16]; memcpy(cb, j0, 16);
    for (size_t i = 0; i < n; i += 16) {
        inc32(cb);
        uint8_t ks[16]; aes_encrypt(&k, cb, ks);
        size_t c = n - i < 16 ? n - i : 16;
        for (size_t j = 0; j < c; j++) out[i + j] = (uint8_t)(d[i + j] ^ ks[j]);
    }
    uint8_t y[16] = {0};
    ghash(y, h, aad, al);
    ghash(y, h, encrypt ? out : d, n);
    uint8_t l[16] = {0}; uint64_t ab = (uint64_t)al * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) { l[i] = (uint8_t)(ab >> (56 - 8 * i)); l[8 + i] = (uint8_t)(cbits >> (56 - 8 * i)); }
    ghash(y, h, l, 16);
    uint8_t ek[16]; aes_encrypt(&k, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(y[i] ^ ek[i]);
    return 0;
}

/* ================================== X25519 ================================== */
typedef int64_t gf[16];
static const gf C121665 = {0xDB41, 1};
static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(gf p, gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fS(c, c); if (a != 2 && a != 4) fM(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
void x25519(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]) {
    uint8_t z[32]; int64_t x[80]; gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64; z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fA(e, a, c); fZ(a, a, c); fA(c, b, d); fZ(b, b, d);
        fS(d, e); fS(f, a); fM(a, c, a); fM(c, b, e);
        fA(e, a, c); fZ(a, a, c); fS(b, a); fZ(c, d, f);
        fM(a, c, C121665); fA(a, a, d); fM(c, c, a); fM(a, d, f);
        fM(d, b, x); fS(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    inv25519((int64_t *)(x + 32), (int64_t *)(x + 32));
    fM((int64_t *)(x + 16), (int64_t *)(x + 16), (int64_t *)(x + 32));
    pack25519(q, (int64_t *)(x + 16));
}

/* ================================== ligacao com a NG ================================== */
#define ARG_STR(i) (argc > (i) && args[i].type == NG_VAL_STRING ? args[i].as.string : NULL)

static NGValue ret_hex(const uint8_t *b, size_t n) { return ng_val_string_take(hex_encode(b, n)); }

static NGValue f_sha256(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *h = ARG_STR(0); size_t n; if (!h) return ng_val_null();
    uint8_t *d = hex_decode(h, &n); if (!d) return ng_val_null();
    uint8_t o[32]; sha256(d, n, o); free(d); return ret_hex(o, 32);
}
static NGValue f_sha512(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *h = ARG_STR(0); size_t n; if (!h) return ng_val_null();
    uint8_t *d = hex_decode(h, &n); if (!d) return ng_val_null();
    uint8_t o[64]; sha512(d, n, o); free(d); return ret_hex(o, 64);
}
static NGValue hmac_generic(NGValue *args, size_t argc, int big) {
    const char *hk = ARG_STR(0), *hd = ARG_STR(1); if (!hk || !hd) return ng_val_null();
    size_t kl, dl; uint8_t *k = hex_decode(hk, &kl), *d = hex_decode(hd, &dl);
    if (!k || !d) { free(k); free(d); return ng_val_null(); }
    uint8_t o[64];
    if (big) hmac512(k, kl, d, dl, o); else hmac256(k, kl, d, dl, o);
    free(k); free(d); return ret_hex(o, big ? 64 : 32);
}
static NGValue f_hmac256(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return hmac_generic(args, argc, 0); }
static NGValue f_hmac512(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return hmac_generic(args, argc, 1); }

static NGValue f_hkdf(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in;
    const char *a = ARG_STR(0), *b = ARG_STR(1), *c = ARG_STR(2);
    if (!a || !b || !c || argc < 4 || args[3].type != NG_VAL_NUMBER) return ng_val_null();
    size_t il, sl, nl; uint8_t *ikm = hex_decode(a, &il), *salt = hex_decode(b, &sl), *info = hex_decode(c, &nl);
    size_t len = (size_t)args[3].as.number;
    NGValue r = ng_val_null();
    if (ikm && salt && info && len > 0 && len <= 255 * 32) {
        uint8_t *o = (uint8_t *)malloc(len);
        if (hkdf256(ikm, il, salt, sl, info, nl, o, len) == 0) r = ret_hex(o, len);
        free(o);
    }
    free(ikm); free(salt); free(info); return r;
}

static NGValue gcm_fn(NGValue *args, size_t argc, int encrypt) {
    const char *hk = ARG_STR(0), *hi = ARG_STR(1), *hd = ARG_STR(2), *ha = argc > 3 && args[3].type == NG_VAL_STRING ? args[3].as.string : "";
    if (!hk || !hi || !hd) return ng_val_null();
    size_t kl, il, dl, al;
    uint8_t *k = hex_decode(hk, &kl), *iv = hex_decode(hi, &il), *d = hex_decode(hd, &dl), *aad = hex_decode(ha, &al);
    NGValue r = ng_val_null();
    if (k && iv && d && aad && il > 0) {
        uint8_t tag[16];
        if (encrypt) {
            uint8_t *o = (uint8_t *)malloc(dl + 16);
            if (gcm_run(k, kl, iv, il, aad, al, d, dl, o, tag, 1) == 0) { memcpy(o + dl, tag, 16); r = ret_hex(o, dl + 16); }
            free(o);
        } else if (dl >= 16) {
            size_t cl = dl - 16;
            uint8_t *o = (uint8_t *)malloc(cl + 1);
            if (gcm_run(k, kl, iv, il, aad, al, d, cl, o, tag, 0) == 0) {
                uint8_t diff = 0; for (int i = 0; i < 16; i++) diff |= (uint8_t)(tag[i] ^ d[cl + (size_t)i]);
                if (diff == 0) r = ret_hex(o, cl);
            }
            free(o);
        }
    }
    free(k); free(iv); free(d); free(aad); return r;
}
static NGValue f_gcm_enc(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return gcm_fn(args, argc, 1); }
static NGValue f_gcm_dec(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return gcm_fn(args, argc, 0); }

static NGValue cbc_fn(NGValue *args, size_t argc, int encrypt) {
    const char *hk = ARG_STR(0), *hi = ARG_STR(1), *hd = ARG_STR(2);
    if (!hk || !hi || !hd) return ng_val_null();
    size_t kl, il, dl; uint8_t *k = hex_decode(hk, &kl), *iv = hex_decode(hi, &il), *d = hex_decode(hd, &dl);
    NGValue r = ng_val_null();
    if (k && iv && d && il == 16) {
        size_t ol; uint8_t *o = encrypt ? cbc_encrypt(k, kl, iv, d, dl, &ol) : cbc_decrypt(k, kl, iv, d, dl, &ol);
        if (o) { r = ret_hex(o, ol); free(o); }
    }
    free(k); free(iv); free(d); return r;
}
static NGValue f_cbc_enc(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return cbc_fn(args, argc, 1); }
static NGValue f_cbc_dec(NGInterpreter *in, NGValue *args, size_t argc) { (void)in; return cbc_fn(args, argc, 0); }

static NGValue f_x25519(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *a = ARG_STR(0), *b = ARG_STR(1); if (!a || !b) return ng_val_null();
    size_t al, bl; uint8_t *p = hex_decode(a, &al), *q = hex_decode(b, &bl);
    NGValue r = ng_val_null();
    if (p && q && al == 32 && bl == 32) { uint8_t o[32]; x25519(o, p, q); r = ret_hex(o, 32); }
    free(p); free(q); return r;
}
static NGValue f_x25519_pub(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *a = ARG_STR(0); if (!a) return ng_val_null();
    size_t al; uint8_t *p = hex_decode(a, &al); NGValue r = ng_val_null();
    if (p && al == 32) { uint8_t base[32] = {9}, o[32]; x25519(o, p, base); r = ret_hex(o, 32); }
    free(p); return r;
}
static NGValue f_aleatorio(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; if (argc < 1 || args[0].type != NG_VAL_NUMBER) return ng_val_null();
    size_t n = (size_t)args[0].as.number; if (n == 0 || n > 65536) return ng_val_null();
    uint8_t *b = (uint8_t *)malloc(n);
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, n, f) != n) { if (f) fclose(f); free(b); return ng_val_null(); }
    fclose(f);
    NGValue r = ret_hex(b, n); free(b); return r;
}
static NGValue f_txt2hex(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *s = ARG_STR(0); if (!s) return ng_val_null();
    return ret_hex((const uint8_t *)s, strlen(s));
}
static NGValue f_hex2txt(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *s = ARG_STR(0); if (!s) return ng_val_null();
    size_t n; uint8_t *d = hex_decode(s, &n); if (!d) return ng_val_null();
    char *o = (char *)malloc(n + 1); memcpy(o, d, n); o[n] = 0; free(d);
    return ng_val_string_take(o);
}
static NGValue f_hex2b64(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *s = ARG_STR(0); if (!s) return ng_val_null();
    size_t n; uint8_t *d = hex_decode(s, &n); if (!d) return ng_val_null();
    char *o = b64_encode(d, n); free(d); return ng_val_string_take(o);
}
static NGValue f_b642hex(NGInterpreter *in, NGValue *args, size_t argc) {
    (void)in; const char *s = ARG_STR(0); if (!s) return ng_val_null();
    size_t n; uint8_t *d = b64_decode(s, &n); if (!d) return ng_val_null();
    NGValue r = ret_hex(d, n); free(d); return r;
}

static void load_cripto(NGInterpreter *interp) {
    NGTable *t = ng_table_new(24);
    ng_table_set(t, "Sha256", ng_val_native_func("Sha256", f_sha256));
    ng_table_set(t, "Sha512", ng_val_native_func("Sha512", f_sha512));
    ng_table_set(t, "Hmac256", ng_val_native_func("Hmac256", f_hmac256));
    ng_table_set(t, "Hmac512", ng_val_native_func("Hmac512", f_hmac512));
    ng_table_set(t, "Hkdf", ng_val_native_func("Hkdf", f_hkdf));
    ng_table_set(t, "AesGcmCifrar", ng_val_native_func("AesGcmCifrar", f_gcm_enc));
    ng_table_set(t, "AesGcmDecifrar", ng_val_native_func("AesGcmDecifrar", f_gcm_dec));
    ng_table_set(t, "AesCbcCifrar", ng_val_native_func("AesCbcCifrar", f_cbc_enc));
    ng_table_set(t, "AesCbcDecifrar", ng_val_native_func("AesCbcDecifrar", f_cbc_dec));
    ng_table_set(t, "X25519", ng_val_native_func("X25519", f_x25519));
    ng_table_set(t, "X25519Publica", ng_val_native_func("X25519Publica", f_x25519_pub));
    ng_table_set(t, "Aleatorio", ng_val_native_func("Aleatorio", f_aleatorio));
    ng_table_set(t, "TextoParaHex", ng_val_native_func("TextoParaHex", f_txt2hex));
    ng_table_set(t, "HexParaTexto", ng_val_native_func("HexParaTexto", f_hex2txt));
    ng_table_set(t, "HexParaBase64", ng_val_native_func("HexParaBase64", f_hex2b64));
    ng_table_set(t, "Base64ParaHex", ng_val_native_func("Base64ParaHex", f_b642hex));
    ng_env_set(interp->globals, "Cripto", ng_val_table(t));
}

void ng_cripto_module_register(void) {
    ng_native_module_register("cripto", load_cripto);
}
