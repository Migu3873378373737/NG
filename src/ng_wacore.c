/* ng_wacore.c - @wacore: cliente WhatsApp Web nativo em C11.
 *
 * Camadas:
 *   - @ws   (ng_ws.c)     transporte WebSocket/TLS
 *   - @zap  (ng_zap.c)    codec binario de nos + Noise XX + GCM do transporte
 *   - este arquivo        credenciais, QR/pareamento, Signal (X3DH + double
 *                         ratchet), sessoes persistentes, mensagens e botoes.
 *
 * A API NG e sincrona e orientada a eventos:
 *   Wacore.Iniciar(dir, forcarNovo)
 *   Wacore.Atualizar(ms)      -> processa rede, QR, mensagens e fila de envio
 *   Wacore.Mensagens()        -> lista de mensagens recebidas desde a ultima chamada
 *   Wacore.QR()               -> {disponivel,texto,indice,total,estado}
 *   Wacore.Estado()  Wacore.Pronto()  Wacore.Erro()  Wacore.Me()
 *   Wacore.Enviar(chat, texto)
 *   Wacore.EnviarBotoes(chat, texto, [{id=...,titulo=...}, ...])
 *   Wacore.Fechar()
 *
 * Dados binarios nao atravessam a NG: ficam neste modulo. A NG ve apenas
 * texto, numeros e tabelas. O estado fica em <dir>/creds.json e
 * <dir>/signal.bin.
 */
#include "ng_wacore.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"
#include "ng_cripto.h"
#include "ng_ws.h"

#ifdef _WIN32
#include "../compat/windows/net_compat.h"
#include "../compat/windows/dir_compat.h"
/* clock_gettime/nanosleep via QueryPerformanceCounter/Sleep (o MinGW aponta
   para clock_gettime64/nanosleep64 da libwinpthread, ausente no runtime). */
#include "../compat/windows/posix_compat.h"
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef NG_HAS_OPENSSL
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

#ifdef NG_HAS_SODIUM
#include <sodium.h>
#endif

#define WC_DIR_MAX 240
#define WC_BUF_MAX (16u * 1024u * 1024u)
#define WC_MAX_PREKEYS 64
#define WC_MAX_SESSIONS 64
#define WC_MAX_MSGS 64
#define WC_MAX_OUT 16
#define WC_MAX_TARGETS 32

static int wc_debug_on(void);
#define WC_DBG(...) do { if (wc_debug_on()) { fprintf(stderr, "[wacore] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

/* ------------------------------------------------------------------ buffer */
typedef struct { uint8_t *p; size_t n, cap; int erro; } B;
static void b_need(B *b, size_t k) {
    if (b->erro) return;
    if (b->n + k <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + k) c *= 2;
    uint8_t *q = (uint8_t *)realloc(b->p, c);
    if (!q) { b->erro = 1; return; }
    b->p = q; b->cap = c;
}
static void b_put(B *b, const void *d, size_t n) {
    if (!n) return;
    b_need(b, n);
    if (b->erro) return;
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b_u8(B *b, unsigned v) { uint8_t x = (uint8_t)v; b_put(b, &x, 1); }
static void b_free(B *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; b->erro = 0; }
/* ------------------------------------------------------------------ hex/b64 */
static int hx(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static uint8_t *unhex(const char *s, size_t *out_n) {
    if (!s) return NULL;
    size_t n = strlen(s);
    if (n & 1) return NULL;
    uint8_t *p = (uint8_t *)malloc(n / 2 + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hx(s[2*i]), c = hx(s[2*i+1]);
        if (a < 0 || c < 0) { free(p); return NULL; }
        p[i] = (uint8_t)(a * 16 + c);
    }
    *out_n = n / 2;
    p[*out_n] = 0;
    return p;
}
static char *tohex(const uint8_t *p, size_t n) {
    static const char *D = "0123456789abcdef";
    char *s = (char *)malloc(n * 2 + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) { s[2*i] = D[p[i] >> 4]; s[2*i+1] = D[p[i] & 15]; }
    s[2*n] = 0;
    return s;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64(const uint8_t *p, size_t n) {
    char *s = (char *)malloc(((n + 2) / 3) * 4 + 1);
    if (!s) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i+1] << 8;
        if (i + 2 < n) v |= p[i+2];
        s[k++] = B64[(v >> 18) & 63];
        s[k++] = B64[(v >> 12) & 63];
        s[k++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        s[k++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    s[k] = 0;
    return s;
}
static int random_bytes(uint8_t *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(p, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

/* ------------------------------------------------------------------ protobuf */
static void pb_varint(B *b, uint64_t v) {
    while (v >= 128) { b_u8(b, (unsigned)((v & 127) | 128)); v >>= 7; }
    b_u8(b, (unsigned)v);
}
static void pb_int(B *b, int field, uint64_t v) { pb_varint(b, (uint64_t)field * 8); pb_varint(b, v); }
static void pb_bytes(B *b, int field, const void *d, size_t n) {
    pb_varint(b, (uint64_t)field * 8 + 2);
    pb_varint(b, n);
    b_put(b, d, n);
}
static void pb_str(B *b, int field, const char *s) { pb_bytes(b, field, s, s ? strlen(s) : 0); }

typedef struct { int field, wt; const uint8_t *p; size_t n; uint64_t num; } PBEnt;
typedef struct { PBEnt e[96]; int n; } PB;

static int pb_decode(const uint8_t *p, size_t n, PB *out) {
    size_t i = 0;
    out->n = 0;
    while (i < n) {
        uint64_t tag = 0; int sh = 0;
        while (i < n && sh < 64) {
            uint8_t c = p[i++];
            tag |= (uint64_t)(c & 127) << sh;
            sh += 7;
            if (!(c & 128)) break;
        }
        int field = (int)(tag >> 3), wt = (int)(tag & 7);
        if (field <= 0 || out->n >= 96) return -1;
        PBEnt *e = &out->e[out->n++];
        e->field = field; e->wt = wt; e->p = NULL; e->n = 0; e->num = 0;
        if (wt == 0) {
            uint64_t v = 0; int s2 = 0;
            while (i < n && s2 < 64) {
                uint8_t c = p[i++];
                v |= (uint64_t)(c & 127) << s2;
                s2 += 7;
                if (!(c & 128)) break;
            }
            e->num = v;
        } else if (wt == 2) {
            uint64_t len = 0; int s2 = 0;
            while (i < n && s2 < 64) {
                uint8_t c = p[i++];
                len |= (uint64_t)(c & 127) << s2;
                s2 += 7;
                if (!(c & 128)) break;
            }
            if (len > n - i) return -1;
            e->p = p + i; e->n = (size_t)len; i += (size_t)len;
        } else if (wt == 1) { if (n - i < 8) return -1; i += 8; }
        else if (wt == 5) { if (n - i < 4) return -1; i += 4; }
        else return -1;
    }
    return 0;
}
static const PBEnt *pb_get(const PB *pb, int field) {
    for (int i = 0; i < pb->n; i++) if (pb->e[i].field == field) return &pb->e[i];
    return NULL;
}
static uint64_t pb_num(const PB *pb, int field, uint64_t def) {
    const PBEnt *e = pb_get(pb, field);
    return e ? e->num : def;
}

/* ------------------------------------------------------------------ crypto */
static int aes_cbc(int enc, const uint8_t key[32], const uint8_t iv[16],
                   const uint8_t *in, size_t n, uint8_t **out, size_t *out_n) {
#ifdef NG_HAS_OPENSSL
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    uint8_t *o = (uint8_t *)malloc(n + 32);
    if (!o) { EVP_CIPHER_CTX_free(ctx); return -1; }
    int l1 = 0, l2 = 0;
    int ok = enc ? EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv)
                 : EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv);
    if (ok == 1) ok = enc ? EVP_EncryptUpdate(ctx, o, &l1, in, (int)n)
                          : EVP_DecryptUpdate(ctx, o, &l1, in, (int)n);
    if (ok == 1) ok = enc ? EVP_EncryptFinal_ex(ctx, o + l1, &l2)
                          : EVP_DecryptFinal_ex(ctx, o + l1, &l2);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) { free(o); return -1; }
    *out = o; *out_n = (size_t)(l1 + l2);
    return 0;
#else
    (void)enc; (void)key; (void)iv; (void)in; (void)n; (void)out; (void)out_n;
    return -1;
#endif
}
static int pad_add(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n) {
    uint8_t p = (uint8_t)(16 - (n % 16));
    uint8_t *o = (uint8_t *)malloc(n + p);
    if (!o) return -1;
    memcpy(o, in, n);
    memset(o + n, p, p);
    *out = o; *out_n = n + p;
    return 0;
}
static int pad_remove(const uint8_t *in, size_t n, size_t *out_n) {
    if (!n) return -1;
    uint8_t p = in[n-1];
    if (p < 1 || p > 16 || p > n) return -1;
    for (size_t i = n - p; i < n; i++) if (in[i] != p) return -1;
    *out_n = n - p;
    return 0;
}
static int cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *in, size_t n, uint8_t **out, size_t *out_n) {
    uint8_t *p = NULL; size_t pn = 0;
    if (pad_add(in, n, &p, &pn) != 0) return -1;
    int r = aes_cbc(1, key, iv, p, pn, out, out_n);
    free(p);
    return r;
}
static int cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *in, size_t n, uint8_t **out, size_t *out_n) {
    uint8_t *p = NULL; size_t pn = 0;
    if (aes_cbc(0, key, iv, in, n, &p, &pn) != 0) return -1;
    if (pad_remove(p, pn, out_n) != 0) { free(p); return -1; }
    *out = p;
    return 0;
}

/* XEdDSA (Curve25519 sign), mesmo esquema do libsignal/Baileys. */
#ifdef NG_HAS_SODIUM
static int xeddsa_sign(const uint8_t priv[32], const uint8_t *msg, size_t msglen, uint8_t sig[64]) {
    uint8_t k[32];
    memcpy(k, priv, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    uint8_t A[32], R[32], r[64], h[64], s[32];
    if (crypto_scalarmult_ed25519_base_noclamp(A, k) != 0) return -1;
    int sign_bit = A[31] >> 7;

    B b = {0};
    uint8_t prefix[32];
    prefix[0] = 0xFE;
    memset(prefix + 1, 0xFF, 31);
    uint8_t rnd[64];
    if (random_bytes(rnd, 64) != 0) { b_free(&b); return -1; }
    b_put(&b, prefix, 32);
    b_put(&b, k, 32);
    b_put(&b, msg, msglen);
    b_put(&b, rnd, 64);
#ifdef NG_HAS_OPENSSL
    SHA512(b.p, b.n, r);
#else
    b_free(&b); return -1;
#endif
    b_free(&b);
    crypto_core_ed25519_scalar_reduce(r, r);
    if (crypto_scalarmult_ed25519_base_noclamp(R, r) != 0) return -1;

    B b2 = {0};
    b_put(&b2, R, 32);
    b_put(&b2, A, 32);
    b_put(&b2, msg, msglen);
#ifdef NG_HAS_OPENSSL
    SHA512(b2.p, b2.n, h);
#else
    b_free(&b2); return -1;
#endif
    b_free(&b2);
    crypto_core_ed25519_scalar_reduce(h, h);
    crypto_core_ed25519_scalar_mul(s, h, k);
    crypto_core_ed25519_scalar_add(s, s, r);

    memcpy(sig, R, 32);
    memcpy(sig + 32, s, 32);
    sig[63] = (uint8_t)((sig[63] & 0x7F) | (sign_bit << 7));
    return 0;
}
#else
static int xeddsa_sign(const uint8_t priv[32], const uint8_t *msg, size_t msglen, uint8_t sig[64]) {
    (void)priv; (void)msg; (void)msglen; (void)sig;
    return -1;
}
#endif

/* ------------------------------------------------------------------ nomes */
static uint64_t wc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void wc_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static void split_jid(const char *jid, char *user, size_t us, char *server, size_t ss, uint32_t *dev) {
    if (user) user[0] = 0;
    if (server) server[0] = 0;
    if (dev) *dev = 0;
    if (!jid) return;
    const char *at = strchr(jid, '@');
    if (!at) { snprintf(user, us, "%s", jid); return; }
    size_t ul = (size_t)(at - jid);
    char left[160];
    if (ul >= sizeof left) ul = sizeof left - 1;
    memcpy(left, jid, ul); left[ul] = 0;
    char *colon = strchr(left, ':');
    if (colon) {
        *colon = 0;
        if (dev) *dev = (uint32_t)strtoul(colon + 1, NULL, 10);
    }
    if (user) snprintf(user, us, "%s", left);
    if (server) snprintf(server, ss, "%s", at + 1);
}
static void base_jid(const char *jid, char *out, size_t outn) {
    char u[160], s[96]; uint32_t d;
    split_jid(jid, u, sizeof u, s, sizeof s, &d);
    snprintf(out, outn, "%s@%s", u, s);
}
static void addr_jid(const char *jid, char *out, size_t outn) {
    char u[160], s[96]; uint32_t d;
    split_jid(jid, u, sizeof u, s, sizeof s, &d);
    if (d) snprintf(out, outn, "%s:%u@%s", u, d, s);
    else snprintf(out, outn, "%s@%s", u, s);
}
static int is_group_jid(const char *jid) {
    size_t n = jid ? strlen(jid) : 0;
    return n >= 5 && !strcmp(jid + n - 5, "@g.us");
}

/* ------------------------------------------------------------------ credenciais */
typedef struct {
    char dir[WC_DIR_MAX];
    char jid[160], lid[160];
    uint8_t noise_priv[32], noise_pub[32];
    uint8_t ident_priv[32], ident_pub[32];
    uint8_t skey_priv[32], skey_pub[32], skey_sig[64], adv_secret[32];
    uint32_t skey_id, reg_id;
    uint8_t account[2048]; size_t account_n;
    uint8_t account_full[4096]; size_t account_full_n;
    uint8_t payload[1024]; size_t payload_n;
    uint8_t user_agent[256]; size_t user_agent_n;
    uint8_t web_info[32]; size_t web_info_n;
    int key_index;
    int paired;
} Creds;
static Creds C;

static int mkdir_p(const char *path) {
    char tmp[WC_DIR_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0700); *p = '/'; }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}
static void creds_path(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s", C.dir, name);
}
static int jget(const char *json, const char *key, char *out, size_t n) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') p++;
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}
static int jget_num(const char *json, const char *key, long *out) {
    char pat[128], buf[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    snprintf(buf, sizeof buf, "%s", p + 1);
    *out = strtol(buf, NULL, 10);
    return 1;
}
static int creds_load(void) {
    char path[320];
    creds_path("creds.json", path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return 0; }
    char *j = (char *)malloc((size_t)sz + 1);
    if (!j) { fclose(f); return 0; }
    size_t r = fread(j, 1, (size_t)sz, f);
    fclose(f); j[r] = 0;

    memset(&C, 0, sizeof C);
    jget(j, "jid", C.jid, sizeof C.jid);
    jget(j, "lid", C.lid, sizeof C.lid);
    char hx[8192];
    size_t n = 0;
#define LOADHEX(field, dst) do { if (jget(j, field, hx, sizeof hx)) { uint8_t *q = unhex(hx, &n); \
    if (q && n <= sizeof(dst)) { memcpy(dst, q, n); } free(q); } } while (0)
    LOADHEX("noise_priv", C.noise_priv);
    LOADHEX("noise_pub", C.noise_pub);
    LOADHEX("_ident_priv", C.ident_priv);
    LOADHEX("_ident_pub", C.ident_pub);
    LOADHEX("_skey_priv", C.skey_priv);
    LOADHEX("_skey_pub", C.skey_pub);
    LOADHEX("_skey_sig", C.skey_sig);
    LOADHEX("_adv_secret", C.adv_secret);
    if (jget(j, "_reg_id", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n == 4) C.reg_id = ((uint32_t)q[2] << 8) | q[3];
        free(q);
    }
    if (jget(j, "_skey_id", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n == 3) C.skey_id = ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
        free(q);
    }
    if (jget(j, "account", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n <= sizeof C.account) { memcpy(C.account, q, n); C.account_n = n; }
        free(q);
    }
    if (jget(j, "account_full", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n <= sizeof C.account_full) { memcpy(C.account_full, q, n); C.account_full_n = n; }
        free(q);
    }
    if (jget(j, "payload", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n <= sizeof C.payload) { memcpy(C.payload, q, n); C.payload_n = n; }
        free(q);
    }
    if (jget(j, "_user_agent", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n <= sizeof C.user_agent) { memcpy(C.user_agent, q, n); C.user_agent_n = n; }
        free(q);
    }
    if (jget(j, "_web_info", hx, sizeof hx)) {
        uint8_t *q = unhex(hx, &n);
        if (q && n <= sizeof C.web_info) { memcpy(C.web_info, q, n); C.web_info_n = n; }
        free(q);
    }
    long l = 0;
    if (jget_num(j, "key_index", &l)) C.key_index = (int)l;
#undef LOADHEX
    free(j);
    C.paired = C.jid[0] && C.ident_priv[0] && C.noise_priv[0];
    return C.ident_priv[0] != 0;
}
static int creds_save(void) {
    mkdir_p(C.dir);
    char path[320], tmp[340];
    creds_path("creds.json", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char *hexes[12];
    hexes[0] = tohex(C.noise_priv, 32);
    hexes[1] = tohex(C.noise_pub, 32);
    hexes[2] = tohex(C.ident_priv, 32);
    hexes[3] = tohex(C.ident_pub, 32);
    hexes[4] = tohex(C.skey_priv, 32);
    hexes[5] = tohex(C.skey_pub, 32);
    hexes[6] = tohex(C.adv_secret, 32);
    uint8_t rb[4] = {0, 0, (uint8_t)(C.reg_id >> 8), (uint8_t)C.reg_id};
    uint8_t sb[3] = {0, 0, (uint8_t)C.skey_id};
    hexes[7] = tohex(rb, 4);
    hexes[8] = tohex(sb, 3);
    hexes[9] = tohex(C.account, C.account_n);
    hexes[10] = tohex(C.account_full, C.account_full_n);
    hexes[11] = tohex(C.skey_sig, 64);
    char *payload = tohex(C.payload, C.payload_n);
    char *ua = tohex(C.user_agent, C.user_agent_n);
    char *wi = tohex(C.web_info, C.web_info_n);
    FILE *f = fopen(tmp, "wb");
    if (!f) { for (int i = 0; i < 12; i++) free(hexes[i]); free(payload); free(ua); free(wi); return -1; }
    fprintf(f, "{\n");
    fprintf(f, "  \"jid\": \"%s\",\n  \"lid\": \"%s\",\n", C.jid, C.lid);
    fprintf(f, "  \"noise_priv\": \"%s\",\n  \"noise_pub\": \"%s\",\n", hexes[0], hexes[1]);
    fprintf(f, "  \"_ident_priv\": \"%s\",\n  \"_ident_pub\": \"%s\",\n", hexes[2], hexes[3]);
    fprintf(f, "  \"_skey_priv\": \"%s\",\n  \"_skey_pub\": \"%s\",\n", hexes[4], hexes[5]);
    fprintf(f, "  \"_skey_sig\": \"%s\",\n", hexes[11]);
    fprintf(f, "  \"_adv_secret\": \"%s\",\n", hexes[6]);
    fprintf(f, "  \"_reg_id\": \"%s\",\n  \"_skey_id\": \"%s\",\n", hexes[7], hexes[8]);
    fprintf(f, "  \"account\": \"%s\",\n  \"account_full\": \"%s\",\n", hexes[9], hexes[10]);
    fprintf(f, "  \"payload\": \"%s\",\n", payload ? payload : "");
    fprintf(f, "  \"_user_agent\": \"%s\",\n  \"_web_info\": \"%s\",\n", ua ? ua : "", wi ? wi : "");
    fprintf(f, "  \"key_index\": %d\n}\n", C.key_index);
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, path);
    for (int i = 0; i < 12; i++) free(hexes[i]);
    free(payload); free(ua); free(wi);
    return 0;
}

static int make_creds(void) {
    memset(&C, 0, sizeof C);
    if (random_bytes(C.noise_priv, 32) != 0) return -1;
    if (random_bytes(C.ident_priv, 32) != 0) return -1;
    if (random_bytes(C.skey_priv, 32) != 0) return -1;
    if (random_bytes(C.adv_secret, 32) != 0) return -1;
    uint8_t base[32] = {9};
    x25519(C.noise_pub, C.noise_priv, base);
    x25519(C.ident_pub, C.ident_priv, base);
    x25519(C.skey_pub, C.skey_priv, base);
    uint8_t r2[2];
    if (random_bytes(r2, 2) != 0) return -1;
    C.reg_id = ((uint32_t)r2[0] * 256 + r2[1]) % 16380 + 1;
    C.skey_id = 1;

    /* Signed prekey: ident_priv assina 0x05 || skey_pub. */
    uint8_t msg[33], sig[64];
    msg[0] = 5;
    memcpy(msg + 1, C.skey_pub, 32);
    if (xeddsa_sign(C.ident_priv, msg, 33, sig) != 0) return -1;
    memcpy(C.skey_sig, sig, 64);

    uint8_t rb[4] = {0, 0, (uint8_t)(C.reg_id >> 8), (uint8_t)C.reg_id};
    uint8_t sb[3] = {0, 0, (uint8_t)C.skey_id};

    /* UserAgent { field1=14, field2=AppVersion, field3="000", field4="000",
       field5="0.1", field7="Desktop", field8="0.1", field10=0,
       field11="en", field12="US" } */
    B app = {0}, ua = {0}, wi = {0}, props = {0}, devprops = {0}, dprd = {0}, pay = {0};
    pb_int(&app, 1, 2);
    pb_int(&app, 2, 3000);
    pb_int(&app, 3, 1043857760);
    pb_int(&ua, 1, 14);
    pb_bytes(&ua, 2, app.p, app.n);
    pb_str(&ua, 3, "000");
    pb_str(&ua, 4, "000");
    pb_str(&ua, 5, "0.1");
    pb_str(&ua, 7, "Desktop");
    pb_str(&ua, 8, "0.1");
    pb_int(&ua, 10, 0);
    pb_str(&ua, 11, "en");
    pb_str(&ua, 12, "US");
    pb_int(&wi, 4, 0);

    pb_int(&props, 3, 10240);
    for (int f = 4; f <= 12; f++) if (f != 5) pb_int(&props, f, 1);
    pb_int(&props, 14, 1);
    pb_int(&props, 15, 0);
    pb_str(&devprops, 1, "Ubuntu");
    B osv = {0};
    pb_int(&osv, 1, 10); pb_int(&osv, 2, 15); pb_int(&osv, 3, 7);
    pb_bytes(&devprops, 2, osv.p, osv.n); b_free(&osv);
    pb_int(&devprops, 3, 1);
    pb_int(&devprops, 4, 0);
    pb_bytes(&devprops, 5, props.p, props.n);

    const char *build_hash = "f6f65c0607a1cdffd8752b051fc95edf";
    uint8_t bh[16]; size_t bhn = 0;
    uint8_t *bhp = unhex(build_hash, &bhn);
    if (bhp && bhn == 16) memcpy(bh, bhp, 16);
    free(bhp);

    pb_bytes(&dprd, 1, rb, 4);
    uint8_t v5 = 5; pb_bytes(&dprd, 2, &v5, 1);
    pb_bytes(&dprd, 3, C.ident_pub, 32);
    pb_bytes(&dprd, 4, sb, 3);
    pb_bytes(&dprd, 5, C.skey_pub, 32);
    pb_bytes(&dprd, 6, sig, 64);
    pb_bytes(&dprd, 7, bh, 16);
    pb_bytes(&dprd, 8, devprops.p, devprops.n);

    pb_int(&pay, 3, 0);
    pb_bytes(&pay, 5, ua.p, ua.n);
    pb_bytes(&pay, 6, wi.p, wi.n);
    pb_int(&pay, 12, 1);
    pb_int(&pay, 13, 1);
    pb_bytes(&pay, 19, dprd.p, dprd.n);
    pb_int(&pay, 33, 0);

    if (pay.n > sizeof C.payload || ua.n > sizeof C.user_agent || wi.n > sizeof C.web_info) {
        b_free(&app); b_free(&ua); b_free(&wi); b_free(&props); b_free(&devprops); b_free(&dprd); b_free(&pay);
        return -1;
    }
    memcpy(C.payload, pay.p, pay.n); C.payload_n = pay.n;
    memcpy(C.user_agent, ua.p, ua.n); C.user_agent_n = ua.n;
    memcpy(C.web_info, wi.p, wi.n); C.web_info_n = wi.n;
    b_free(&app); b_free(&ua); b_free(&wi); b_free(&props); b_free(&devprops); b_free(&dprd); b_free(&pay);
    C.paired = 0;
    return 0;
}

static int login_payload(uint8_t **out, size_t *out_n) {
    char u[160], s[96]; uint32_t d;
    split_jid(C.jid, u, sizeof u, s, sizeof s, &d);
    if (!u[0]) return -1;
    B b = {0};
    pb_int(&b, 1, strtoull(u, NULL, 10));
    pb_int(&b, 3, 1);
    pb_bytes(&b, 5, C.user_agent, C.user_agent_n);
    pb_bytes(&b, 6, C.web_info, C.web_info_n);
    pb_int(&b, 12, 1);
    pb_int(&b, 13, 1);
    pb_int(&b, 18, d);
    pb_int(&b, 33, 1);
    pb_int(&b, 41, 0);
    if (b.erro) { b_free(&b); return -1; }
    *out = b.p; *out_n = b.n; b.p = NULL; b_free(&b);
    return 0;
}

/* ------------------------------------------------------------------ Signal */
static const uint8_t SIG_VERSION = 0x33;
static const uint8_t ZERO32[32] = {0};
static const uint8_t FF32[32] = {0xFF};

/* ------------------------------------------------------- sender keys (grupos) */
#define WC_MAX_SK 64
#define WC_MAX_SKIP 16
#define WC_MAX_DIST 32
typedef struct {
    int used;
    char group[160], sender[160];
    uint32_t id;
    uint8_t chain[32];
    uint8_t sign_priv[32], sign_pub[32];
    uint32_t index;
    struct { int used; uint32_t iter; uint8_t seed[32]; } skipped[WC_MAX_SKIP];
    int ndist;
    char dist[WC_MAX_DIST][160];
} SenderKey;
static SenderKey G_SK[WC_MAX_SK];

static SenderKey *skkey_find(const char *group, const char *sender, uint32_t id) {
    for (int i = 0; i < WC_MAX_SK; i++)
        if (G_SK[i].used && !strcmp(G_SK[i].group, group) &&
            !strcmp(G_SK[i].sender, sender) && G_SK[i].id == id) return &G_SK[i];
    return NULL;
}
static SenderKey *sk_alloc(const char *group, const char *sender, uint32_t id) {
    SenderKey *k = skkey_find(group, sender, id);
    if (k) return k;
    for (int i = 0; i < WC_MAX_SK; i++) if (!G_SK[i].used) {
        memset(&G_SK[i], 0, sizeof G_SK[i]);
        G_SK[i].used = 1;
        snprintf(G_SK[i].group, sizeof G_SK[i].group, "%s", group);
        snprintf(G_SK[i].sender, sizeof G_SK[i].sender, "%s", sender);
        G_SK[i].id = id;
        return &G_SK[i];
    }
    return NULL;
}
static void sk_path(char *out, size_t n) { creds_path("sender.bin", out, n); }
static int sk_save(void) {
    char path[320], tmp[340];
    sk_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fwrite("WCSK", 1, 4, f);
    for (int i = 0; i < WC_MAX_SK; i++) {
        SenderKey *k = &G_SK[i];
        uint8_t used = (uint8_t)k->used;
        fwrite(&used, 1, 1, f);
        if (!used) continue;
        uint8_t gl = (uint8_t)strlen(k->group), sl = (uint8_t)strlen(k->sender);
        fwrite(&gl, 1, 1, f); fwrite(k->group, 1, gl, f);
        fwrite(&sl, 1, 1, f); fwrite(k->sender, 1, sl, f);
        fwrite(&k->id, 4, 1, f);
        fwrite(k->chain, 1, 32, f);
        fwrite(k->sign_priv, 1, 32, f); fwrite(k->sign_pub, 1, 32, f);
        fwrite(&k->index, 4, 1, f);
        for (int j = 0; j < WC_MAX_SKIP; j++) {
            uint8_t u = (uint8_t)k->skipped[j].used;
            fwrite(&u, 1, 1, f);
            if (u) { fwrite(&k->skipped[j].iter, 4, 1, f); fwrite(k->skipped[j].seed, 1, 32, f); }
        }
        uint8_t nd = (uint8_t)k->ndist; fwrite(&nd, 1, 1, f);
        for (int j = 0; j < nd && j < WC_MAX_DIST; j++) {
            uint8_t dl = (uint8_t)strlen(k->dist[j]);
            fwrite(&dl, 1, 1, f); fwrite(k->dist[j], 1, dl, f);
        }
    }
    fclose(f); chmod(tmp, 0600); rename(tmp, path);
    return 0;
}
static int sk_load(void) {
    char path[320];
    sk_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "WCSK", 4)) { fclose(f); return 0; }
    memset(G_SK, 0, sizeof G_SK);
    for (int i = 0; i < WC_MAX_SK; i++) {
        SenderKey *k = &G_SK[i];
        uint8_t used = 0;
        if (fread(&used, 1, 1, f) != 1) break;
        if (!used) continue;
        k->used = 1;
        uint8_t gl = 0; fread(&gl, 1, 1, f);
        if (gl >= sizeof k->group) gl = sizeof k->group - 1;
        fread(k->group, 1, gl, f); k->group[gl] = 0;
        uint8_t sl = 0; fread(&sl, 1, 1, f);
        if (sl >= sizeof k->sender) sl = sizeof k->sender - 1;
        fread(k->sender, 1, sl, f); k->sender[sl] = 0;
        fread(&k->id, 4, 1, f);
        fread(k->chain, 1, 32, f);
        fread(k->sign_priv, 1, 32, f); fread(k->sign_pub, 1, 32, f);
        fread(&k->index, 4, 1, f);
        for (int j = 0; j < WC_MAX_SKIP; j++) {
            uint8_t u = 0; fread(&u, 1, 1, f);
            if (!u) continue;
            k->skipped[j].used = 1;
            fread(&k->skipped[j].iter, 4, 1, f); fread(k->skipped[j].seed, 1, 32, f);
        }
        uint8_t nd = 0; fread(&nd, 1, 1, f);
        if (nd > WC_MAX_DIST) nd = WC_MAX_DIST;
        k->ndist = nd;
        for (int j = 0; j < nd; j++) {
            uint8_t dl = 0; fread(&dl, 1, 1, f);
            if (dl >= sizeof k->dist[j]) dl = sizeof k->dist[j] - 1;
            fread(k->dist[j], 1, dl, f); k->dist[j][dl] = 0;
        }
    }
    fclose(f);
    return 0;
}
static int sk_have_dist(SenderKey *k, const char *addr) {
    for (int i = 0; i < k->ndist; i++) if (!strcmp(k->dist[i], addr)) return 1;
    return 0;
}
static void sk_add_dist(SenderKey *k, const char *addr) {
    if (sk_have_dist(k, addr)) return;
    if (k->ndist >= WC_MAX_DIST) return;
    snprintf(k->dist[k->ndist++], sizeof k->dist[0], "%s", addr);
}

typedef struct { uint8_t pub[32], chain[32]; uint32_t index; } RecvChain;
typedef struct {
    int used;
    char addr[160];
    uint8_t root[32];
    uint8_t send_priv[32], send_pub[32], send_chain[32];
    uint32_t send_index, prev;
    RecvChain recv[8]; int nrecv;
    uint8_t remote_ident[32], base[32];
    int has_pending; uint32_t pk_id, skey_id; uint8_t pending_base[32];
} SigSess;
typedef struct { int used; uint32_t id; uint8_t priv[32]; } PreKey;

static SigSess G_SESS[WC_MAX_SESSIONS];
static PreKey G_PRE[WC_MAX_PREKEYS];
static int G_PRE_N = 0;

static SigSess *sig_find(const char *addr) {
    for (int i = 0; i < WC_MAX_SESSIONS; i++)
        if (G_SESS[i].used && !strcmp(G_SESS[i].addr, addr)) return &G_SESS[i];
    return NULL;
}
static SigSess *sig_alloc(const char *addr) {
    SigSess *s = sig_find(addr);
    if (s) return s;
    for (int i = 0; i < WC_MAX_SESSIONS; i++) {
        if (!G_SESS[i].used) {
            memset(&G_SESS[i], 0, sizeof G_SESS[i]);
            G_SESS[i].used = 1;
            snprintf(G_SESS[i].addr, sizeof G_SESS[i].addr, "%s", addr);
            return &G_SESS[i];
        }
    }
    return NULL;
}
static PreKey *pre_find(uint32_t id) {
    for (int i = 0; i < G_PRE_N; i++) if (G_PRE[i].used && G_PRE[i].id == id) return &G_PRE[i];
    return NULL;
}
static int pre_add(uint32_t id, const uint8_t priv[32]) {
    for (int i = 0; i < WC_MAX_PREKEYS; i++) {
        if (!G_PRE[i].used) {
            G_PRE[i].used = 1; G_PRE[i].id = id; memcpy(G_PRE[i].priv, priv, 32);
            if (i >= G_PRE_N) G_PRE_N = i + 1;
            return 0;
        }
    }
    return -1;
}
static void pre_del(uint32_t id) {
    for (int i = 0; i < WC_MAX_PREKEYS; i++)
        if (G_PRE[i].used && G_PRE[i].id == id) { memset(&G_PRE[i], 0, sizeof G_PRE[i]); return; }
}

static void sig_path(char *out, size_t n) { creds_path("signal.bin", out, n); }
static int sig_save(void) {
    char path[320], tmp[340];
    sig_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fwrite("WCS1", 1, 4, f);
    for (int i = 0; i < WC_MAX_PREKEYS; i++) {
        uint8_t used = (uint8_t)G_PRE[i].used;
        fwrite(&used, 1, 1, f);
        if (used) { fwrite(&G_PRE[i].id, 4, 1, f); fwrite(G_PRE[i].priv, 1, 32, f); }
    }
    for (int i = 0; i < WC_MAX_SESSIONS; i++) {
        uint8_t used = (uint8_t)G_SESS[i].used;
        fwrite(&used, 1, 1, f);
        if (!used) continue;
        SigSess *s = &G_SESS[i];
        uint8_t alen = (uint8_t)strlen(s->addr);
        fwrite(&alen, 1, 1, f); fwrite(s->addr, 1, alen, f);
        fwrite(s->root, 1, 32, f);
        fwrite(s->send_priv, 1, 32, f); fwrite(s->send_pub, 1, 32, f); fwrite(s->send_chain, 1, 32, f);
        fwrite(&s->send_index, 4, 1, f); fwrite(&s->prev, 4, 1, f);
        uint8_t nr = (uint8_t)s->nrecv; fwrite(&nr, 1, 1, f);
        for (int j = 0; j < s->nrecv; j++) {
            fwrite(s->recv[j].pub, 1, 32, f);
            fwrite(s->recv[j].chain, 1, 32, f);
            fwrite(&s->recv[j].index, 4, 1, f);
        }
        fwrite(s->remote_ident, 1, 32, f); fwrite(s->base, 1, 32, f);
        uint8_t hp = (uint8_t)s->has_pending; fwrite(&hp, 1, 1, f);
        if (s->has_pending) {
            fwrite(&s->pk_id, 4, 1, f); fwrite(&s->skey_id, 4, 1, f); fwrite(s->pending_base, 1, 32, f);
        }
    }
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, path);
    return 0;
}
static int sig_load(void) {
    char path[320];
    sig_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "WCS1", 4)) { fclose(f); return 0; }
    memset(G_PRE, 0, sizeof G_PRE); G_PRE_N = 0;
    memset(G_SESS, 0, sizeof G_SESS);
    for (int i = 0; i < WC_MAX_PREKEYS; i++) {
        uint8_t used = 0;
        if (fread(&used, 1, 1, f) != 1) { fclose(f); return 0; }
        if (used) {
            G_PRE[i].used = 1;
            fread(&G_PRE[i].id, 4, 1, f);
            fread(G_PRE[i].priv, 1, 32, f);
            G_PRE_N = i + 1;
        }
    }
    for (int i = 0; i < WC_MAX_SESSIONS; i++) {
        uint8_t used = 0;
        if (fread(&used, 1, 1, f) != 1) break;
        if (!used) continue;
        SigSess *s = &G_SESS[i];
        s->used = 1;
        uint8_t alen = 0; fread(&alen, 1, 1, f);
        if (alen >= sizeof s->addr) alen = sizeof s->addr - 1;
        fread(s->addr, 1, alen, f); s->addr[alen] = 0;
        fread(s->root, 1, 32, f);
        fread(s->send_priv, 1, 32, f); fread(s->send_pub, 1, 32, f); fread(s->send_chain, 1, 32, f);
        fread(&s->send_index, 4, 1, f); fread(&s->prev, 4, 1, f);
        uint8_t nr = 0; fread(&nr, 1, 1, f);
        if (nr > 8) nr = 8;
        s->nrecv = nr;
        for (int j = 0; j < nr; j++) {
            fread(s->recv[j].pub, 1, 32, f);
            fread(s->recv[j].chain, 1, 32, f);
            fread(&s->recv[j].index, 4, 1, f);
        }
        fread(s->remote_ident, 1, 32, f); fread(s->base, 1, 32, f);
        uint8_t hp = 0; fread(&hp, 1, 1, f);
        if (hp) {
            s->has_pending = 1;
            fread(&s->pk_id, 4, 1, f); fread(&s->skey_id, 4, 1, f); fread(s->pending_base, 1, 32, f);
        }
    }
    fclose(f);
    return 1;
}

static void ratchet(const uint8_t root[32], const uint8_t their_pub[32],
                    const uint8_t our_priv[32], uint8_t root2[32], uint8_t chain[32]) {
    uint8_t dh[32], out[64];
    x25519(dh, our_priv, their_pub);
    hkdf256(dh, 32, root, 32, (const uint8_t *)"WhisperRatchet", 14, out, 64);
    memcpy(root2, out, 32);
    memcpy(chain, out + 32, 32);
}
static void msg_keys(const uint8_t seed[32], uint8_t key[32], uint8_t mac[32], uint8_t iv[16]) {
    uint8_t out[80];
    hkdf256(seed, 32, ZERO32, 32, (const uint8_t *)"WhisperMessageKeys", 18, out, 80);
    memcpy(key, out, 32);
    memcpy(mac, out + 32, 32);
    memcpy(iv, out + 64, 16);
}

static int sig_encrypt(const char *jid, const uint8_t *plain, size_t pn,
                       char *type_out, size_t type_n, uint8_t **enc_out, size_t *enc_n) {
    char a[160]; addr_jid(jid, a, sizeof a);
    SigSess *s = sig_find(a);
    if (!s) return -2;
    uint8_t seed[32], key[32], mac[32], iv[16], dh[32];
    uint8_t next_chain[32];
    hmac256(s->send_chain, 32, (const uint8_t *)"\1", 1, seed);
    hmac256(s->send_chain, 32, (const uint8_t *)"\2", 1, next_chain);
    msg_keys(seed, key, mac, iv);
    (void)dh;
    uint8_t *ct = NULL; size_t ctn = 0;
    if (cbc_encrypt(key, iv, plain, pn, &ct, &ctn) != 0) return -1;

    uint8_t pub33[33]; pub33[0] = 5; memcpy(pub33 + 1, s->send_pub, 32);
    B body = {0};
    b_u8(&body, SIG_VERSION);
    pb_bytes(&body, 1, pub33, 33);
    pb_int(&body, 2, s->send_index);
    pb_int(&body, 3, s->prev);
    pb_bytes(&body, 4, ct, ctn);
    free(ct);
    uint8_t ident33[33]; ident33[0] = 5; memcpy(ident33 + 1, C.ident_pub, 32);
    uint8_t remote33[33]; remote33[0] = 5; memcpy(remote33 + 1, s->remote_ident, 32);
    B macbuf = {0};
    b_put(&macbuf, ident33, 33);
    b_put(&macbuf, remote33, 33);
    b_put(&macbuf, body.p, body.n);
    uint8_t fullmac[32];
    hmac256(mac, 32, macbuf.p, macbuf.n, fullmac);
    b_free(&macbuf);
    b_put(&body, fullmac, 8);

    s->send_index++;
    memcpy(s->send_chain, next_chain, 32);

    if (s->has_pending) {
        B pk = {0};
        b_u8(&pk, SIG_VERSION);
        pb_int(&pk, 1, s->pk_id);
        B base33 = {0}; b_u8(&base33, 5); b_put(&base33, s->pending_base, 32);
        pb_bytes(&pk, 2, base33.p, base33.n);
        b_free(&base33);
        pb_bytes(&pk, 3, ident33, 33);
        pb_bytes(&pk, 4, body.p, body.n);
        pb_int(&pk, 5, C.reg_id);
        pb_int(&pk, 6, s->skey_id);
        *enc_out = pk.p; *enc_n = pk.n; pk.p = NULL; b_free(&pk);
        snprintf(type_out, type_n, "pkmsg");
        s->has_pending = 0;
    } else {
        *enc_out = body.p; *enc_n = body.n; body.p = NULL;
        snprintf(type_out, type_n, "msg");
    }
    b_free(&body);
    sig_save();
    return 0;
}

/* Decifra um WhisperMessage dentro de uma sessao. */
static int sig_decrypt_with(SigSess *s, const uint8_t *data, size_t dn, uint8_t **out, size_t *out_n) {
    if (dn < 1 + 8 || data[0] != SIG_VERSION) return -1;
    PB p;
    if (pb_decode(data + 1, dn - 9, &p) != 0) return -1;
    const PBEnt *er = pb_get(&p, 1);
    const PBEnt *ec = pb_get(&p, 4);
    if (!er || er->n != 33 || !ec) return -1;
    uint8_t rk[32]; memcpy(rk, er->p + 1, 32);
    uint64_t counter = pb_num(&p, 2, 0), prev = pb_num(&p, 3, 0);

    RecvChain *rc = NULL;
    for (int i = 0; i < s->nrecv; i++) if (!memcmp(s->recv[i].pub, rk, 32)) { rc = &s->recv[i]; break; }
    if (!rc) {
        if (s->nrecv >= 8) return -1;
        rc = &s->recv[s->nrecv++];
        memcpy(rc->pub, rk, 32);
        uint8_t root1[32], ck[32], newpriv[32], newpub[32], root2[32], sck[32];
        ratchet(s->root, rk, s->send_priv, root1, ck);
        if (random_bytes(newpriv, 32) != 0) return -1;
        uint8_t base[32] = {9}; x25519(newpub, newpriv, base);
        ratchet(root1, rk, newpriv, root2, sck);
        s->prev = s->send_index > 0 ? s->send_index - 1 : 0;
        memcpy(s->root, root2, 32);
        memcpy(s->send_priv, newpriv, 32); memcpy(s->send_pub, newpub, 32); memcpy(s->send_chain, sck, 32);
        s->send_index = 0;
        memcpy(rc->chain, ck, 32); rc->index = 0;
    }
    (void)prev;
    if (counter - rc->index > 2000) return -1;
    while (rc->index < counter) {
        hmac256(rc->chain, 32, (const uint8_t *)"\2", 1, rc->chain);
        rc->index++;
    }
    uint8_t seed[32], key[32], mac[32], iv[16];
    hmac256(rc->chain, 32, (const uint8_t *)"\1", 1, seed);
    hmac256(rc->chain, 32, (const uint8_t *)"\2", 1, rc->chain);
    rc->index++;
    msg_keys(seed, key, mac, iv);

    uint8_t remote33[33]; remote33[0] = 5; memcpy(remote33 + 1, s->remote_ident, 32);
    uint8_t ident33[33]; ident33[0] = 5; memcpy(ident33 + 1, C.ident_pub, 32);
    B mb = {0}; b_put(&mb, remote33, 33); b_put(&mb, ident33, 33);
    b_put(&mb, data, dn - 8);
    uint8_t fullmac[32]; hmac256(mac, 32, mb.p, mb.n, fullmac); b_free(&mb);
    if (memcmp(fullmac, data + dn - 8, 8)) { WC_DBG("[sig] MAC invalido"); return -1; }

    uint8_t *pt = NULL; size_t ptn = 0;
    if (cbc_decrypt(key, iv, ec->p, ec->n, &pt, &ptn) != 0) return -1;
    *out = pt; *out_n = ptn;
    s->has_pending = 0;
    return 0;
}

static int sig_decrypt_prekey(const char *jid, const uint8_t *data, size_t dn, uint8_t **out, size_t *out_n) {
    if (dn < 1 || data[0] != SIG_VERSION) return -1;
    PB p;
    if (pb_decode(data + 1, dn - 1, &p) != 0) return -1;
    const PBEnt *e2 = pb_get(&p, 2);
    const PBEnt *e3 = pb_get(&p, 3);
    const PBEnt *e4 = pb_get(&p, 4);
    if (!e2 || e2->n != 33 || !e3 || e3->n != 33 || !e4) return -1;
    uint8_t base[32], ident[32];
    memcpy(base, e2->p + 1, 32);
    memcpy(ident, e3->p + 1, 32);
    uint32_t pkid = (uint32_t)pb_num(&p, 1, 0);
    uint32_t spkid = (uint32_t)pb_num(&p, 6, 0);
    if (spkid && spkid != C.skey_id) return -1;

    char a[160]; addr_jid(jid, a, sizeof a);
    SigSess *old = sig_find(a);
    if (old && !memcmp(old->base, base, 32)) {
        return sig_decrypt_with(old, e4->p, e4->n, out, out_n);
    }
    PreKey *pk = pkid ? pre_find(pkid) : NULL;
    B sec = {0};
    b_put(&sec, FF32, 32);
    uint8_t dh1[32], dh2[32], dh3[32], dh4[32];
    x25519(dh1, C.skey_priv, ident);
    x25519(dh2, C.ident_priv, base);
    x25519(dh3, C.skey_priv, base);
    b_put(&sec, dh1, 32); b_put(&sec, dh2, 32); b_put(&sec, dh3, 32);
    if (pk) { x25519(dh4, pk->priv, base); b_put(&sec, dh4, 32); }
    uint8_t d[64]; hkdf256(sec.p, sec.n, ZERO32, 32, (const uint8_t *)"WhisperText", 11, d, 64);
    b_free(&sec);

    SigSess *s = sig_alloc(a);
    if (!s) return -1;
    memset(s, 0, sizeof *s);
    s->used = 1; snprintf(s->addr, sizeof s->addr, "%s", a);
    memcpy(s->root, d, 32);
    memcpy(s->send_priv, C.skey_priv, 32);
    memcpy(s->send_pub, C.skey_pub, 32);
    memcpy(s->send_chain, d + 32, 32);
    s->send_index = 0;
    memcpy(s->remote_ident, ident, 32);
    memcpy(s->base, base, 32);

    int r = sig_decrypt_with(s, e4->p, e4->n, out, out_n);
    if (r == 0 && pk) pre_del(pkid);
    if (r != 0) WC_DBG("[sig] pkmsg falhou (pkid=%u prekey=%s)", pkid, pk ? "ok" : "AUSENTE");
    sig_save();
    return r;
}

static int sig_decrypt(const char *jid, const char *type, const uint8_t *data, size_t dn,
                       uint8_t **out, size_t *out_n) {
    if (!strcmp(type, "pkmsg")) return sig_decrypt_prekey(jid, data, dn, out, out_n);
    if (!strcmp(type, "msg")) {
        char a[160]; addr_jid(jid, a, sizeof a);
        SigSess *s = sig_find(a);
        if (!s) { WC_DBG("[sig] sem sessao para %s", a); return -1; }
        return sig_decrypt_with(s, data, dn, out, out_n);
    }
    WC_DBG("[sig] tipo enc nao suportado: %s", type ? type : "?");
    return -1;
}

/* Guarda um SenderKeyDistributionMessage encontrado dentro de uma Message. */
static int skdm_store(const uint8_t *plain, size_t pn, const char *sender_addr) {
    PB m;
    if (pb_decode(plain, pn, &m) != 0) return -1;
    const PBEnt *sdm = pb_get(&m, 2); /* senderKeyDistributionMessage */
    if (!sdm || sdm->wt != 2) return -1;
    PB w;
    if (pb_decode(sdm->p, sdm->n, &w) != 0) return -1;
    const PBEnt *g = pb_get(&w, 1), *ax = pb_get(&w, 2);
    if (!g || g->wt != 2 || !ax || ax->wt != 2) return -1;
    char group[160];
    size_t gl = g->n < sizeof group - 1 ? g->n : sizeof group - 1;
    memcpy(group, g->p, gl); group[gl] = 0;
    if (ax->n < 2 || ax->p[0] != SIG_VERSION) return -1;
    PB sk;
    if (pb_decode(ax->p + 1, ax->n - 1, &sk) != 0) return -1;
    const PBEnt *idb = pb_get(&sk, 1), *idx = pb_get(&sk, 2), *chain = pb_get(&sk, 3);
    if (!idb || !chain || chain->n < 32) return -1;
    SenderKey *k = sk_alloc(group, sender_addr, (uint32_t)idb->num);
    if (!k) return -1;
    memcpy(k->chain, chain->p, 32);
    k->index = idx ? (uint32_t)idx->num : 0;
    const PBEnt *sp = pb_get(&sk, 4);
    if (sp && sp->n >= 33) memcpy(k->sign_pub, sp->p + (sp->p[0] == 5 ? 1 : 0), 32);
    WC_DBG("[sig] SKDM recebida grupo=%s de=%s id=%u", group, sender_addr, (uint32_t)idb->num);
    sk_save();
    return 0;
}

/* Decifra um <enc type="skmsg">. O conteudo e body + 64 bytes de assinatura. */
static int sk_decrypt(const char *group, const char *sender_addr,
                      const uint8_t *data, size_t dn, uint8_t **out, size_t *out_n) {
    if (dn < 65 || data[0] != SIG_VERSION) return -1;
    PB p;
    if (pb_decode(data + 1, dn - 65, &p) != 0) return -1;
    uint32_t id = (uint32_t)pb_num(&p, 1, 0), iter = (uint32_t)pb_num(&p, 2, 0);
    const PBEnt *ct = pb_get(&p, 3);
    if (!ct) return -1;
    SenderKey *k = skkey_find(group, sender_addr, id);
    if (!k) { WC_DBG("[sig] sem sender key de %s no grupo %s (id=%u)", sender_addr, group, id); return -1; }
    uint8_t seed[32];
    if (iter < k->index) {
        int found = 0;
        for (int j = 0; j < WC_MAX_SKIP; j++)
            if (k->skipped[j].used && k->skipped[j].iter == iter) {
                memcpy(seed, k->skipped[j].seed, 32);
                memset(&k->skipped[j], 0, sizeof k->skipped[j]);
                found = 1; break;
            }
        if (!found) { WC_DBG("[sig] mensagem de grupo repetida (iter=%u)", iter); return -1; }
    } else {
        if (iter - k->index > 2000) return -1;
        while (k->index < iter) {
            int slot = -1;
            for (int j = 0; j < WC_MAX_SKIP; j++) if (!k->skipped[j].used) { slot = j; break; }
            if (slot < 0) { slot = 0; }
            hmac256(k->chain, 32, (const uint8_t *)"\1", 1, k->skipped[slot].seed);
            k->skipped[slot].used = 1; k->skipped[slot].iter = k->index++;
            hmac256(k->chain, 32, (const uint8_t *)"\2", 1, k->chain);
        }
        hmac256(k->chain, 32, (const uint8_t *)"\1", 1, seed);
        hmac256(k->chain, 32, (const uint8_t *)"\2", 1, k->chain);
        k->index++;
    }
    uint8_t kk[48];
    hkdf256(seed, 32, ZERO32, 32, (const uint8_t *)"WhisperGroup", 12, kk, 48);
    int r = aes_cbc(0, kk + 16, kk, ct->p, ct->n, out, out_n);
    sk_save();
    return r;
}

/* Bundle de prekeys recebido do servidor. */
typedef struct {
    int ok, has_pk;
    uint8_t ident[32], skey_pub[32], pk_pub[32];
    uint32_t skey_id, reg_id, pk_id;
} Bundle;
static void sig_build_out(const char *jid, const Bundle *b) {
    char a[160]; addr_jid(jid, a, sizeof a);
    uint8_t eph_priv[32], eph_pub[32], spriv[32], spub[32];
    if (random_bytes(eph_priv, 32) != 0 || random_bytes(spriv, 32) != 0) return;
    uint8_t base[32] = {9};
    x25519(eph_pub, eph_priv, base);
    x25519(spub, spriv, base);
    B sec = {0};
    b_put(&sec, FF32, 32);
    uint8_t dh1[32], dh2[32], dh3[32], dh4[32];
    x25519(dh1, C.ident_priv, b->skey_pub);
    x25519(dh2, eph_priv, b->ident);
    x25519(dh3, eph_priv, b->skey_pub);
    b_put(&sec, dh1, 32); b_put(&sec, dh2, 32); b_put(&sec, dh3, 32);
    if (b->has_pk) { x25519(dh4, eph_priv, b->pk_pub); b_put(&sec, dh4, 32); }
    uint8_t d[64]; hkdf256(sec.p, sec.n, ZERO32, 32, (const uint8_t *)"WhisperText", 11, d, 64);
    b_free(&sec);

    SigSess *s = sig_alloc(a);
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->used = 1; snprintf(s->addr, sizeof s->addr, "%s", a);
    uint8_t root2[32], sck[32];
    ratchet(d, b->skey_pub, spriv, root2, sck);
    memcpy(s->root, root2, 32);
    memcpy(s->send_priv, spriv, 32); memcpy(s->send_pub, spub, 32);
    memcpy(s->send_chain, sck, 32); s->send_index = 0;
    memcpy(s->remote_ident, b->ident, 32);
    memcpy(s->base, eph_pub, 32);
    s->nrecv = 1;
    memcpy(s->recv[0].pub, b->skey_pub, 32);
    memcpy(s->recv[0].chain, d + 32, 32);
    s->recv[0].index = 0;
    s->has_pending = 1;
    s->pk_id = b->pk_id; s->skey_id = b->skey_id;
    memcpy(s->pending_base, eph_pub, 32);
    sig_save();
}

/* ------------------------------------------------------------------ nos NG */
static NGInterpreter *G_INTERP = NULL;

static NGTable *node_new(const char *tag) {
    NGTable *t = ng_table_new(8);
    if (!t) return NULL;
    ng_table_set(t, "tag", ng_val_string(tag));
    ng_table_set(t, "attrs", ng_val_table(ng_table_new(8)));
    return t;
}
static void node_attr(NGTable *n, const char *k, const char *v) {
    NGValue at;
    if (!n || !ng_table_get(n, "attrs", &at) || at.type != NG_VAL_TABLE) return;
    ng_table_set(at.as.table, k, ng_val_string(v ? v : ""));
}
static void node_attr_num(NGTable *n, const char *k, long long v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    node_attr(n, k, b);
}
static void node_bin(NGTable *n, const uint8_t *p, size_t k) {
    char *h = tohex(p, k);
    ng_table_set(n, "content", ng_val_string_take(h ? h : strdup("")));
    ng_table_set(n, "binario", ng_val_bool(true));
}
static NGTable *arr_new(size_t n) {
    NGTable *a = ng_table_new(8);
    if (!a) return NULL;
    ng_table_set(a, "_is_array", ng_val_bool(true));
    ng_table_set(a, "length", ng_val_number((double)n));
    return a;
}
static void arr_set(NGTable *a, size_t i, NGTable *child) {
    char k[24];
    snprintf(k, sizeof k, "%zu", i);
    ng_table_set(a, k, ng_val_table(child));
}
static NGTable *tab_get(NGTable *t, const char *k) {
    NGValue v;
    if (t && ng_table_get(t, k, &v) && v.type == NG_VAL_TABLE) return v.as.table;
    return NULL;
}
static const char *tab_str(NGTable *t, const char *k) {
    NGValue v;
    if (t && ng_table_get(t, k, &v) && v.type == NG_VAL_STRING) return v.as.string;
    return NULL;
}
static int tab_num(NGTable *t, const char *k, int def) {
    NGValue v;
    if (t && ng_table_get(t, k, &v) && v.type == NG_VAL_NUMBER) return (int)v.as.number;
    return def;
}
static int tab_bool(NGTable *t, const char *k) {
    NGValue v;
    if (!t || !ng_table_get(t, k, &v)) return 0;
    if (v.type == NG_VAL_BOOL) return v.as.boolean ? 1 : 0;
    if (v.type == NG_VAL_NUMBER) return v.as.number != 0;
    return 0;
}
static size_t tab_len(NGTable *t) {
    int n = tab_num(t, "length", 0);
    return n > 0 ? (size_t)n : 0;
}
static NGTable *tab_child(NGTable *t, size_t i) {
    char k[24]; snprintf(k, sizeof k, "%zu", i);
    return tab_get(t, k);
}
/* Tabela que contem os filhos de um no (content) ou a propria tabela se
 * ela ja for um vetor (length + chaves "0", "1", ...). */
static NGTable *tab_children(NGTable *t) {
    if (!t) return NULL;
    NGTable *c = tab_get(t, "content");
    if (c) return c;
    NGValue v;
    if (ng_table_get(t, "_is_array", &v) && v.type == NG_VAL_BOOL && v.as.boolean) return t;
    if (tab_num(t, "length", -1) >= 0) return t;
    return NULL;
}
static NGTable *tab_child_tag(NGTable *t, const char *tag) {
    NGTable *arr = tab_children(t);
    size_t n = tab_len(arr);
    for (size_t i = 0; i < n; i++) {
        NGTable *c = tab_child(arr, i);
        const char *ct = tab_str(c, "tag");
        if (ct && !strcmp(ct, tag)) return c;
    }
    return NULL;
}

/* Chama uma funcao do modulo @zap. O chamador libera o retorno. O @zap
 * registra o global "Zap" (com maiuscula), por isso nao usamos o invoke
 * generico, que procura um global com o mesmo nome do modulo. */
static NGValue zap_call(const char *fn, NGValue *args, size_t argc) {
    if (!G_INTERP) return ng_val_null();
    ng_native_module_load("zap", G_INTERP);
    NGValue mod;
    if (!ng_env_get(G_INTERP->globals, "Zap", &mod) || mod.type != NG_VAL_TABLE) return ng_val_null();
    NGValue f;
    if (!ng_table_get(mod.as.table, fn, &f) || f.type != NG_VAL_NATIVE_FUNC) return ng_val_null();
    return ng_interp_call_func(G_INTERP, &f, args, argc);
}
static int zap_start(const uint8_t priv[32]) {
    NGValue r;
    if (priv) {
        char *h = tohex(priv, 32);
        NGValue a = ng_val_string(h ? h : "");
        free(h);
        r = zap_call("Iniciar", &a, 1);
        ng_val_free(&a);
    } else {
        r = zap_call("Iniciar", NULL, 0);
    }
    int id = r.type == NG_VAL_NUMBER ? (int)r.as.number : 0;
    ng_val_free(&r);
    return id;
}
static int zap_frame_initial(int h, uint8_t **out, size_t *out_n) {
    NGValue a = ng_val_number(h);
    NGValue r = zap_call("QuadroInicial", &a, 1);
    int ok = 0;
    if (r.type == NG_VAL_STRING) { *out = unhex(r.as.string, out_n); ok = *out != NULL; }
    ng_val_free(&r);
    return ok ? 0 : -1;
}
static int zap_handshake(int h, const uint8_t *body, size_t bn,
                         const uint8_t priv[32], const uint8_t pub[32],
                         const uint8_t *payload, size_t pn, uint8_t **out, size_t *out_n) {
    char *hb = tohex(body, bn), *hp = tohex(priv, 32), *hu = tohex(pub, 32), *hy = tohex(payload, pn);
    NGValue a[5];
    a[0] = ng_val_number(h);
    a[1] = ng_val_string(hb ? hb : "");
    a[2] = ng_val_string(hp ? hp : "");
    a[3] = ng_val_string(hu ? hu : "");
    a[4] = ng_val_string(hy ? hy : "");
    NGValue r = zap_call("ProcessarServidor", a, 5);
    int ok = 0;
    if (r.type == NG_VAL_STRING) { *out = unhex(r.as.string, out_n); ok = *out != NULL; }
    for (int i = 0; i < 5; i++) ng_val_free(&a[i]);
    ng_val_free(&r);
    free(hb); free(hp); free(hu); free(hy);
    return ok ? 0 : -1;
}
static int zap_encrypt_node(int h, NGTable *node, uint8_t **out, size_t *out_n) {
    NGValue a[2];
    a[0] = ng_val_number(h);
    a[1] = ng_val_table(node);
    NGValue r = zap_call("Cifrar", a, 2);
    int ok = 0;
    if (r.type == NG_VAL_STRING) { *out = unhex(r.as.string, out_n); ok = *out != NULL; }
    ng_val_free(&a[1]);
    ng_val_free(&r);
    return ok ? 0 : -1;
}
static NGTable *zap_decrypt_node(int h, const uint8_t *body, size_t bn) {
    char *hb = tohex(body, bn);
    NGValue a[2];
    a[0] = ng_val_number(h);
    a[1] = ng_val_string(hb ? hb : "");
    NGValue r = zap_call("Decifrar", a, 2);
    ng_val_free(&a[0]); ng_val_free(&a[1]);
    free(hb);
    if (r.type != NG_VAL_TABLE) { ng_val_free(&r); return NULL; }
    return r.as.table; /* transferido para o chamador */
}
static void zap_close(int h) {
    if (h <= 0) return;
    NGValue a = ng_val_number(h);
    NGValue r = zap_call("Fechar", &a, 1);
    ng_val_free(&a); ng_val_free(&r);
}

/* ------------------------------------------------------------------ estado */
typedef struct {
    int used;
    char chat[160], id[96], sender[160], autor[64], nome[128], texto[4096], botao_id[128];
    int grupo;
} Msg;
typedef struct {
    int used, stage, kind; /* kind 0=texto, 1=botoes */
    int is_group;
    char addr_mode[64];
    char to[160], to_base[160];
    char texto[4096];
    int nbtn;
    char btn_id[3][128], btn_titulo[3][64];
    int ntargets;
    char targets[WC_MAX_TARGETS][160];
} Out;

static int W_ATIVO = 0, W_WS = 0, W_ZAP = 0, W_HANDSHAKE = 0;
static char W_ESTADO[32] = "desconectado";
static char W_ERRO[256] = "";
static char W_QR[1024] = "";
static int W_QR_IDX = 0;
static uint64_t W_NEXT_CONNECT = 0, W_NEXT_PING = 0;
/* Backoff exponencial da reconexao: 3s, 6s, 12s... ate 60s. Zera quando logado. */
static uint64_t W_BACKOFF_MS = 3000;
static void wc_reagendar(void) {
    W_NEXT_CONNECT = wc_now_ms() + W_BACKOFF_MS;
    W_BACKOFF_MS = W_BACKOFF_MS < 60000 ? W_BACKOFF_MS * 2 : 60000;
}
static uint64_t W_NEXT_PRESENCE = 0;   /* 0 = ainda nao logado */
static int W_STOP = 0;
static uint8_t W_PAYLOAD[2048];
static size_t W_PAYLOAD_N = 0;
static Msg W_MSGS[WC_MAX_MSGS];
static int W_MSG_N = 0;
static Out W_OUT[WC_MAX_OUT];
static char W_QUERY[64] = "";
static int W_QUERY_KIND = 0, W_QUERY_OUT = -1;

static int wc_debug_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("NG_WACORE_DEBUG") != NULL;
    return v;
}
static void st_set(const char *s) { snprintf(W_ESTADO, sizeof W_ESTADO, "%s", s); }
static void err_set(const char *s) { snprintf(W_ERRO, sizeof W_ERRO, "%s", s ? s : ""); }
static void ws_send_raw(const uint8_t *p, size_t n) {
    if (W_WS > 0 && ng_ws_c_open(W_WS)) ng_ws_c_send_binary(W_WS, p, n);
}
static int send_node(NGTable *n) {
    if (!W_ZAP) return -1;
    uint8_t *frame = NULL; size_t fn = 0;
    if (zap_encrypt_node(W_ZAP, n, &frame, &fn) != 0) return -1;
    ws_send_raw(frame, fn);
    free(frame);
    return 0;
}
static void make_query_id(char *out, size_t n) {
    static uint32_t q = 0;
    uint8_t r[4]; random_bytes(r, 4);
    snprintf(out, n, "wq%u%02x%02x%02x%02x", ++q, r[0], r[1], r[2], r[3]);
}
static void new_msg_id(char *out, size_t n) {
    uint8_t r[9]; random_bytes(r, 9);
    char *h = tohex(r, 9);
    snprintf(out, n, "3EB0%s", h ? h : "000000000000000000");
    free(h);
    for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
}

static void wc_close_conn(void) {
    if (W_WS > 0) { ng_ws_c_close(W_WS); W_WS = 0; }
    if (W_ZAP > 0) { zap_close(W_ZAP); W_ZAP = 0; }
    W_HANDSHAKE = 0;
    /* Consultas em voo nao sobrevivem a queda: reenfileira os envios. */
    W_QUERY[0] = 0; W_QUERY_KIND = 0; W_QUERY_OUT = -1;
    for (int i = 0; i < WC_MAX_OUT; i++)
        if (W_OUT[i].used && W_OUT[i].stage != 3) W_OUT[i].stage = 0;
}
/* ------------------------------------------------------------------ mensagens */
static int node_binval(NGTable *n, const uint8_t **p, size_t *pn) {
    const char *s = tab_str(n, "content");
    if (!s) { *p = NULL; *pn = 0; return -1; }
    if (tab_bool(n, "binario")) {
        uint8_t *q = unhex(s, pn);
        if (!q) return -1;
        *p = q;
        return 1; /* precisa free */
    }
    *p = (const uint8_t *)s;
    *pn = strlen(s);
    return 0;
}
static int pb_copy_text(const PBEnt *e, char *out, size_t outn) {
    if (!e || !e->p || e->n == 0 || outn == 0) return 0;
    size_t k = e->n < outn - 1 ? e->n : outn - 1;
    memcpy(out, e->p, k);
    out[k] = 0;
    return out[0] != 0;
}

static int extract_text(const uint8_t *p, size_t n, char *out, size_t outn, char *botao, size_t botaon) {
    if (botao && botaon) botao[0] = 0;
    out[0] = 0;
    PB m;
    if (pb_decode(p, n, &m) != 0) return -1;
    const PBEnt *e;

    /* Mensagens embrulhadas: ephemeral(24), viewOnce(25), deviceSent(31). */
    {
        int wrappers[3] = { 24, 25, 31 };
        for (int wi = 0; wi < 3; wi++) {
            const PBEnt *w = pb_get(&m, wrappers[wi]);
            if (!w || w->wt != 2) continue;
            PB sub;
            if (pb_decode(w->p, w->n, &sub) != 0) continue;
            const PBEnt *inner = pb_get(&sub, 1);
            if (!inner || inner->wt != 2) continue;
            if (extract_text(inner->p, inner->n, out, outn, botao, botaon) == 0 && out[0]) return 0;
        }
    }

    /* Respostas de botoes/listas vem primeiro: alguns clientes incluem um
       corpo de texto (campo 1) junto da resposta, e o ID precisa prevalecer. */

    /* buttonsResponseMessage (43): selectedButtonId(1), selectedDisplayText(2) */
    e = pb_get(&m, 43);
    if (e) {
        PB x;
        if (pb_decode(e->p, e->n, &x) == 0) {
            if (pb_copy_text(pb_get(&x, 1), out, outn)) {
                if (botao) snprintf(botao, botaon, "%s", out);
                return 0;
            }
            if (pb_copy_text(pb_get(&x, 2), out, outn)) return 0;
        }
    }
    /* templateButtonReplyMessage (29): selectedId(1), selectedDisplayText(2) */
    e = pb_get(&m, 29);
    if (e) {
        PB x;
        if (pb_decode(e->p, e->n, &x) == 0) {
            if (pb_copy_text(pb_get(&x, 1), out, outn)) {
                if (botao) snprintf(botao, botaon, "%s", out);
                return 0;
            }
            if (pb_copy_text(pb_get(&x, 2), out, outn)) return 0;
        }
    }
    /* listResponseMessage (39): title(2), singleSelectReply(3)->selectedRowId(1) */
    e = pb_get(&m, 39);
    if (e) {
        PB x, y;
        if (pb_decode(e->p, e->n, &x) == 0) {
            const PBEnt *s = pb_get(&x, 3);
            if (s && pb_decode(s->p, s->n, &y) == 0) {
                if (pb_copy_text(pb_get(&y, 1), out, outn)) {
                    if (botao) snprintf(botao, botaon, "%s", out);
                    return 0;
                }
            }
            if (pb_copy_text(pb_get(&x, 2), out, outn)) return 0;
        }
    }
    /* interactiveResponseMessage (48): nativeFlowResponseMessage(2)->paramsJson(2) */
    e = pb_get(&m, 48);
    if (e) {
        PB x, y;
        if (pb_decode(e->p, e->n, &x) == 0) {
            const PBEnt *nf = pb_get(&x, 2);
            if (nf && pb_decode(nf->p, nf->n, &y) == 0) {
                const PBEnt *pj = pb_get(&y, 2);
                if (pj) {
                    char tmp[1024];
                    size_t k = pj->n < sizeof tmp - 1 ? pj->n : sizeof tmp - 1;
                    memcpy(tmp, pj->p, k); tmp[k] = 0;
                    const char *q = strstr(tmp, "\"id\"");
                    if (q) {
                        q = strchr(q, ':');
                        if (q) {
                            q++;
                            while (*q && (*q == ' ' || *q == '"')) q++;
                            size_t z = 0;
                            while (q[z] && q[z] != '"' && z + 1 < outn) { out[z] = q[z]; z++; }
                            out[z] = 0;
                            if (botao && out[0]) snprintf(botao, botaon, "%s", out);
                        }
                    }
                }
            }
            if (out[0]) return 0;
            const PBEnt *b = pb_get(&x, 1);
            if (b) {
                PB z;
                if (pb_decode(b->p, b->n, &z) == 0) {
                    if (pb_copy_text(pb_get(&z, 1), out, outn)) return 0;
                }
            }
        }
    }

    /* Texto comum: conversation (1) e extendedTextMessage.text (6.1). */
    e = pb_get(&m, 1);
    if (pb_copy_text(e, out, outn)) return 0;
    e = pb_get(&m, 6);
    if (e) {
        PB x;
        if (pb_decode(e->p, e->n, &x) == 0) {
            if (pb_copy_text(pb_get(&x, 1), out, outn)) return 0;
        }
    }
    return 0;
}

static void msg_push(const char *chat, const char *id, const char *sender, const char *nome,
                     const char *texto, const char *botao, int grupo) {
    int idx = -1;
    for (int i = 0; i < WC_MAX_MSGS; i++) if (!W_MSGS[i].used) { idx = i; break; }
    if (idx < 0) {
        memmove(W_MSGS, W_MSGS + 1, sizeof(Msg) * (WC_MAX_MSGS - 1));
        idx = WC_MAX_MSGS - 1;
    }
    Msg *m = &W_MSGS[idx];
    memset(m, 0, sizeof *m);
    m->used = 1;
    snprintf(m->chat, sizeof m->chat, "%s", chat ? chat : "");
    snprintf(m->id, sizeof m->id, "%s", id ? id : "");
    snprintf(m->sender, sizeof m->sender, "%s", sender ? sender : "");
    {
        char u[160], sv[96]; uint32_t d;
        split_jid(sender ? sender : "", u, sizeof u, sv, sizeof sv, &d);
        snprintf(m->autor, sizeof m->autor, "%s", u);
    }
    snprintf(m->nome, sizeof m->nome, "%s", nome ? nome : "");
    snprintf(m->texto, sizeof m->texto, "%s", texto ? texto : "");
    snprintf(m->botao_id, sizeof m->botao_id, "%s", botao ? botao : "");
    m->grupo = grupo;
    W_MSG_N++;
}

static void send_ack(NGTable *orig, const char *id, const char *to) {
    if (!id || !*id || !to) return;
    NGTable *ack = node_new("ack");
    node_attr(ack, "id", id);
    node_attr(ack, "to", to);
    node_attr(ack, "class", tab_str(orig, "tag"));
    NGTable *at = tab_get(orig, "attrs");
    const char *part = tab_str(at, "participant");
    if (part) node_attr(ack, "participant", part);
    send_node(ack);
    ng_table_release(ack);
}

/* Pedido de reenvio: quando nao conseguimos decifrar, o remetente reenvia
   a mensagem junto com a SenderKeyDistributionMessage que nos faltava. */
#define WC_MAX_RETRY 16
static char W_RETRY_ID[WC_MAX_RETRY][96];
static int W_RETRY_N[WC_MAX_RETRY];
static int retry_allowed(const char *id) {
    if (!id || !*id) return 0;
    for (int i = 0; i < WC_MAX_RETRY; i++) {
        if (!strcmp(W_RETRY_ID[i], id)) {
            if (W_RETRY_N[i] >= 2) return 0;
            W_RETRY_N[i]++;
            return 1;
        }
    }
    for (int i = 0; i < WC_MAX_RETRY; i++) if (!W_RETRY_ID[i][0]) {
        snprintf(W_RETRY_ID[i], sizeof W_RETRY_ID[i], "%s", id);
        W_RETRY_N[i] = 1;
        return 1;
    }
    return 0;
}
static void send_retry(NGTable *n, const char *from, const char *participant,
                       const char *recipient, const char *msg_id) {
    NGTable *at = tab_get(n, "attrs");
    if (!retry_allowed(msg_id)) return;
    NGTable *r = node_new("receipt");
    node_attr(r, "id", msg_id);
    node_attr(r, "type", "retry");
    node_attr(r, "to", from);
    if (participant && *participant) node_attr(r, "participant", participant);
    if (recipient && *recipient) node_attr(r, "recipient", recipient);

    NGTable *retry = node_new("retry");
    node_attr(retry, "count", "1");
    node_attr(retry, "id", tab_str(at, "id") ? tab_str(at, "id") : msg_id);
    node_attr(retry, "t", tab_str(at, "t") ? tab_str(at, "t") : "0");
    node_attr(retry, "v", "1");
    node_attr(retry, "error", "0");

    uint8_t reg[4] = { (uint8_t)(C.reg_id >> 24), (uint8_t)(C.reg_id >> 16),
                       (uint8_t)(C.reg_id >> 8), (uint8_t)C.reg_id };
    NGTable *regn = node_new("registration");
    node_bin(regn, reg, 4);

    NGTable *a = arr_new(2);
    arr_set(a, 0, retry);
    arr_set(a, 1, regn);
    ng_table_set(r, "content", ng_val_table(a));
    send_node(r);
    ng_table_release(r);
    WC_DBG("[msg] retry enviado para %s (msg=%s)", from, msg_id);
}

static void on_message(NGTable *n) {
    NGTable *at = tab_get(n, "attrs");
    const char *id = tab_str(at, "id");
    const char *from = tab_str(at, "from");
    const char *participant = tab_str(at, "participant");
    const char *notify = tab_str(at, "notify");
    if (!from) return;
    const char *sender = participant ? participant : from;
    int grupo = is_group_jid(from);

    char group[160], sender_addr[160], sender_base[160];
    base_jid(from, group, sizeof group);
    addr_jid(sender, sender_addr, sizeof sender_addr);
    base_jid(sender, sender_base, sizeof sender_base);

    uint8_t *chosen = NULL;
    size_t chosen_n = 0;
    int chosen_sk = 0;

    NGTable *content = tab_get(n, "content");
    size_t nk = tab_len(content);
    for (size_t i = 0; i < nk; i++) {
        NGTable *c = tab_child(content, i);
        if (!c) continue;
        const char *child_tag = tab_str(c, "tag");
        WC_DBG("[msg] filho[%zu] tag=%s", i, child_tag ? child_tag : "-");
        if (!child_tag || strcmp(child_tag, "enc")) continue;
        NGTable *ea = tab_get(c, "attrs");
        const char *enc_type = tab_str(ea, "type");
        const char *enc_hex = tab_str(c, "content");
        if (!enc_type || !enc_hex) {
            WC_DBG("[msg] enc sem tipo/conteudo (tipo=%s tam=%zu)", enc_type ? enc_type : "-", enc_hex ? strlen(enc_hex) : 0);
            continue;
        }
        size_t dn = 0;
        uint8_t *enc = unhex(enc_hex, &dn);
        if (!enc) continue;
        WC_DBG("[msg] enc[%zu] from=%s participant=%s tipo=%s grupo=%d",
               i, from, participant ? participant : "-", enc_type, grupo);

        uint8_t *plain = NULL;
        size_t pn = 0;
        int r = -1;
        if (!strcmp(enc_type, "pkmsg") || !strcmp(enc_type, "msg")) {
            /* Tenta todos os enderecos possiveis: o remetente pode vir como LID,
               como numero (sender_pn) ou sem sufixo de dispositivo. */
            const char *cand[4];
            size_t nc = 0;
            cand[nc++] = sender_addr;
            cand[nc++] = sender_base;
            const char *pn_jid = tab_str(at, "sender_pn");
            const char *rcpt = tab_str(at, "recipient");
            if (pn_jid && *pn_jid) cand[nc++] = pn_jid;
            if (rcpt && *rcpt) cand[nc++] = rcpt;
            for (size_t ci = 0; ci < nc; ci++) {
                r = sig_decrypt(cand[ci], enc_type, enc, dn, &plain, &pn);
                if (r == 0 && plain) { WC_DBG("[msg] decifrado com addr=%s", cand[ci]); break; }
            }
            if (r == 0 && plain) skdm_store(plain, pn, sender_addr);
        } else if (!strcmp(enc_type, "skmsg")) {
            const char *sc[3]; size_t nsc = 0;
            sc[nsc++] = sender_addr;
            sc[nsc++] = sender_base;
            const char *pn_jid = tab_str(at, "sender_pn");
            if (pn_jid && *pn_jid) sc[nsc++] = pn_jid;
            for (size_t si = 0; si < nsc; si++) {
                r = sk_decrypt(group, sc[si], enc, dn, &plain, &pn);
                if (r == 0 && plain) break;
            }
        } else {
            WC_DBG("[msg] enc tipo nao suportado: %s", enc_type);
        }
        free(enc);
        if (r != 0 || !plain) {
            WC_DBG("[msg] falha ao decifrar enc[%zu] (%s)", i, enc_type);
            continue;
        }
        if (!chosen || !chosen_sk) {
            free(chosen);
            chosen = plain; chosen_n = pn; chosen_sk = !strcmp(enc_type, "skmsg");
            if (chosen_sk) break; /* a versao do grupo prevalece */
        } else {
            free(plain);
        }
    }

    if (!chosen) {
        WC_DBG("[msg] nenhum enc decifravel -> pedindo reenvio");
        send_retry(n, from, participant, tab_str(at, "recipient"), id);
        send_ack(n, id, from);
        return;
    }
    char texto[4096] = "", botao[128] = "";
    extract_text(chosen, chosen_n, texto, sizeof texto, botao, sizeof botao);
    free(chosen);
    if (!texto[0]) {
        WC_DBG("[msg] texto vazio -> descartada");
        send_ack(n, id, from);
        return;
    }

    char chat[160];
    base_jid(from, chat, sizeof chat);
    /* Mensagens enviadas para nos mesmos: o destinatario real vem em recipient. */
    const char *me_jid = C.jid, *me_lid = C.lid;
    char me_base[160], me_lid_base[160];
    if (me_jid && *me_jid) base_jid(me_jid, me_base, sizeof me_base); else me_base[0] = 0;
    if (me_lid && *me_lid) base_jid(me_lid, me_lid_base, sizeof me_lid_base); else me_lid_base[0] = 0;
    if ((me_base[0] && !strcmp(chat, me_base)) || (me_lid_base[0] && !strcmp(chat, me_lid_base))) {
        const char *rcpt = tab_str(at, "recipient");
        if (rcpt && *rcpt) { base_jid(rcpt, chat, sizeof chat); }
    }
    WC_DBG("[msg] OK chat=%s texto=%s", chat, texto);
    msg_push(chat, id, sender, notify, texto, botao, grupo);
    send_ack(n, id, from);
}

/* ------------------------------------------------------------------ consultas */
static int copy_pub32(uint8_t dst[32], const uint8_t *p, size_t pn) {
    if (pn == 32) { memcpy(dst, p, 32); return 1; }
    if (pn == 33 && p[0] == 5) { memcpy(dst, p + 1, 32); return 1; }
    return 0;
}
static Bundle parse_bundle(NGTable *u) {
    Bundle b;
    memset(&b, 0, sizeof b);
    NGTable *skey = tab_child_tag(u, "skey");
    NGTable *ident = tab_child_tag(u, "identity");
    if (!skey || !ident) return b;
    const uint8_t *p = NULL; size_t pn = 0;
    int owned = node_binval(ident, &p, &pn);
    copy_pub32(b.ident, p, pn);
    if (owned == 1) free((void *)p);
    NGTable *kid = tab_child_tag(skey, "id");
    NGTable *kval = tab_child_tag(skey, "value");
    if (kid) {
        owned = node_binval(kid, &p, &pn);
        if (pn >= 3) b.skey_id = ((uint32_t)p[pn-3] << 16) | ((uint32_t)p[pn-2] << 8) | p[pn-1];
        if (owned == 1) free((void *)p);
    }
    if (kval) {
        owned = node_binval(kval, &p, &pn);
        copy_pub32(b.skey_pub, p, pn);
        if (owned == 1) free((void *)p);
    }
    NGTable *reg = tab_child_tag(u, "registration");
    if (reg) {
        owned = node_binval(reg, &p, &pn);
        if (pn >= 4) b.reg_id = ((uint32_t)p[pn-4] << 24) | ((uint32_t)p[pn-3] << 16) | ((uint32_t)p[pn-2] << 8) | p[pn-1];
        if (owned == 1) free((void *)p);
    }
    NGTable *key = tab_child_tag(u, "key");
    if (key) {
        NGTable *pkid = tab_child_tag(key, "id");
        NGTable *pkval = tab_child_tag(key, "value");
        if (pkid) {
            owned = node_binval(pkid, &p, &pn);
            if (pn >= 3) b.pk_id = ((uint32_t)p[pn-3] << 16) | ((uint32_t)p[pn-2] << 8) | p[pn-1];
            if (owned == 1) free((void *)p);
        }
        if (pkval) {
            owned = node_binval(pkval, &p, &pn);
            if (copy_pub32(b.pk_pub, p, pn)) b.has_pk = 1;
            if (owned == 1) free((void *)p);
        }
    }
    b.ok = 0;
    for (int i = 0; i < 32; i++) if (b.ident[i]) b.ok = 1;
    for (int i = 0; i < 32; i++) if (b.skey_pub[i]) b.ok = 1;
    return b;
}
/* Participantes do grupo (iq xmlns=w:g2) -> lista de JIDs base. */
static void on_group_result(NGTable *n) {
    if (W_QUERY_OUT < 0 || W_QUERY_OUT >= WC_MAX_OUT) return;
    Out *o = &W_OUT[W_QUERY_OUT];
    if (!o->used) return;
    NGTable *g = tab_child_tag(n, "group");
    NGTable *at = g ? tab_get(g, "attrs") : NULL;
    if (at) {
        const char *am = tab_str(at, "addressing_mode");
        if (am) snprintf(o->addr_mode, sizeof o->addr_mode, "%s", am);
    }
    NGTable *kids = tab_children(g);
    memset(o->targets, 0, sizeof o->targets);
    o->ntargets = 0;
    if (kids) {
        size_t nk = tab_len(kids);
        for (size_t i = 0; i < nk && o->ntargets < WC_MAX_TARGETS; i++) {
            NGTable *p = tab_child(kids, i);
            if (!p || !tab_str(p, "tag") || strcmp(tab_str(p, "tag"), "participant")) continue;
            const char *j = tab_str(tab_get(p, "attrs"), "jid");
            if (!j || !*j) continue;
            char bj[160]; base_jid(j, bj, sizeof bj);
            int dup = 0;
            for (int z = 0; z < o->ntargets; z++) if (!strcmp(o->targets[z], bj)) { dup = 1; break; }
            if (dup) continue;
            char mb[160], lb[160];
            mb[0] = lb[0] = 0;
            if (C.jid && *C.jid) base_jid(C.jid, mb, sizeof mb);
            if (C.lid && *C.lid) base_jid(C.lid, lb, sizeof lb);
            if ((mb[0] && !strcmp(bj, mb)) || (lb[0] && !strcmp(bj, lb))) continue;
            snprintf(o->targets[o->ntargets++], sizeof o->targets[0], "%s", bj);
        }
    }
    if (o->ntargets == 0) { err_set("grupo sem participantes"); o->used = 0; return; }

    /* Agora consulta os dispositivos de todos os participantes. */
    char qid[64]; make_query_id(qid, sizeof qid);
    NGTable *iq = node_new("iq");
    node_attr(iq, "id", qid);
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "get");
    node_attr(iq, "xmlns", "usync");
    NGTable *us = node_new("usync");
    node_attr(us, "sid", qid);
    node_attr(us, "mode", "query");
    node_attr(us, "last", "true");
    node_attr(us, "index", "0");
    node_attr(us, "context", "message");
    NGTable *uc = arr_new(2);
    NGTable *query = node_new("query");
    NGTable *devices = node_new("devices");
    node_attr(devices, "version", "2");
    NGTable *qa = arr_new(1); arr_set(qa, 0, devices);
    ng_table_set(query, "content", ng_val_table(qa));
    arr_set(uc, 0, query);
    NGTable *list = node_new("list");
    NGTable *la = arr_new((size_t)o->ntargets);
    for (int i = 0; i < o->ntargets; i++) {
        NGTable *u = node_new("user");
        node_attr(u, "jid", o->targets[i]);
        arr_set(la, (size_t)i, u);
    }
    ng_table_set(list, "content", ng_val_table(la));
    arr_set(uc, 1, list);
    ng_table_set(us, "content", ng_val_table(uc));
    NGTable *ica = arr_new(1); arr_set(ica, 0, us);
    ng_table_set(iq, "content", ng_val_table(ica));
    snprintf(W_QUERY, sizeof W_QUERY, "%s", qid);
    W_QUERY_KIND = 1;
    W_QUERY_OUT = (int)(o - W_OUT);
    send_node(iq);
    ng_table_release(iq);
}

static void on_usync_result(NGTable *n) {
    if (W_QUERY_OUT < 0 || W_QUERY_OUT >= WC_MAX_OUT) return;
    Out *o = &W_OUT[W_QUERY_OUT];
    if (!o->used) return;
    NGTable *us = tab_child_tag(n, "usync");
    NGTable *list = us ? tab_child_tag(us, "list") : NULL;
    NGTable *larr = tab_children(list);
    int nt = 0;
    if (larr) {
        size_t nl = tab_len(larr);
        for (size_t i = 0; i < nl; i++) {
            NGTable *u = tab_child(larr, i);
            const char *uj = tab_str(u, "jid");
            if (!uj) continue;
            char user[160], server[96]; uint32_t dev;
            split_jid(uj, user, sizeof user, server, sizeof server, &dev);
            NGTable *devs = tab_child_tag(u, "devices");
            NGTable *dl = devs ? tab_child_tag(devs, "device-list") : NULL;
            NGTable *darr = tab_children(dl);
            int found = 0;
            if (darr) {
                size_t nd = tab_len(darr);
                for (size_t j = 0; j < nd && nt < WC_MAX_TARGETS; j++) {
                    NGTable *d = tab_child(darr, j);
                    if (!d || strcmp(tab_str(d, "tag") ? tab_str(d, "tag") : "", "device")) continue;
                    NGTable *da = tab_get(d, "attrs");
                    const char *sid = tab_str(da, "id");
                    uint32_t id = sid ? (uint32_t)strtoul(sid, NULL, 10) : 0;
                    char devj[160];
                    if (id == 0) snprintf(devj, sizeof devj, "%s@%s", user, server);
                    else snprintf(devj, sizeof devj, "%s:%u@%s", user, id, server);
                    if (o->is_group) {
                        char db[160], mb[160], lb[160];
                        base_jid(devj, db, sizeof db);
                        mb[0] = lb[0] = 0;
                        if (C.jid && *C.jid) base_jid(C.jid, mb, sizeof mb);
                        if (C.lid && *C.lid) base_jid(C.lid, lb, sizeof lb);
                        if ((mb[0] && !strcmp(db, mb)) || (lb[0] && !strcmp(db, lb))) continue;
                    }
                    snprintf(o->targets[nt], sizeof o->targets[nt], "%s", devj);
                    nt++; found = 1;
                }
            }
            if (!found && nt < WC_MAX_TARGETS) {
                char bj[160]; base_jid(uj, bj, sizeof bj);
                snprintf(o->targets[nt++], sizeof o->targets[nt], "%s", bj);
            }
        }
    }
    if (nt == 0) snprintf(o->targets[nt++], sizeof o->targets[nt], "%s", o->to_base);
    o->ntargets = nt;

    /* Usuarios sem sessao precisam de bundle de prekeys. */
    int need = 0;
    for (int i = 0; i < nt; i++) {
        char a[160]; addr_jid(o->targets[i], a, sizeof a);
        if (!sig_find(a)) need++;
    }
    if (need == 0) { o->stage = 3; return; }

    NGTable *iq = node_new("iq");
    node_attr(iq, "type", "get");
    node_attr(iq, "xmlns", "encrypt");
    node_attr(iq, "to", "s.whatsapp.net");
    char qid[64]; make_query_id(qid, sizeof qid);
    node_attr(iq, "id", qid);
    snprintf(W_QUERY, sizeof W_QUERY, "%s", qid);
    W_QUERY_KIND = 2;
    NGTable *key = node_new("key");
    NGTable *kids = arr_new((size_t)nt);
    size_t kk = 0;
    for (int i = 0; i < nt; i++) {
        char a[160]; addr_jid(o->targets[i], a, sizeof a);
        if (sig_find(a)) continue;
        NGTable *u = node_new("user");
        node_attr(u, "jid", a);
        node_attr(u, "reason", "identity");
        arr_set(kids, kk++, u);
    }
    ng_table_set(kids, "length", ng_val_number((double)kk));
    ng_table_set(key, "content", ng_val_table(kids));
    NGTable *one = arr_new(1); arr_set(one, 0, key);
    ng_table_set(iq, "content", ng_val_table(one));
    send_node(iq);
    ng_table_release(iq);
    o->stage = 2;
}
static void on_keys_result(NGTable *n) {
    if (W_QUERY_OUT < 0 || W_QUERY_OUT >= WC_MAX_OUT) return;
    Out *o = &W_OUT[W_QUERY_OUT];
    if (!o->used) return;
    NGTable *list = tab_child_tag(n, "list");
    NGTable *larr = tab_children(list);
    if (larr) {
        size_t nl = tab_len(larr);
        for (size_t i = 0; i < nl; i++) {
            NGTable *u = tab_child(larr, i);
            const char *uj = tab_str(u, "jid");
            if (!uj) continue;
            Bundle b = parse_bundle(u);
            if (b.ok) sig_build_out(uj, &b);
        }
    }
    o->stage = 3;
}

static int wc_connect(void) {
    wc_close_conn();
    err_set("");
    st_set("conectando");
    WC_DBG("conectando websocket");
    W_WS = ng_ws_c_connect("wss://web.whatsapp.com/ws/chat", "https://web.whatsapp.com",
                           "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
                           12000);
    if (W_WS <= 0) {
        WC_DBG("websocket falhou: %s", ng_ws_c_error());
        err_set(ng_ws_c_error());
        st_set("desconectado");
        W_NEXT_CONNECT = wc_now_ms() + 3000;
        return -1;
    }
    /* A chave passada ao Noise e a EFEMERA da conexao (nova a cada
       handshake). A chave estatica persistida (C.noise_priv/pub) entra
       depois, em zap_handshake/ProcessarServidor. */
    W_ZAP = zap_start(NULL);
    if (W_ZAP <= 0) { err_set("nao foi possivel iniciar o Noise"); wc_close_conn(); st_set("desconectado"); W_NEXT_CONNECT = wc_now_ms() + 3000; return -1; }
    uint8_t *frame = NULL; size_t fn = 0;
    if (zap_frame_initial(W_ZAP, &frame, &fn) != 0) {
        err_set("falha ao montar ClientHello"); wc_close_conn(); st_set("desconectado"); W_NEXT_CONNECT = wc_now_ms() + 3000; return -1;
    }
    ws_send_raw(frame, fn);
    free(frame);
    W_HANDSHAKE = 1;
    st_set(C.paired ? "autenticando" : "pareando");
    if (C.paired) {
        uint8_t *p = NULL; size_t pn = 0;
        if (login_payload(&p, &pn) == 0) {
            if (pn <= sizeof W_PAYLOAD) { memcpy(W_PAYLOAD, p, pn); W_PAYLOAD_N = pn; }
            free(p);
        }
    } else {
        if (C.payload_n <= sizeof W_PAYLOAD) { memcpy(W_PAYLOAD, C.payload, C.payload_n); W_PAYLOAD_N = C.payload_n; }
    }
    WC_DBG("ClientHello enviado (payload %zu bytes, pareado=%d)", W_PAYLOAD_N, C.paired);
    return 0;
}

/* ------------------------------------------------------------------ QR */
static char W_REFS[16][128];
static int W_REFS_N = 0;
static uint64_t W_QR_AT = 0;

static void qr_make(int idx) {
    if (idx < 1 || idx > W_REFS_N) { W_QR_IDX = 0; W_QR[0] = 0; return; }
    /* qr_noise/qr_ident/qr_adv sao as chaves PUBLICAS e o segredo de pareamento */
    char *qn = b64(C.noise_pub, 32);
    char *qi = b64(C.ident_pub, 32);
    char *qa = b64(C.adv_secret, 32);
    snprintf(W_QR, sizeof W_QR, "%s,%s,%s,%s", W_REFS[idx - 1], qn ? qn : "", qi ? qi : "", qa ? qa : "");
    free(qn); free(qi); free(qa);
    W_QR_IDX = idx;
    W_QR_AT = wc_now_ms() + (idx == 1 ? 60000 : 20000);
    WC_DBG("QR %d/%d gerado (%zu bytes)", idx, W_REFS_N, strlen(W_QR));
}

/* ------------------------------------------------------------------ pareamento */
static void reply_result(NGTable *orig) {
    NGTable *at = tab_get(orig, "attrs");
    const char *id = tab_str(at, "id");
    if (!id) return;
    NGTable *iq = node_new("iq");
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "result");
    node_attr(iq, "id", id);
    send_node(iq);
    ng_table_release(iq);
}

static void handle_pair_device(NGTable *n) {
    NGTable *pd = tab_child_tag(n, "pair-device");
    if (!pd) return;
    reply_result(n);
    W_REFS_N = 0;
    NGTable *pdarr = tab_children(pd);
    size_t nk = tab_len(pdarr);
    for (size_t i = 0; i < nk && W_REFS_N < 16; i++) {
        NGTable *c = tab_child(pdarr, i);
        const char *tag = tab_str(c, "tag");
        const char *txt = tab_str(c, "content");
        if (tag && !strcmp(tag, "ref") && txt && *txt) {
            snprintf(W_REFS[W_REFS_N], sizeof W_REFS[0], "%s", txt);
            W_REFS_N++;
        }
    }
    WC_DBG("pair-device: %zu filhos, %d refs", nk, W_REFS_N);
    if (W_REFS_N == 0) { err_set("pair-device sem refs"); return; }
    st_set("pareando");
    qr_make(1);
}

static void hex_preview(const uint8_t *p, size_t n, char *out, size_t on) {
    size_t k = n < 32 ? n : 32;
    size_t i = 0;
    if (!on) return;
    for (; i < k && i + 2 < on; i++) sprintf(out + i * 2, "%02x", p[i]);
    out[i * 2] = 0;
    if (n > k && i + 2 < on) strcat(out, "..");
}

static int handle_pair_success(NGTable *n) {
    NGTable *ps = tab_child_tag(n, "pair-success");
    if (!ps) return -1;
    NGTable *dev_id = tab_child_tag(ps, "device-identity");
    NGTable *dev = tab_child_tag(ps, "device");
    if (!dev_id) { err_set("pair-success sem device-identity"); return -1; }
    const uint8_t *p = NULL; size_t pn = 0;
    int owned = node_binval(dev_id, &p, &pn);
    if (!p || pn == 0) { if (owned == 1) free((void *)p); err_set("device-identity vazio"); return -1; }
    char prev[80];
    hex_preview(p, pn, prev, sizeof prev);
    WC_DBG("device-identity: %zu bytes [%s]", pn, prev);

    /* O container e um ADVSignedDeviceIdentityHMAC. Alguns clientes entregam
       o conteudo como hex/texto ou com um prefixo de 1..4 bytes; tenta as
       representacoes conhecidas antes de desistir. */
    PB hm;
    const uint8_t *use = p; size_t usen = pn;
    uint8_t *decoded = NULL;
    int ok = pb_decode(use, usen, &hm);
    if (ok != 0) {
        size_t hn = 0;
        uint8_t *q = unhex((const char *)p, &hn);
        if (q) {
            if (hn && pb_decode(q, hn, &hm) == 0) {
                decoded = q; use = q; usen = hn; ok = 0;
            } else free(q);
        }
        for (int off = 1; ok != 0 && off <= 4 && (size_t)off < pn; off++) {
            if (pb_decode(p + off, pn - off, &hm) == 0) {
                use = p + off; usen = pn - off; ok = 0;
            }
        }
    }
#define WC_FREE_DEV() do { if (owned == 1) free((void *)p); if (decoded) free(decoded); } while (0)
    if (ok != 0) {
        WC_FREE_DEV();
        err_set("device-identity invalido"); return -1;
    }
    const PBEnt *details = pb_get(&hm, 1);
    const PBEnt *their = pb_get(&hm, 2);
    if (!details || !their || their->n != 32) {
        WC_FREE_DEV(); err_set("HMAC do device-identity ausente"); return -1;
    }
    /* ADVEncryptionType.HOSTED = 1: o prefixo 06 05 entra no HMAC. */
    B hmmsg = {0};
    if (pb_num(&hm, 3, 0) == 1) b_put(&hmmsg, (const uint8_t *)"\006\005", 2);
    b_put(&hmmsg, details->p, details->n);
    uint8_t mine[32];
    if (hmmsg.erro) {
        b_free(&hmmsg); WC_FREE_DEV(); err_set("HMAC do device-identity grande demais"); return -1;
    }
    hmac256(C.adv_secret, 32, hmmsg.p, hmmsg.n, mine);
    b_free(&hmmsg);
    if (memcmp(mine, their->p, 32) != 0) {
        WC_FREE_DEV(); err_set("HMAC do device-identity invalido"); return -1;
    }
    WC_DBG("device-identity HMAC ok");

    PB acc;
    if (pb_decode(details->p, details->n, &acc) != 0) {
        WC_FREE_DEV(); err_set("ADVSignedDeviceIdentity invalido"); return -1;
    }
    const PBEnt *devdet = pb_get(&acc, 1);
    const PBEnt *sigkey = pb_get(&acc, 2);
    const PBEnt *acctsig = pb_get(&acc, 3);
    if (!devdet || !sigkey || !acctsig || sigkey->n != 32 || acctsig->n != 64) {
        WC_FREE_DEV(); err_set("ADVSignedDeviceIdentity incompleto"); return -1;
    }
    PB dd;
    uint64_t key_index = pb_decode(devdet->p, devdet->n, &dd) == 0 ? pb_num(&dd, 3, 0) : 0;

    B dm = {0}, aenc = {0}, afull = {0};
    b_u8(&dm, 6); b_u8(&dm, 1);
    b_put(&dm, devdet->p, devdet->n);
    b_put(&dm, C.ident_pub, 32);
    b_put(&dm, sigkey->p, sigkey->n);
    uint8_t devsig[64];
    if (dm.erro || xeddsa_sign(C.ident_priv, dm.p, dm.n, devsig) != 0) {
        b_free(&dm); WC_FREE_DEV(); err_set("falha ao assinar o device-identity"); return -1;
    }
    /* Como no Baileys/whatsmeow, o device-identity devolvido nao inclui a
       chave de assinatura da conta (fields 1,3,4). */
    pb_bytes(&aenc, 1, devdet->p, devdet->n);
    pb_bytes(&aenc, 3, acctsig->p, acctsig->n);
    pb_bytes(&aenc, 4, devsig, 64);
    pb_bytes(&afull, 1, devdet->p, devdet->n);
    pb_bytes(&afull, 2, sigkey->p, sigkey->n);
    pb_bytes(&afull, 3, acctsig->p, acctsig->n);
    pb_bytes(&afull, 4, devsig, 64);
    b_free(&dm);
    if (aenc.erro || afull.erro || aenc.n > sizeof C.account || afull.n > sizeof C.account_full) {
        b_free(&aenc); b_free(&afull); WC_FREE_DEV();
        err_set("device-identity grande demais"); return -1;
    }
    memcpy(C.account, aenc.p, aenc.n); C.account_n = aenc.n;
    memcpy(C.account_full, afull.p, afull.n); C.account_full_n = afull.n;
    b_free(&aenc); b_free(&afull);

    const char *jid = dev ? tab_str(tab_get(dev, "attrs"), "jid") : NULL;
    const char *lid = dev ? tab_str(tab_get(dev, "attrs"), "lid") : NULL;
    if (jid) snprintf(C.jid, sizeof C.jid, "%s", jid);
    if (lid) snprintf(C.lid, sizeof C.lid, "%s", lid);
    C.key_index = (int)key_index;
    C.paired = 1;
    creds_save();
    W_QR_IDX = 0; W_REFS_N = 0; W_QR[0] = 0;

    NGTable *at = tab_get(n, "attrs");
    const char *id = tab_str(at, "id");
    NGTable *iq = node_new("iq");
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "result");
    node_attr(iq, "id", id ? id : "");
    NGTable *sign = node_new("pair-device-sign");
    NGTable *di = node_new("device-identity");
    node_attr_num(di, "key-index", (long long)key_index);
    node_bin(di, C.account, C.account_n);
    NGTable *a1 = arr_new(1); arr_set(a1, 0, di);
    ng_table_set(sign, "content", ng_val_table(a1));
    NGTable *a2 = arr_new(1); arr_set(a2, 0, sign);
    ng_table_set(iq, "content", ng_val_table(a2));
    send_node(iq);
    ng_table_release(iq);

    WC_FREE_DEV();
    st_set("pareando");
    err_set("");
#undef WC_FREE_DEV
    WC_DBG("pair-success processado: jid=%s key_index=%d", C.jid, C.key_index);
    return 0;
}

/* ------------------------------------------------------------------ prekeys */
static int pre_count(void) {
    int n = 0;
    for (int i = 0; i < WC_MAX_PREKEYS; i++) if (G_PRE[i].used) n++;
    return n;
}
static void ensure_prekeys(int want) {
    while (pre_count() < want) {
        uint8_t idb[3], priv[32];
        if (random_bytes(idb, 3) != 0 || random_bytes(priv, 32) != 0) return;
        uint32_t id = ((uint32_t)idb[0] << 16) | ((uint32_t)idb[1] << 8) | idb[2];
        if (id == 0) id = 1;
        if (pre_find(id)) continue;
        if (pre_add(id, priv) != 0) return;
    }
}
static void upload_prekeys(void) {
    ensure_prekeys(20);
    int n = pre_count();
    if (n == 0) return;
    char qid[64]; make_query_id(qid, sizeof qid);
    NGTable *iq = node_new("iq");
    node_attr(iq, "id", qid);
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "set");
    node_attr(iq, "xmlns", "encrypt");

    NGTable *kids = arr_new((size_t)n + 5);
    size_t k = 0;
    uint8_t rb[4] = {0, 0, (uint8_t)(C.reg_id >> 8), (uint8_t)C.reg_id};
    NGTable *reg = node_new("registration"); node_bin(reg, rb, 4); arr_set(kids, k++, reg);
    uint8_t kt = 5;
    NGTable *ty = node_new("type"); node_bin(ty, &kt, 1); arr_set(kids, k++, ty);
    NGTable *idn = node_new("identity"); node_bin(idn, C.ident_pub, 32); arr_set(kids, k++, idn);
    uint8_t sb[3] = {0, 0, (uint8_t)C.skey_id};
    NGTable *sk = node_new("skey");
    NGTable *skc = arr_new(3);
    NGTable *skid = node_new("id"); node_bin(skid, sb, 3); arr_set(skc, 0, skid);
    NGTable *skval = node_new("value"); node_bin(skval, C.skey_pub, 32); arr_set(skc, 1, skval);
    NGTable *sksig = node_new("signature"); node_bin(sksig, C.skey_sig, 64); arr_set(skc, 2, sksig);
    ng_table_set(sk, "content", ng_val_table(skc));
    arr_set(kids, k++, sk);
    NGTable *list = node_new("list");
    NGTable *lk = arr_new((size_t)n);
    size_t j = 0;
    for (int i = 0; i < WC_MAX_PREKEYS; i++) {
        if (!G_PRE[i].used) continue;
        uint8_t kb[3] = {0, 0, 0};
        kb[0] = (uint8_t)(G_PRE[i].id >> 16); kb[1] = (uint8_t)(G_PRE[i].id >> 8); kb[2] = (uint8_t)G_PRE[i].id;
        uint8_t pub[32], base[32] = {9};
        x25519(pub, G_PRE[i].priv, base);
        NGTable *key = node_new("key");
        NGTable *kc = arr_new(2);
        NGTable *ki = node_new("id"); node_bin(ki, kb, 3); arr_set(kc, 0, ki);
        NGTable *kv = node_new("value"); node_bin(kv, pub, 32); arr_set(kc, 1, kv);
        ng_table_set(key, "content", ng_val_table(kc));
        arr_set(lk, j++, key);
    }
    ng_table_set(list, "content", ng_val_table(lk));
    arr_set(kids, k++, list);
    ng_table_set(kids, "length", ng_val_number((double)k));
    ng_table_set(iq, "content", ng_val_table(kids));
    send_node(iq);
    ng_table_release(iq);
    sig_save();
}

/* ------------------------------------------------------------------ envio */
/* Escapa aspas e barras para inserir texto dentro de uma string JSON. */
static void json_esc(const char *in, char *out, size_t n) {
    size_t k = 0;
    for (size_t i = 0; in && in[i] && k + 2 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c < 0x20) { continue; }
        else out[k++] = (char)c;
    }
    out[k] = 0;
}

static uint8_t *build_plain(const Out *o, size_t *pn) {
    B msg = {0};
    if (o->kind == 0) {
        pb_str(&msg, 1, o->texto);
    } else {
        /* interactiveMessage (campo 45): body(2)=text(1), nativeFlowMessage(6) com
           botoes quick_reply (buttonParamsJson = {display_text, id}). Formato
           usado pelas bibliotecas que renderizam botoes em contas comuns. */
        B im = {0}, body = {0}, nfm = {0};
        pb_str(&body, 1, o->texto);
        pb_bytes(&im, 2, body.p, body.n);
        b_free(&body);
        for (int i = 0; i < o->nbtn; i++) {
            B btn = {0};
            char js[512], id_e[256], tit_e[256];
            json_esc(o->btn_id[i], id_e, sizeof id_e);
            json_esc(o->btn_titulo[i], tit_e, sizeof tit_e);
            snprintf(js, sizeof js, "{\"display_text\":\"%s\",\"id\":\"%s\"}", tit_e, id_e);
            pb_str(&btn, 1, "quick_reply");
            pb_str(&btn, 2, js);
            pb_bytes(&nfm, 1, btn.p, btn.n);
            b_free(&btn);
        }
        pb_bytes(&im, 6, nfm.p, nfm.n);
        b_free(&nfm);
        pb_bytes(&msg, 45, im.p, im.n);
        b_free(&im);
    }
    if (msg.erro) { b_free(&msg); return NULL; }
    *pn = msg.n;
    uint8_t *p = msg.p;
    msg.p = NULL;
    b_free(&msg);
    return p;
}

typedef struct { char jid[160], tipo[8]; uint8_t *data; size_t n; } EncEnt;

/* Envio para grupo: SKDM 1:1 para cada dispositivo + enc type=skmsg. */
static void out_final_group(Out *o) {
    size_t pn = 0;
    uint8_t *plain = build_plain(o, &pn);
    if (!plain) { err_set("falha ao montar a mensagem"); o->used = 0; return; }

    char own[160]; addr_jid(C.jid, own, sizeof own);
    uint8_t rb[3];
    if (random_bytes(rb, 3) != 0) { free(plain); o->used = 0; return; }
    uint32_t id = (((uint32_t)rb[0] << 16) | ((uint32_t)rb[1] << 8) | rb[2]) % 0xFFFFFF + 1;
    SenderKey *k = sk_alloc(o->to_base, own, id);
    if (!k) { free(plain); o->used = 0; return; }
    if (!k->chain[0] && !k->chain[31]) {
        if (random_bytes(k->chain, 32) != 0 || random_bytes(k->sign_priv, 32) != 0) {
            free(plain); o->used = 0; return;
        }
        uint8_t base9[32] = {9};
        x25519(k->sign_pub, k->sign_priv, base9);
        k->index = 0;
    }

    /* SenderKeyDistributionMessage: Message.field2 {groupId, SenderKeyMessage}. */
    B skdm = {0};
    b_u8(&skdm, SIG_VERSION);
    pb_int(&skdm, 1, k->id);
    pb_int(&skdm, 2, k->index);
    pb_bytes(&skdm, 3, k->chain, 32);
    uint8_t pub33[33]; pub33[0] = 5; memcpy(pub33 + 1, k->sign_pub, 32);
    pb_bytes(&skdm, 4, pub33, 33);
    B inner = {0};
    pb_bytes(&inner, 1, o->to_base, strlen(o->to_base));
    pb_bytes(&inner, 2, skdm.p, skdm.n);
    B pskdm = {0};
    pb_bytes(&pskdm, 2, inner.p, inner.n);
    b_free(&skdm); b_free(&inner);

    EncEnt ent[WC_MAX_TARGETS];
    int ne = 0, has_pk = 0;
    memset(ent, 0, sizeof ent);
    char mid[64]; new_msg_id(mid, sizeof mid);

    for (int i = 0; i < o->ntargets && ne < WC_MAX_TARGETS; i++) {
        char a[160]; addr_jid(o->targets[i], a, sizeof a);
        if (sk_have_dist(k, a)) continue;
        char tipo[8]; uint8_t *data = NULL; size_t dn = 0;
        int r = sig_encrypt(o->targets[i], pskdm.p, pskdm.n, tipo, sizeof tipo, &data, &dn);
        if (r != 0) { WC_DBG("[out] SKDM falhou para %s (r=%d)", a, r); continue; }
        snprintf(ent[ne].jid, sizeof ent[ne].jid, "%s", a);
        snprintf(ent[ne].tipo, sizeof ent[ne].tipo, "%s", tipo);
        ent[ne].data = data; ent[ne].n = dn;
        if (!strcmp(tipo, "pkmsg")) has_pk = 1;
        sk_add_dist(k, a);
        ne++;
    }
    b_free(&pskdm);

    /* skmsg = versao + id + iteracao + AES-CBC(Message) + assinatura XEdDSA. */
    uint8_t seed[32], keys[48];
    hmac256(k->chain, 32, (const uint8_t *)"\1", 1, seed);
    hkdf256(seed, 32, ZERO32, 32, (const uint8_t *)"WhisperGroup", 12, keys, 48);
    size_t ctn = 0;
    uint8_t *ct = NULL;
    int cr = aes_cbc(1, keys + 16, keys, plain, pn, &ct, &ctn);
    free(plain);
    if (cr != 0 || !ct) { err_set("falha ao cifrar mensagem do grupo"); o->used = 0; return; }

    B body = {0};
    b_u8(&body, SIG_VERSION);
    pb_int(&body, 1, k->id);
    pb_int(&body, 2, k->index);
    pb_bytes(&body, 3, ct, ctn);
    free(ct);
    uint8_t sig[64];
    if (xeddsa_sign(k->sign_priv, body.p, body.n, sig) != 0) {
        b_free(&body); err_set("falha ao assinar mensagem do grupo"); o->used = 0; return;
    }
    b_put(&body, sig, 64);
    hmac256(k->chain, 32, (const uint8_t *)"\2", 1, k->chain);
    k->index++;
    sk_save();

    NGTable *m = node_new("message");
    node_attr(m, "id", mid);
    node_attr(m, "to", o->to_base);
    node_attr(m, "type", "text");
    if (o->addr_mode[0]) node_attr(m, "addressing_mode", o->addr_mode);
    NGTable *mc = arr_new(2 + (has_pk ? 1 : 0));
    size_t mi = 0;

    if (ne > 0) {
        NGTable *parts = node_new("participants");
        NGTable *pa = arr_new((size_t)ne);
        for (int i = 0; i < ne; i++) {
            NGTable *to = node_new("to");
            node_attr(to, "jid", ent[i].jid);
            NGTable *e = node_new("enc");
            node_attr(e, "v", "2");
            node_attr(e, "type", ent[i].tipo);
            node_bin(e, ent[i].data, ent[i].n);
            NGTable *ea = arr_new(1); arr_set(ea, 0, e);
            ng_table_set(to, "content", ng_val_table(ea));
            arr_set(pa, (size_t)i, to);
            free(ent[i].data);
        }
        ng_table_set(parts, "content", ng_val_table(pa));
        arr_set(mc, mi++, parts);
    }
    if (has_pk) {
        NGTable *di = node_new("device-identity");
        node_bin(di, C.account, C.account_n);
        arr_set(mc, mi++, di);
    }
    NGTable *e = node_new("enc");
    node_attr(e, "v", "2");
    node_attr(e, "type", "skmsg");
    node_bin(e, body.p, body.n);
    arr_set(mc, mi++, e);
    size_t skmsg_n = body.n;
    b_free(&body);
    ng_table_set(m, "content", ng_val_table(mc));
    if (send_node(m) != 0) err_set("falha ao enviar a mensagem do grupo");
    ng_table_release(m);
    WC_DBG("[out] grupo %s: skmsg (%zu bytes) + %d SKDM(s)", o->to_base, skmsg_n, ne);
    o->used = 0;
}

static void out_final(Out *o) {
    if (o->is_group) { out_final_group(o); return; }
    size_t pn = 0;
    uint8_t *plain = build_plain(o, &pn);
    if (!plain) { err_set("falha ao montar a mensagem"); o->used = 0; return; }
    char mid[64]; new_msg_id(mid, sizeof mid);

    EncEnt ent[WC_MAX_TARGETS];
    int ne = 0, sem_sessao = 0, has_pk = 0;
    memset(ent, 0, sizeof ent);
    for (int i = 0; i < o->ntargets && ne < WC_MAX_TARGETS; i++) {
        char tipo[8]; uint8_t *data = NULL; size_t dn = 0;
        int r = sig_encrypt(o->targets[i], plain, pn, tipo, sizeof tipo, &data, &dn);
        if (r == -2) { sem_sessao++; continue; }
        if (r != 0) continue;
        snprintf(ent[ne].jid, sizeof ent[ne].jid, "%s", o->targets[i]);
        snprintf(ent[ne].tipo, sizeof ent[ne].tipo, "%s", tipo);
        ent[ne].data = data; ent[ne].n = dn;
        if (!strcmp(tipo, "pkmsg")) has_pk = 1;
        ne++;
    }
    free(plain);

    if (ne == 0) {
        err_set(sem_sessao ? "sem sessao Signal para o destinatario" : "falha ao cifrar a mensagem");
        o->used = 0;
        return;
    }

    /* Formato multi-dispositivo do WhatsApp: uma unica mensagem para o JID
       base, com um filho <to jid=...> para cada dispositivo. */
    NGTable *m = node_new("message");
    node_attr(m, "id", mid);
    node_attr(m, "to", o->to_base);
    node_attr(m, "type", "text");
    NGTable *parts = node_new("participants");
    NGTable *pa = arr_new((size_t)ne);
    for (int i = 0; i < ne; i++) {
        NGTable *to = node_new("to");
        node_attr(to, "jid", ent[i].jid);
        NGTable *e = node_new("enc");
        node_attr(e, "v", "2");
        node_attr(e, "type", ent[i].tipo);
        node_attr(e, "mediatype", "text");
        node_bin(e, ent[i].data, ent[i].n);
        NGTable *ea = arr_new(1); arr_set(ea, 0, e);
        ng_table_set(to, "content", ng_val_table(ea));
        arr_set(pa, (size_t)i, to);
        free(ent[i].data);
    }
    ng_table_set(parts, "content", ng_val_table(pa));
    int com_biz = (o->kind == 1);
    NGTable *mc = arr_new((has_pk ? 1 : 0) + 1 + (com_biz ? 1 : 0));
    size_t mi = 0;
    arr_set(mc, mi++, parts);
    if (has_pk) {
        NGTable *di = node_new("device-identity");
        node_bin(di, C.account, C.account_n);
        arr_set(mc, mi++, di);
    }
    if (com_biz) {
        /* <biz><interactive type="native_flow" v="1"><native_flow v="9" name="mixed"/></interactive></biz> */
        NGTable *nf = node_new("native_flow");
        node_attr(nf, "v", "9");
        node_attr(nf, "name", "mixed");
        NGTable *inter = node_new("interactive");
        node_attr(inter, "type", "native_flow");
        node_attr(inter, "v", "1");
        NGTable *ia = arr_new(1); arr_set(ia, 0, nf);
        ng_table_set(inter, "content", ng_val_table(ia));
        NGTable *biz = node_new("biz");
        NGTable *ba = arr_new(1); arr_set(ba, 0, inter);
        ng_table_set(biz, "content", ng_val_table(ba));
        arr_set(mc, mi++, biz);
    }
    ng_table_set(m, "content", ng_val_table(mc));
    if (send_node(m) != 0) err_set("falha ao enviar a mensagem");
    ng_table_release(m);
    o->used = 0;
}

static void out_begin(Out *o) {
    char qid[64]; make_query_id(qid, sizeof qid);
    if (o->is_group) {
        NGTable *gi = node_new("iq");
        node_attr(gi, "id", qid);
        node_attr(gi, "to", o->to_base);
        node_attr(gi, "type", "get");
        node_attr(gi, "xmlns", "w:g2");
        NGTable *query = node_new("query");
        node_attr(query, "request", "interactive");
        NGTable *qa = arr_new(1); arr_set(qa, 0, query);
        ng_table_set(gi, "content", ng_val_table(qa));
        snprintf(W_QUERY, sizeof W_QUERY, "%s", qid);
        W_QUERY_KIND = 3;
        W_QUERY_OUT = (int)(o - W_OUT);
        send_node(gi);
        ng_table_release(gi);
        o->stage = 1;
        return;
    }
    NGTable *iq = node_new("iq");
    node_attr(iq, "id", qid);
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "get");
    node_attr(iq, "xmlns", "usync");
    NGTable *us = node_new("usync");
    node_attr(us, "sid", qid);
    node_attr(us, "mode", "query");
    node_attr(us, "last", "true");
    node_attr(us, "index", "0");
    node_attr(us, "context", "message");
    NGTable *uc = arr_new(2);
    NGTable *query = node_new("query");
    NGTable *devices = node_new("devices");
    node_attr(devices, "version", "2");
    NGTable *qa = arr_new(1); arr_set(qa, 0, devices);
    ng_table_set(query, "content", ng_val_table(qa));
    arr_set(uc, 0, query);
    NGTable *list = node_new("list");
    NGTable *la = arr_new(1);
    NGTable *user = node_new("user");
    node_attr(user, "jid", o->to_base);
    arr_set(la, 0, user);
    ng_table_set(list, "content", ng_val_table(la));
    arr_set(uc, 1, list);
    ng_table_set(us, "content", ng_val_table(uc));
    NGTable *ica = arr_new(1); arr_set(ica, 0, us);
    ng_table_set(iq, "content", ng_val_table(ica));
    snprintf(W_QUERY, sizeof W_QUERY, "%s", qid);
    W_QUERY_KIND = 1;
    W_QUERY_OUT = (int)(o - W_OUT);
    send_node(iq);
    ng_table_release(iq);
    o->stage = 1;
}

static int out_inflight(void) {
    for (int i = 0; i < WC_MAX_OUT; i++)
        if (W_OUT[i].used && (W_OUT[i].stage == 1 || W_OUT[i].stage == 2)) return 1;
    return 0;
}

static void wc_process_out(void) {
    if (W_QUERY[0]) return;
    if (out_inflight()) return;
    for (int i = 0; i < WC_MAX_OUT; i++) {
        Out *o = &W_OUT[i];
        if (!o->used) continue;
        if (o->stage == 0) { out_begin(o); return; }
        if (o->stage == 3) { out_final(o); return; }
    }
}

/* ------------------------------------------------------------------ protocolo */
static void on_iq(NGTable *n) {
    NGTable *at = tab_get(n, "attrs");
    const char *id = tab_str(at, "id");
    const char *type = tab_str(at, "type");
    const char *xmlns = tab_str(at, "xmlns");

    WC_DBG("iq id=%s type=%s xmlns=%s", id ? id : "", type ? type : "", xmlns ? xmlns : "");
    if (wc_debug_on()) {
        NGTable *cc = tab_get(n, "content");
        size_t cn = tab_len(cc);
        for (size_t i = 0; i < cn; i++) {
            NGTable *ch = tab_child(cc, i);
            WC_DBG("  filho iq[%zu]=%s", i, ch && tab_str(ch, "tag") ? tab_str(ch, "tag") : "?");
        }
    }
    if (tab_child_tag(n, "pair-device")) { handle_pair_device(n); return; }
    if (tab_child_tag(n, "pair-success")) { handle_pair_success(n); return; }

    if (type && !strcmp(type, "get") && xmlns && !strcmp(xmlns, "urn:xmpp:ping")) {
        reply_result(n);
        return;
    }
    if (id && W_QUERY[0] && !strcmp(id, W_QUERY)) {
        int kind = W_QUERY_KIND;
        W_QUERY[0] = 0; W_QUERY_KIND = 0;
        if (kind == 1) on_usync_result(n);
        else if (kind == 2) on_keys_result(n);
        else if (kind == 3) on_group_result(n);
    }
}

/* Presenca "available" no formato do WhatsApp Web: sem atributo "to" e com
   "name". Renovada a cada 4 min enquanto online (ver wc_update). */
static void send_presence(void) {
    NGTable *p = node_new("presence");
    node_attr(p, "name", "AstraBot");
    node_attr(p, "type", "available");
    send_node(p);
    ng_table_release(p);
}

static void send_ping(void) {
    static int pn = 0;
    char id[32]; snprintf(id, sizeof id, "ka-%d", ++pn);
    NGTable *iq = node_new("iq");
    node_attr(iq, "id", id);
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "type", "get");
    node_attr(iq, "xmlns", "w:p");
    NGTable *ping = node_new("ping");
    NGTable *a = arr_new(1); arr_set(a, 0, ping);
    ng_table_set(iq, "content", ng_val_table(a));
    send_node(iq);
    ng_table_release(iq);
}

static void on_success(NGTable *n) {
    NGTable *at = tab_get(n, "attrs");
    const char *lid = tab_str(at, "lid");
    if (lid && *lid) { snprintf(C.lid, sizeof C.lid, "%s", lid); creds_save(); }
    C.paired = 1;
    W_BACKOFF_MS = 3000;
    st_set("online");
    err_set("");
    W_QR_IDX = 0; W_REFS_N = 0; W_QR[0] = 0;
    W_NEXT_PING = wc_now_ms() + 30000;

    NGTable *iq = node_new("iq");
    node_attr(iq, "id", "passive-1");
    node_attr(iq, "to", "s.whatsapp.net");
    node_attr(iq, "xmlns", "passive");
    node_attr(iq, "type", "set");
    NGTable *active = node_new("active");
    NGTable *a = arr_new(1); arr_set(a, 0, active);
    ng_table_set(iq, "content", ng_val_table(a));
    send_node(iq);
    ng_table_release(iq);

    send_presence();
    W_NEXT_PRESENCE = wc_now_ms() + 240000;
    upload_prekeys();
}

static void on_node(NGTable *n) {
    const char *tag = tab_str(n, "tag");
    NGTable *at = tab_get(n, "attrs");
    if (!tag) return;

    if (!strcmp(tag, "xmlstreamend")) {
        err_set("stream encerrada pelo servidor");
        wc_close_conn();
        st_set("desconectado");
        wc_reagendar();
        return;
    }
    if (!strcmp(tag, "stream:error")) {
        const char *code = tab_str(at, "code");
        WC_DBG("stream:error code=%s", code ? code : "?");
        if (code && !strcmp(code, "515")) {
            /* Reinicio normal logo apos o pareamento. */
            wc_close_conn();
            st_set("desconectado");
            W_NEXT_CONNECT = wc_now_ms() + 1000;
            return;
        }
        /* Detalhe do filho (ex.: <conflict type="replaced"/>) para o diagnostico. */
        {
            NGTable *ct = tab_get(n, "content");
            size_t nk = tab_len(ct);
            for (size_t i = 0; i < nk; i++) {
                NGTable *c = tab_child(ct, i);
                const char *ftag = c ? tab_str(c, "tag") : NULL;
                if (!ftag) continue;
                NGTable *ca = tab_get(c, "attrs");
                const char *ftype = tab_str(ca, "type");
                WC_DBG("stream:error filho=%s type=%s", ftag, ftype ? ftype : "-");
            }
        }
        if (code && !strcmp(code, "440")) {
            W_STOP = 1;
            err_set("CONFLITO (440): outra conexao esta usando esta mesma sessao. Feche o outro bot/aparelho que usa dados/whatsapp_sessao e rode de novo.");
        } else if (code && !strcmp(code, "401")) {
            W_STOP = 1;
            err_set("SESSAO REMOVIDA (401): o aparelho foi desvinculado no celular. Pareie de novo com ASTRA_QR_FORCAR=1.");
        } else if (code && !strcmp(code, "411")) {
            W_STOP = 1;
            err_set("MULTIDEVICE (411): a sessao nao e compativel com o multi-dispositivo. Pareie de novo com ASTRA_QR_FORCAR=1.");
        } else if (code && !strcmp(code, "500")) {
            W_STOP = 1;
            err_set("SESSAO INVALIDA (500): credenciais corrompidas. Pareie de novo com ASTRA_QR_FORCAR=1.");
        } else if (code && !strcmp(code, "403")) {
            W_STOP = 1;
            err_set("RECUSADA (403): o WhatsApp recusou a conta ou o cliente. Veja NG_WACORE_DEBUG=1 e o LEIA-ME.");
        } else {
            err_set(code ? code : "stream:error");
        }
        wc_close_conn();
        st_set("desconectado");
        return;
    }
    if (!strcmp(tag, "failure")) {
        const char *reason = tab_str(at, "reason");
        WC_DBG("failure reason=%s", reason ? reason : "?");
        err_set(reason ? reason : "login recusado");
        wc_close_conn();
        st_set("desconectado");
        /* 503/504 sao transitorios: nao matam o bot, tentam de novo com backoff. */
        if (reason && (!strcmp(reason, "503") || !strcmp(reason, "504"))) {
            wc_reagendar();
        } else {
            W_STOP = 1;
        }
        return;
    }
    if (!strcmp(tag, "success")) { WC_DBG("success"); on_success(n); return; }
    if (!strcmp(tag, "ack")) {
        const char *cls = tab_str(at, "class");
        const char *err = tab_str(at, "error");
        WC_DBG("[ack] class=%s id=%s erro=%s", cls ? cls : "-",
               tab_str(at, "id") ? tab_str(at, "id") : "-", err ? err : "-");
        return;
    }
    if (!strcmp(tag, "iq")) { on_iq(n); return; }
    if (!strcmp(tag, "message")) { on_message(n); return; }
    if (!strcmp(tag, "notification") || !strcmp(tag, "receipt")) {
        const char *id = tab_str(at, "id");
        const char *from = tab_str(at, "from");
        if (id && from) send_ack(n, id, from);
        return;
    }
}

static void wc_handle_frame(const uint8_t *d, size_t dn) {
    if (W_HANDSHAKE) {
        if (dn < 3) return;
        size_t hl = ((size_t)d[0] << 16) | ((size_t)d[1] << 8) | d[2];
        if (dn < 3 + hl) return;
        uint8_t *fin = NULL; size_t fn = 0;
        if (zap_handshake(W_ZAP, d + 3, hl, C.noise_priv, C.noise_pub, W_PAYLOAD, W_PAYLOAD_N, &fin, &fn) == 0) {
            ws_send_raw(fin, fn);
            free(fin);
            W_HANDSHAKE = 0;
            WC_DBG("ClientFinish enviado");
            st_set(C.paired ? "autenticando" : "pareando");
        } else {
            WC_DBG("handshake falhou");
            err_set("falha no handshake Noise");
            wc_close_conn();
            st_set("desconectado");
            wc_reagendar();
        }
        return;
    }
    size_t pos = 0;
    while (dn - pos >= 3) {
        size_t len = ((size_t)d[pos] << 16) | ((size_t)d[pos + 1] << 8) | d[pos + 2];
        if (dn - pos - 3 < len) break;
        NGTable *node = zap_decrypt_node(W_ZAP, d + pos + 3, len);
        pos += 3 + len;
        if (node) {
            WC_DBG("no recebido: %s", tab_str(node, "tag") ? tab_str(node, "tag") : "?");
            on_node(node);
            ng_table_release(node);
        } else {
            WC_DBG("falha ao decifrar no de transporte (%zu bytes)", len);
        }
    }
}

/* ------------------------------------------------------------------ laco */
static void wc_update(int ms) {
    if (!W_ATIVO || W_STOP) return;
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;
    uint64_t end = wc_now_ms() + (uint64_t)ms;
    do {
        uint64_t now = wc_now_ms();
        if (W_WS <= 0) {
            if (now >= W_NEXT_CONNECT) wc_connect();
            if (W_WS <= 0) {
                uint64_t w = end > now ? end - now : 0;
                if (W_NEXT_CONNECT > now && W_NEXT_CONNECT - now < w) w = W_NEXT_CONNECT - now;
                if (w > 200) w = 200;
                if (w) wc_sleep_ms((int)w);
                continue;
            }
        }
        wc_process_out();
        now = wc_now_ms();
        if (W_QR_IDX > 0 && now >= W_QR_AT && !strcmp(W_ESTADO, "pareando")) {
            if (W_QR_IDX < W_REFS_N) qr_make(W_QR_IDX + 1);
            else { W_QR_IDX = 0; W_QR[0] = 0; }
        }
        if (now >= W_NEXT_PING) { send_ping(); W_NEXT_PING = now + 30000; }
        if (W_NEXT_PRESENCE && now >= W_NEXT_PRESENCE && !strcmp(W_ESTADO, "online")) {
            send_presence();
            W_NEXT_PRESENCE = now + 240000;
        }
        now = wc_now_ms();
        int wait = 100;
        if (end > now && end - now < 100) wait = (int)(end - now);
        uint8_t *data = NULL; size_t dn = 0; int op = 0;
        int r = ng_ws_c_recv(W_WS, wait, &data, &dn, &op);
        if (r == 1) { wc_handle_frame(data, dn); free(data); }
        else if (r < 0 || op == 8) {
            WC_DBG("websocket encerrado: r=%d op=%d", r, op);
            free(data);
            wc_close_conn();
            st_set("desconectado");
            if (!W_STOP) wc_reagendar();
        }
    } while (W_ATIVO && !W_STOP && wc_now_ms() < end);
}

/* ------------------------------------------------------------------ API NG */
static const char *fstr(NGValue *a, size_t n, size_t i) {
    return (i < n && a[i].type == NG_VAL_STRING) ? a[i].as.string : NULL;
}
static double fnum(NGValue *a, size_t n, size_t i, double def) {
    return (i < n && a[i].type == NG_VAL_NUMBER) ? a[i].as.number : def;
}
static int fbool(NGValue *a, size_t n, size_t i) {
    return i < n && a[i].type == NG_VAL_BOOL && a[i].as.boolean;
}

static NGValue f_iniciar(NGInterpreter *interp, NGValue *a, size_t n) {
    const char *dir = fstr(a, n, 0);
    if (!dir || !*dir) return ng_val_bool(false);
    G_INTERP = interp;
    int force = fbool(a, n, 1);
    wc_close_conn();
    W_ATIVO = 1; W_STOP = 0;
    memset(W_MSGS, 0, sizeof W_MSGS); W_MSG_N = 0;
    memset(W_OUT, 0, sizeof W_OUT);
    W_QUERY[0] = 0; W_QUERY_KIND = 0; W_QUERY_OUT = -1;
    W_QR_IDX = 0; W_REFS_N = 0; W_QR[0] = 0;
    err_set(""); st_set("desconectado");
    memset(&C, 0, sizeof C);
    snprintf(C.dir, sizeof C.dir, "%s", dir);
    if (mkdir_p(C.dir) != 0) return ng_val_bool(false);
    int loaded = creds_load();
    if (force || !loaded) {
        if (make_creds() != 0) return ng_val_bool(false);
        snprintf(C.dir, sizeof C.dir, "%s", dir);
        if (force) C.paired = 0;
        creds_save();
    }
    snprintf(C.dir, sizeof C.dir, "%s", dir);
    sig_load();
    sk_load();
    WC_DBG("iniciar: payload=%zu ua=%zu paired=%d jid=%s", C.payload_n, C.user_agent_n, C.paired, C.jid);
    W_NEXT_CONNECT = 0;
    W_NEXT_PING = wc_now_ms() + 30000;
    st_set(C.paired ? "conectando" : "pareando");
    return ng_val_bool(true);
}

static NGValue f_atualizar(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp;
    int ms = (int)fnum(a, n, 0, 1000);
    wc_update(ms);
    return ng_val_bool(true);
}

static NGValue f_mensagens(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    size_t cnt = 0;
    for (int i = 0; i < WC_MAX_MSGS; i++) if (W_MSGS[i].used) cnt++;
    if (cnt) WC_DBG("[msg] entregando %zu mensagem(ns) ao bot", cnt);
    NGTable *arr = arr_new(cnt);
    size_t k = 0;
    for (int i = 0; i < WC_MAX_MSGS; i++) {
        Msg *m = &W_MSGS[i];
        if (!m->used) continue;
        NGTable *t = ng_table_new(16);
        ng_table_set(t, "chat", ng_val_string(m->chat));
        ng_table_set(t, "id", ng_val_string(m->id));
        ng_table_set(t, "de", ng_val_string(m->sender));
        ng_table_set(t, "autor", ng_val_string(m->autor));
        ng_table_set(t, "nome", ng_val_string(m->nome));
        ng_table_set(t, "texto", ng_val_string(m->texto));
        ng_table_set(t, "botao_id", ng_val_string(m->botao_id));
        ng_table_set(t, "grupo", ng_val_bool(m->grupo != 0));
        arr_set(arr, k++, t);
    }
    ng_table_set(arr, "length", ng_val_number((double)k));
    memset(W_MSGS, 0, sizeof W_MSGS); W_MSG_N = 0;
    return ng_val_table(arr);
}

static NGValue f_qr(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    NGTable *t = ng_table_new(8);
    int avail = W_QR_IDX > 0 && W_QR[0] != 0;
    ng_table_set(t, "disponivel", ng_val_bool(avail));
    ng_table_set(t, "texto", ng_val_string(avail ? W_QR : ""));
    ng_table_set(t, "indice", ng_val_number((double)W_QR_IDX));
    ng_table_set(t, "total", ng_val_number((double)W_REFS_N));
    ng_table_set(t, "estado", ng_val_string(W_ESTADO));
    return ng_val_table(t);
}

static NGValue f_estado(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    return ng_val_string(W_ESTADO);
}
static NGValue f_pronto(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    return ng_val_bool(!strcmp(W_ESTADO, "online"));
}
static NGValue f_erro(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    return ng_val_string(W_ERRO);
}
static NGValue f_me(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    NGTable *t = ng_table_new(8);
    ng_table_set(t, "jid", ng_val_string(C.jid));
    ng_table_set(t, "lid", ng_val_string(C.lid));
    ng_table_set(t, "pareado", ng_val_bool(C.paired != 0));
    return ng_val_table(t);
}

static int out_enqueue(const char *chat, const char *texto, int kind,
                       NGTable *botoes, size_t nbotoes) {
    if (!chat || !*chat || !texto) return 0;
    char base[160];
    base_jid(chat, base, sizeof base);
    if (strlen(texto) >= sizeof W_OUT[0].texto) { err_set("texto grande demais"); return 0; }
    Out *o = NULL;
    for (int i = 0; i < WC_MAX_OUT; i++) if (!W_OUT[i].used) { o = &W_OUT[i]; break; }
    if (!o) { err_set("fila de envio cheia"); return 0; }
    memset(o, 0, sizeof *o);
    o->used = 1;
    snprintf(o->to, sizeof o->to, "%s", chat);
    snprintf(o->to_base, sizeof o->to_base, "%s", base);
    o->is_group = is_group_jid(base);
    snprintf(o->texto, sizeof o->texto, "%s", texto);
    o->kind = kind;
    if (kind == 1) {
        if (nbotoes > 3) { o->used = 0; err_set("no maximo 3 botoes"); return 0; }
        int n = (int)nbotoes;
        for (int i = 0; i < n; i++) {
            NGTable *b = tab_child(botoes, (size_t)i);
            const char *id = tab_str(b, "id");
            const char *titulo = tab_str(b, "titulo");
            if (!titulo) titulo = tab_str(b, "texto");
            if (!id || !titulo) continue;
            snprintf(o->btn_id[o->nbtn], sizeof o->btn_id[0], "%s", id);
            snprintf(o->btn_titulo[o->nbtn], sizeof o->btn_titulo[0], "%s", titulo);
            o->nbtn++;
        }
        if (o->nbtn == 0) { o->used = 0; err_set("nenhum botao valido"); return 0; }
    }
    return 1;
}

static NGValue f_enviar(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp;
    const char *chat = fstr(a, n, 0), *txt = fstr(a, n, 1);
    if (!chat || !txt) { err_set("uso: Wacore.Enviar(chat, texto)"); return ng_val_bool(false); }
    return ng_val_bool(out_enqueue(chat, txt, 0, NULL, 0) != 0);
}

static NGValue f_enviar_botoes(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp;
    const char *chat = fstr(a, n, 0), *txt = fstr(a, n, 1);
    NGTable *b = (n >= 3 && a[2].type == NG_VAL_TABLE) ? a[2].as.table : NULL;
    if (!chat || !txt || !b) { err_set("uso: Wacore.EnviarBotoes(chat, texto, botoes)"); return ng_val_bool(false); }
    return ng_val_bool(out_enqueue(chat, txt, 1, b, tab_len(b)) != 0);
}

static NGValue f_fechar(NGInterpreter *interp, NGValue *a, size_t n) {
    (void)interp; (void)a; (void)n;
    W_ATIVO = 0; W_STOP = 1;
    wc_close_conn();
    st_set("desconectado");
    return ng_val_bool(true);
}

static void load_wacore(NGInterpreter *interp) {
    G_INTERP = interp;
    /* O handshake/transporte usa a API NG do @zap; garante que ele esteja
       carregado mesmo quando o script importa apenas @wacore. */
    ng_native_module_load("zap", interp);
    NGTable *t = ng_table_new(20);
    ng_table_set(t, "Iniciar", ng_val_native_func("Iniciar", f_iniciar));
    ng_table_set(t, "Atualizar", ng_val_native_func("Atualizar", f_atualizar));
    ng_table_set(t, "Mensagens", ng_val_native_func("Mensagens", f_mensagens));
    ng_table_set(t, "QR", ng_val_native_func("QR", f_qr));
    ng_table_set(t, "Estado", ng_val_native_func("Estado", f_estado));
    ng_table_set(t, "Pronto", ng_val_native_func("Pronto", f_pronto));
    ng_table_set(t, "Erro", ng_val_native_func("Erro", f_erro));
    ng_table_set(t, "Me", ng_val_native_func("Me", f_me));
    ng_table_set(t, "Enviar", ng_val_native_func("Enviar", f_enviar));
    ng_table_set(t, "EnviarBotoes", ng_val_native_func("EnviarBotoes", f_enviar_botoes));
    ng_table_set(t, "Fechar", ng_val_native_func("Fechar", f_fechar));
    ng_env_set(interp->globals, "Wacore", ng_val_table(t));
}

void ng_wacore_module_register(void) {
    ng_native_module_register("wacore", load_wacore);
}



