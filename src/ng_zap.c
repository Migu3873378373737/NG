/* ng_zap.c - @zap: protocolo do WhatsApp Web (multi-dispositivo) em C nativo.
 *
 * Port direto de NG-modulos/whatsapp/deps:
 *   wabinary.lua  -> codec binario de nos (tokens, JIDs, nibble/hex, binarios)
 *   proto.lua     -> protobuf minimo (so o necessario para o HandshakeMessage)
 *   noise.lua     -> Noise_XX_25519_AESGCM_SHA256 (estado, HKDF, GCM com contador)
 *   pair.lua      -> sequencia do handshake (ClientHello, ServerHello, ClientFinish)
 *
 * NAO faz (fica no conector Lua por enquanto): Signal (E2E), pareamento por
 * device-identity/ADV e montagem do ClientPayload. A cripto (SHA, HMAC, HKDF,
 * AES-GCM, X25519) vem do ng_cripto.c, sem OpenSSL.
 *
 * Dados binarios trafegam como HEX na NG. Mapa da API (objeto Zap):
 *   h  = Zap.Iniciar()                              -> sessao (numero) ou null
 *   q  = Zap.QuadroInicial(h)                       -> hex: WA header + ClientHello
 *   c  = Zap.Receber(h, hexMensagemWS)              -> lista de corpos hex (quadros completos)
 *   f  = Zap.ProcessarServidor(h, hexCorpo, nPriv, nPub, hexPayload) -> hex do ClientFinish
 *   f  = Zap.Cifrar(h, no)                          -> hex do quadro cifrado (3 bytes + GCM)
 *   no = Zap.Decifrar(h, hexCorpo)                  -> no (tabela) ou null
 *   hx = Zap.Codificar(no)                          -> hex do quadro claro (sem cifra)
 *   no = Zap.Decodificar(hexClaro)                  -> no (tabela) ou null
 *   Zap.Fechar(h)   Zap.Erro()
 *
 * No (tabela NG): { tag="iq", attrs={k=v}, content="texto" | content=[nos...] }
 *   content com "binario"=true => o texto e hex e vira bytes brutos.
 */
#include "ng_zap.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"
#include "ng_cripto.h"
#include "ng_zap_tokens.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NG_NO_ZLIB
#include <zlib.h>
#endif

/* ---------------------------------------------------------------- constantes */
#define ZAP_SESSOES 8
#define ZAP_MAX_QUADRO (16u * 1024u * 1024u)
#define ZAP_MAX_PROF 48

#define T_LIST_EMPTY 0
#define T_STREAM_END 2
#define T_DICT0 236
#define T_INTEROP 245
#define T_FB 246
#define T_AD 247
#define T_LIST8 248
#define T_LIST16 249
#define T_JID_PAIR 250
#define T_HEX8 251
#define T_BIN8 252
#define T_BIN20 253
#define T_BIN32 254
#define T_NIB8 255
#define T_PACKED_MAX 127

/* "Noise_XX_25519_AESGCM_SHA256" + 4 bytes zero (32 bytes), como noise.lua */
static const uint8_t NOISE_MODE[32] = {
    'N','o','i','s','e','_','X','X','_','2','5','5','1','9','_','A','E','S','G','C','M','_',
    'S','H','A','2','5','6', 0, 0, 0, 0 };
static const uint8_t WA_HEADER[4] = {'W', 'A', 6, 3};
static const uint8_t VAZIO[1] = {0};
static const uint8_t BASE_X25519[32] = {9};

static const char *const SERVIDORES[4] = {"s.whatsapp.net", "lid", "", ""};

/* ------------------------------------------------------------ buffers e arena */
typedef struct { uint8_t *p; size_t n, cap; int erro; } Buf;

static void bput(Buf *b, const void *s, size_t n) {
    if (b->erro || n == 0) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n) c *= 2;
        uint8_t *q = (uint8_t *)realloc(b->p, c);
        if (!q) { b->erro = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void bputc(Buf *b, unsigned c) { uint8_t x = (uint8_t)c; bput(b, &x, 1); }
static void bputint(Buf *b, uint64_t v, int k) {
    for (int i = k - 1; i >= 0; i--) bputc(b, (unsigned)((v >> (8 * i)) & 0xff));
}
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; b->erro = 0; }

/* Memoria de um codec: tudo que o decoder/encoder aloca some junto no fim. */
typedef struct { void **v; size_t n, cap; } Arena;
static void *arena_get(Arena *a, size_t sz) {
    void *p = calloc(1, sz ? sz : 1);
    if (!p) return NULL;
    if (a->n == a->cap) {
        size_t c = a->cap ? a->cap * 2 : 64;
        void **q = (void **)realloc(a->v, c * sizeof *q);
        if (!q) { free(p); return NULL; }
        a->v = q; a->cap = c;
    }
    a->v[a->n++] = p;
    return p;
}
static char *arena_str(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_get(a, n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static void arena_free(Arena *a) {
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL; a->n = a->cap = 0;
}

/* ------------------------------------------------------------ hex */
static int hexdig(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static uint8_t *unhex(const char *s, size_t *on) {
    size_t n = strlen(s);
    if (n % 2) return NULL;
    uint8_t *b = (uint8_t *)malloc(n / 2 + 1);
    if (!b) return NULL;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexdig(s[2 * i]), c = hexdig(s[2 * i + 1]);
        if (a < 0 || c < 0) { free(b); return NULL; }
        b[i] = (uint8_t)(a * 16 + c);
    }
    *on = n / 2;
    return b;
}
static char *tohex(const uint8_t *b, size_t n) {
    static const char D[] = "0123456789abcdef";
    char *s = (char *)malloc(n * 2 + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) { s[2 * i] = D[b[i] >> 4]; s[2 * i + 1] = D[b[i] & 15]; }
    s[2 * n] = 0;
    return s;
}

/* ------------------------------------------------------------ tokens (hash) */
typedef struct { const char *s; uint16_t len; int16_t dict; uint8_t idx; } TokEnt;
#define TOKMAP_N 2048
static TokEnt tokmap[TOKMAP_N];
static int tokmap_pronto = 0;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}
static void tok_put(const char *s, int dict, int idx) {
    size_t n = strlen(s);
    if (!n) return;
    uint32_t i = fnv(s, n) & (TOKMAP_N - 1);
    while (tokmap[i].s) {
        if (tokmap[i].len == n && !memcmp(tokmap[i].s, s, n)) return;   /* primeiro vence, como o Lua */
        i = (i + 1) & (TOKMAP_N - 1);
    }
    tokmap[i].s = s; tokmap[i].len = (uint16_t)n; tokmap[i].dict = (int16_t)dict; tokmap[i].idx = (uint8_t)idx;
}
static const TokEnt *tok_find(const char *s, size_t n) {
    uint32_t i = fnv(s, n) & (TOKMAP_N - 1);
    while (tokmap[i].s) {
        if (tokmap[i].len == n && !memcmp(tokmap[i].s, s, n)) return &tokmap[i];
        i = (i + 1) & (TOKMAP_N - 1);
    }
    return NULL;
}
static const char *const *const DICTS[4] = {ZAP_DICT0, ZAP_DICT1, ZAP_DICT2, ZAP_DICT3};
static const int DICT_N[4] = {ZAP_DICT0_N, ZAP_DICT1_N, ZAP_DICT2_N, ZAP_DICT3_N};
static void tok_init(void) {
    if (tokmap_pronto) return;
    for (int i = 1; i < ZAP_SINGLE_N; i++) tok_put(ZAP_SINGLE[i], -1, i);
    for (int d = 0; d < 4; d++)
        for (int j = 0; j < DICT_N[d]; j++) tok_put(DICTS[d][j], d, j);
    tokmap_pronto = 1;
}

/* ------------------------------------------------------------ no em C */
enum { ZK_NADA = 0, ZK_BYTES = 1, ZK_LISTA = 2 };
typedef struct ZNode {
    char *tag;
    int na;
    char **ak, **av;          /* atributos (chave, valor) */
    int kind;
    int binario;              /* ZK_BYTES: 1 = bytes brutos (is_binary) */
    uint8_t *dados;
    size_t dn;
    struct ZNode **filhos;
    int nf;
} ZNode;

/* ------------------------------------------------------------ decoder */
typedef struct { char *s; size_t n; } Str;
typedef struct {
    const uint8_t *b;
    size_t n, p;
    int prof;
    Arena *ar;
    const char *erro;
    jmp_buf jb;
} Dec;

#define DFAIL(d, msg) do { (d)->erro = (msg); longjmp((d)->jb, 1); } while (0)

static uint8_t dbyte(Dec *d) {
    if (d->p >= d->n) DFAIL(d, "wabinary: fim inesperado");
    return d->b[d->p++];
}
static const uint8_t *dbytes(Dec *d, size_t k) {
    if (k > d->n - d->p) DFAIL(d, "wabinary: fim inesperado");
    const uint8_t *q = d->b + d->p;
    d->p += k;
    return q;
}
static uint64_t dint(Dec *d, int k) {
    uint64_t v = 0;
    for (int i = 0; i < k; i++) v = (v << 8) | dbyte(d);
    return v;
}
static uint64_t dint20(Dec *d) {
    uint64_t a = dbyte(d), b = dbyte(d), c = dbyte(d);
    return (a % 16) * 65536 + b * 256 + c;
}
static Str mkstr(Dec *d, const char *s, size_t n) {
    Str r;
    r.s = arena_str(d->ar, s, n);
    if (!r.s) DFAIL(d, "sem memoria");
    r.n = n;
    return r;
}
/* concatena n partes (copia para a arena) */
static Str concat(Dec *d, const Str *partes, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += partes[i].n;
    char *p = (char *)arena_get(d->ar, total + 1);
    if (!p) DFAIL(d, "sem memoria");
    size_t k = 0;
    for (size_t i = 0; i < n; i++) { memcpy(p + k, partes[i].s, partes[i].n); k += partes[i].n; }
    p[k] = 0;
    Str r = {p, total};
    return r;
}
static Str lit(Dec *d, const char *s) { return mkstr(d, s, strlen(s)); }

static Str dpacked(Dec *d, int tag) {
    int start = dbyte(d);
    size_t pares = (size_t)(start % 128);
    char *tmp = (char *)arena_get(d->ar, pares * 2 + 1);
    if (!tmp) DFAIL(d, "sem memoria");
    size_t k = 0;
    for (size_t i = 0; i < pares; i++) {
        int cur = dbyte(d);
        for (int half = 0; half < 2; half++) {
            int v = half == 0 ? cur / 16 : cur % 16;
            char c;
            if (tag == T_NIB8) {
                if (v <= 11) c = "0123456789-."[v];
                else if (v == 15) c = '\0';
                else DFAIL(d, "wabinary: nibble invalido");
            } else {
                c = "0123456789ABCDEF"[v];
            }
            tmp[k++] = c;
        }
    }
    if (start >= 128 && k > 0) k--;          /* v:sub(1,-2) do Lua: descarta o padding */
    tmp[k] = 0;
    Str r = {tmp, k};
    return r;
}

static Str dstr(Dec *d, int tag);

static Str dstr(Dec *d, int tag) {
    if (++d->prof > ZAP_MAX_PROF) DFAIL(d, "wabinary: aninhamento excessivo");
    Str r;
    if (tag >= 1 && tag < ZAP_SINGLE_N) {
        const char *s = ZAP_SINGLE[tag];
        r = mkstr(d, s, strlen(s));
    } else if (tag >= T_DICT0 && tag <= T_DICT0 + 3) {
        int di = tag - T_DICT0, ix = dbyte(d);
        if (ix >= DICT_N[di]) DFAIL(d, "wabinary: token duplo invalido");
        const char *s = DICTS[di][ix];
        r = mkstr(d, s, strlen(s));
    } else if (tag == T_LIST_EMPTY) {
        r = mkstr(d, "", 0);
    } else if (tag == T_BIN8) {
        size_t k = dbyte(d);
        const uint8_t *q = dbytes(d, k);
        r = mkstr(d, (const char *)q, k);
    } else if (tag == T_BIN20) {
        size_t k = (size_t)dint20(d);
        const uint8_t *q = dbytes(d, k);
        r = mkstr(d, (const char *)q, k);
    } else if (tag == T_BIN32) {
        size_t k = (size_t)dint(d, 4);
        const uint8_t *q = dbytes(d, k);
        r = mkstr(d, (const char *)q, k);
    } else if (tag == T_JID_PAIR) {
        Str i = dstr(d, dbyte(d));
        Str j = dstr(d, dbyte(d));
        if (j.n == 0) DFAIL(d, "wabinary: jid invalido");
        if (i.n == 0) r = j;
        else {
            Str partes[3] = {i, lit(d, "@"), j};
            r = concat(d, partes, 3);
        }
    } else if (tag == T_FB || tag == T_INTEROP) {
        /* FB: user, device(2), server. INTEROP: user, device(2), integrator(2), server. */
        Str user = dstr(d, dbyte(d));
        unsigned dev = (unsigned)dint(d, 2);
        unsigned integ = 0;
        if (tag == T_INTEROP) integ = (unsigned)dint(d, 2);
        Str server = dstr(d, dbyte(d));
        char dv[24] = "";
        if (dev) snprintf(dv, sizeof dv, ":%u", dev);
        char ig[24] = "";
        if (tag == T_INTEROP) snprintf(ig, sizeof ig, "%u-", integ);
        Str partes[5] = {mkstr(d, ig, strlen(ig)), user, mkstr(d, dv, strlen(dv)), lit(d, "@"), server};
        r = concat(d, partes, 5);
    } else if (tag == T_AD) {
        int agente = dbyte(d), dev = dbyte(d);
        Str user = dstr(d, dbyte(d));
        const char *srv = (agente == 0) ? SERVIDORES[0] : (agente == 1) ? SERVIDORES[1] : "s.whatsapp.net";
        char dv[16] = "";
        if (dev) snprintf(dv, sizeof dv, ":%d", dev);
        Str partes[4] = {user, mkstr(d, dv, strlen(dv)), lit(d, "@"), mkstr(d, srv, strlen(srv))};
        r = concat(d, partes, 4);
    } else if (tag == T_HEX8 || tag == T_NIB8) {
        r = dpacked(d, tag);
    } else {
        DFAIL(d, "wabinary: tag invalida");
    }
    d->prof--;
    return r;
}

static size_t dlist_size(Dec *d, int tag) {
    if (tag == T_LIST_EMPTY) return 0;
    if (tag == T_LIST8) return dbyte(d);
    if (tag == T_LIST16) return (size_t)dint(d, 2);
    DFAIL(d, "wabinary: tag de lista invalida");
    return 0;
}

static ZNode *dnode(Dec *d) {
    if (++d->prof > ZAP_MAX_PROF) DFAIL(d, "wabinary: aninhamento excessivo");
    int t = dbyte(d);
    size_t size = dlist_size(d, t);
    int dt = dbyte(d);
    ZNode *n = (ZNode *)arena_get(d->ar, sizeof *n);
    if (!n) DFAIL(d, "sem memoria");
    if (dt == T_STREAM_END) {
        n->tag = arena_str(d->ar, "xmlstreamend", 12);
        n->kind = ZK_NADA;
        d->prof--;
        return n;
    }
    Str h = dstr(d, dt);
    if (size == 0 || h.n == 0) DFAIL(d, "wabinary: no invalido");
    size_t na = (size - 1) / 2;
    n->tag = h.s;
    n->na = (int)na;
    if (na) {
        n->ak = (char **)arena_get(d->ar, sizeof(char *) * na);
        n->av = (char **)arena_get(d->ar, sizeof(char *) * na);
        if (!n->ak || !n->av) DFAIL(d, "sem memoria");
    }
    for (size_t i = 0; i < na; i++) {
        Str k = dstr(d, dbyte(d));
        Str v = dstr(d, dbyte(d));
        n->ak[i] = k.s;
        n->av[i] = v.s;
    }
    if (size % 2 == 0) {
        int tg = dbyte(d);
        if (tg == T_LIST_EMPTY || tg == T_LIST8 || tg == T_LIST16) {
            size_t cnt = dlist_size(d, tg);
            n->kind = ZK_LISTA;
            n->nf = (int)cnt;
            if (cnt) {
                n->filhos = (ZNode **)arena_get(d->ar, sizeof(ZNode *) * cnt);
                if (!n->filhos) DFAIL(d, "sem memoria");
            }
            for (size_t i = 0; i < cnt; i++) n->filhos[i] = dnode(d);
        } else if (tg == T_BIN8 || tg == T_BIN20 || tg == T_BIN32) {
            size_t k = tg == T_BIN8 ? dbyte(d) : tg == T_BIN20 ? (size_t)dint20(d) : (size_t)dint(d, 4);
            const uint8_t *q = dbytes(d, k);
            uint8_t *c = (uint8_t *)arena_get(d->ar, k + 1);
            if (!c) DFAIL(d, "sem memoria");
            if (k) memcpy(c, q, k);
            n->kind = ZK_BYTES;
            n->binario = 1;
            n->dados = c;
            n->dn = k;
        } else {
            Str s = dstr(d, tg);
            n->kind = ZK_BYTES;
            n->binario = 0;
            n->dados = (uint8_t *)s.s;
            n->dn = s.n;
        }
    } else {
        n->kind = ZK_NADA;
    }
    d->prof--;
    return n;
}

/* ------------------------------------------------------------ inflate (zlib) */
#ifndef NG_NO_ZLIB
static int zinflate(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return -1;
    size_t cap = n * 4 + 64, usado = 0;
    uint8_t *o = (uint8_t *)malloc(cap);
    if (!o) { inflateEnd(&z); return -1; }
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    for (;;) {
        if (usado == cap) {
            cap *= 2;
            uint8_t *q = (uint8_t *)realloc(o, cap);
            if (!q) { free(o); inflateEnd(&z); return -1; }
            o = q;
        }
        z.next_out = o + usado;
        z.avail_out = (uInt)(cap - usado);
        int r = inflate(&z, Z_NO_FLUSH);
        usado = (size_t)z.total_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && r != Z_BUF_ERROR) { free(o); inflateEnd(&z); return -1; }
        if (r == Z_BUF_ERROR && z.avail_in == 0) { free(o); inflateEnd(&z); return -1; }
    }
    inflateEnd(&z);
    *out = o;
    *on = usado;
    return 0;
}
#else
/* ------------------------------------------------------------ inflate embutido (RFC1951)
 * Usado quando a NG e compilada sem zlib (-DNG_NO_ZLIB, caso do Windows sem
 * libz MinGW): decoder deflate completo - blocos stored, fixed e dynamic
 * Huffman - mais o cabecalho zlib (CMF/FLG). Nao ha atalho: todo o quadro
 * comprimido trafega por aqui. */
#define ZI_MAXBITS 15

typedef struct {
    const uint8_t *in; size_t inlen, inpos;
    unsigned long hold; int bits;
    uint8_t *out; size_t outlen, outcap;
} ZI;

typedef struct { short count[ZI_MAXBITS + 1]; short symbol[288]; } ZIHuff;

/* le `need` bits (LSB-first, como o deflate) */
static int zi_bits(ZI *s, int need) {
    unsigned long val = s->hold;
    while (s->bits < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (unsigned long)s->in[s->inpos++] << s->bits;
        s->bits += 8;
    }
    s->hold = val >> need;
    s->bits -= need;
    return (int)(val & ((1UL << need) - 1));
}

/* monta a tabela canonica de Huffman. >=0 = ok (0 = completo), <0 = invalida. */
static int zi_construct(ZIHuff *h, const short *length, int n) {
    int symbol, len, left;
    short offs[ZI_MAXBITS + 2];
    for (len = 0; len <= ZI_MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;                 /* sem simbolos */
    left = 1;
    for (len = 1; len <= ZI_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;                    /* excedido */
    }
    offs[1] = 0;
    for (len = 1; len < ZI_MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;                                    /* >0 = incompleto (aceitavel em dist) */
}

static int zi_decode(ZI *s, const ZIHuff *h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= ZI_MAXBITS; len++) {
        int b = zi_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int zi_put(ZI *s, uint8_t b) {
    if (s->outlen == s->outcap) {
        size_t nc = s->outcap ? s->outcap * 2 : 4096;
        uint8_t *q = (uint8_t *)realloc(s->out, nc);
        if (!q) return -1;
        s->out = q; s->outcap = nc;
    }
    s->out[s->outlen++] = b;
    return 0;
}

static void zi_fixed(ZIHuff *lit, ZIHuff *dist) {
    short lengths[288];
    int i;
    for (i = 0; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    zi_construct(lit, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    zi_construct(dist, lengths, 30);
}

static int zi_dynamic(ZI *s, ZIHuff *lit, ZIHuff *dist) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320], cl[19];
    ZIHuff clen;
    int hlit, hdist, hclen, i, n;

    hlit = zi_bits(s, 5); if (hlit < 0) return -1; hlit += 257;
    hdist = zi_bits(s, 5); if (hdist < 0) return -1; hdist += 1;
    hclen = zi_bits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 288 || hdist > 32) return -1;

    for (i = 0; i < 19; i++) cl[i] = 0;
    for (i = 0; i < hclen; i++) {
        int v = zi_bits(s, 3);
        if (v < 0) return -1;
        cl[order[i]] = (short)v;
    }
    if (zi_construct(&clen, cl, 19) < 0) return -1;

    n = hlit + hdist;
    for (i = 0; i < n; i++) lengths[i] = 0;
    i = 0;
    while (i < n) {
        int sym = zi_decode(s, &clen);
        if (sym < 0) return -1;
        if (sym < 16) lengths[i++] = (short)sym;
        else {
            int rep, extra, prev;
            if (sym == 16) {
                if (i == 0) return -1;
                extra = zi_bits(s, 2); if (extra < 0) return -1;
                rep = 3 + extra; prev = lengths[i - 1];
                while (rep-- > 0) { if (i >= n) return -1; lengths[i++] = (short)prev; }
            } else if (sym == 17) {
                extra = zi_bits(s, 3); if (extra < 0) return -1;
                rep = 3 + extra;
                while (rep-- > 0) { if (i >= n) return -1; lengths[i++] = 0; }
            } else if (sym == 18) {
                extra = zi_bits(s, 7); if (extra < 0) return -1;
                rep = 11 + extra;
                while (rep-- > 0) { if (i >= n) return -1; lengths[i++] = 0; }
            } else return -1;
        }
    }
    if (lengths[256] == 0) return -1;               /* sem simbolo de fim de bloco */
    if (zi_construct(lit, lengths, hlit) < 0) return -1;
    if (zi_construct(dist, lengths + hlit, hdist) < 0) return -1;
    return 0;
}

/* um bloco deflate; *final = BFINAL do bloco */
static int zi_block(ZI *s, ZIHuff *lit, ZIHuff *dist, int *final) {
    static const short lenbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lenextra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short distbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
    static const short distextra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    int bfinal = zi_bits(s, 1); if (bfinal < 0) return -1;
    int btype = zi_bits(s, 2); if (btype < 0) return -1;
    *final = bfinal;

    if (btype == 0) {                                /* stored: LEN/NLEN + bytes crus */
        s->bits = 0; s->hold = 0;                    /* alinha no byte */
        if (s->inpos + 4 > s->inlen) return -1;
        unsigned len = (unsigned)s->in[s->inpos] | ((unsigned)s->in[s->inpos + 1] << 8);
        unsigned nlen = (unsigned)s->in[s->inpos + 2] | ((unsigned)s->in[s->inpos + 3] << 8);
        s->inpos += 4;
        if ((len ^ 0xffffu) != nlen) return -1;
        if (s->inpos + len > s->inlen) return -1;
        for (unsigned i = 0; i < len; i++) if (zi_put(s, s->in[s->inpos + i]) != 0) return -1;
        s->inpos += len;
        return 0;
    }
    if (btype == 3) return -1;

    if (btype == 1) zi_fixed(lit, dist);
    else if (zi_dynamic(s, lit, dist) != 0) return -1;

    for (;;) {
        int sym = zi_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) { if (zi_put(s, (uint8_t)sym) != 0) return -1; continue; }
        if (sym == 256) break;                       /* fim do bloco */
        sym -= 257;
        if (sym > 28) return -1;
        int extra = zi_bits(s, lenextra[sym]); if (extra < 0) return -1;
        int length = lenbase[sym] + extra;
        int dsym = zi_decode(s, dist);
        if (dsym < 0 || dsym > 29) return -1;
        int dextra = zi_bits(s, distextra[dsym]); if (dextra < 0) return -1;
        size_t distance = (size_t)distbase[dsym] + (size_t)dextra;
        if (distance == 0 || distance > s->outlen) return -1;
        for (int i = 0; i < length; i++) {           /* copia com sobreposicao */
            uint8_t b = s->out[s->outlen - distance];
            if (zi_put(s, b) != 0) return -1;
        }
    }
    return 0;
}

static int zinflate(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    ZI s;
    ZIHuff lit, dist;
    int final = 0;
    memset(&s, 0, sizeof s);
    s.in = in; s.inlen = n;
    if (n < 2) return -1;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8) return -1;                /* CM=8 (deflate) */
    if (((cmf << 8) | flg) % 31 != 0) return -1;      /* FCHECK */
    if (flg & 0x20) return -1;                       /* FDICT nao suportado */
    s.inpos = 2;
    while (!final) if (zi_block(&s, &lit, &dist, &final) != 0) { free(s.out); return -1; }
    if (!s.out) {                                    /* saida vazia */
        s.out = (uint8_t *)malloc(1);
        if (!s.out) return -1;
        s.out[0] = 0;
    }
    *out = s.out;
    *on = s.outlen;
    return 0;
}
#endif /* NG_NO_ZLIB */

/* Decodifica um quadro claro: byte de flags + no. flags & 2 => zlib comprimido. */
static ZNode *decode_quadro(const uint8_t *p, size_t n, Arena *ar, const char **erro) {
    if (n == 0) { *erro = "wabinary: quadro vazio"; return NULL; }
    uint8_t flags = p[0];
    const uint8_t *corpo = p + 1;
    size_t cn = n - 1;
    uint8_t *volatile desc = NULL;
    size_t dn = 0;
    if (flags & 2) {
        uint8_t *tmp = NULL;
        if (zinflate(corpo, cn, &tmp, &dn) != 0) { *erro = "wabinary: falha ao descomprimir"; return NULL; }
        desc = tmp;
        corpo = tmp; cn = dn;
    }
    Dec d;
    memset(&d, 0, sizeof d);
    d.b = corpo; d.n = cn; d.ar = ar;
    ZNode *r = NULL;
    if (setjmp(d.jb) == 0) r = dnode(&d);
    else { *erro = d.erro; r = NULL; }
    free(desc);
    return r;
}

/* ------------------------------------------------------------ encoder */
static void w_lista(Buf *b, size_t n) {
    if (n == 0) bputc(b, T_LIST_EMPTY);
    else if (n < 256) { bputc(b, T_LIST8); bputc(b, (unsigned)n); }
    else { bputc(b, T_LIST16); bputint(b, n, 2); }
}
static void w_raw(Buf *b, const uint8_t *s, size_t n) {
    if (n >= 1048576) { bputc(b, T_BIN32); bputint(b, n, 4); }
    else if (n >= 256) { bputc(b, T_BIN20); bputc(b, (unsigned)((n / 65536) % 16)); bputc(b, (unsigned)((n / 256) % 256)); bputc(b, (unsigned)(n % 256)); }
    else { bputc(b, T_BIN8); bputc(b, (unsigned)n); }
    bput(b, s, n);
}
static int is_pack(const char *s, size_t n, int hex) {
    if (n == 0 || n >= T_PACKED_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (hex) { if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0; }
        else if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
    }
    return 1;
}
static int pack_char(int c, int hex) {
    if (c < 0) return 15;
    if (hex) return hexdig(c);
    if (c == '-') return 10;
    if (c == '.') return 11;
    return hexdig(c);
}
static void w_packed(Buf *b, const char *s, size_t n, int hex) {
    bputc(b, hex ? T_HEX8 : T_NIB8);
    size_t len = (n + 1) / 2;
    if (n % 2) len += 128;
    bputc(b, (unsigned)len);
    for (size_t i = 0; i < n; i += 2) {
        int a = pack_char(s[i], hex);
        int c = i + 1 < n ? pack_char(s[i + 1], hex) : 15;
        bputc(b, (unsigned)(a * 16 + c));
    }
}
static void w_string(Buf *b, const char *s, size_t n);

static void w_jid(Buf *b, const char *s, size_t n) {
    const char *arr = memchr(s, '@', n);
    size_t at = (size_t)(arr - s);
    size_t cl = at;
    const char *srv = s + at + 1;
    size_t sl = n - at - 1;
    /* combined = ua[:device]; ua/device so casam com ^([^:]*):?(%d*)$ */
    const char *dois = memchr(s, ':', cl);
    size_t ua_len = cl;
    size_t dev_len = 0;
    const char *dev = NULL;
    if (dois) {
        size_t c = (size_t)(dois - s);
        int digitos = 1;
        for (size_t i = c + 1; i < cl; i++) if (s[i] < '0' || s[i] > '9') { digitos = 0; break; }
        if (digitos) { ua_len = c; dev = dois + 1; dev_len = cl - c - 1; }
    }
    /* user = ua antes do primeiro '_' */
    size_t user_len = 0;
    while (user_len < ua_len && s[user_len] != '_') user_len++;
    int eh_lid = (sl == 3 && memcmp(srv, "lid", 3) == 0);
    if (dev_len) {
        unsigned long dv = 0;
        for (size_t i = 0; i < dev_len; i++) dv = dv * 10 + (unsigned long)(dev[i] - '0');
        if (dv > 255) { b->erro = 1; return; }
        bputc(b, T_AD);
        bputc(b, eh_lid ? 1 : 0);
        bputc(b, (unsigned)dv);
        w_string(b, s, user_len);
    } else {
        bputc(b, T_JID_PAIR);
        if (user_len) w_string(b, s, user_len);
        else bputc(b, T_LIST_EMPTY);
        w_string(b, srv, sl);
    }
}

static void w_string(Buf *b, const char *s, size_t n) {
    const TokEnt *t = tok_find(s, n);
    if (t) {
        if (t->dict >= 0) bputc(b, (unsigned)(T_DICT0 + t->dict));
        bputc(b, t->idx);
        return;
    }
    if (n == 0) { w_raw(b, (const uint8_t *)s, 0); return; }
    if (is_pack(s, n, 0)) { w_packed(b, s, n, 0); return; }
    if (is_pack(s, n, 1)) { w_packed(b, s, n, 1); return; }
    if (memchr(s, '@', n)) { w_jid(b, s, n); return; }
    w_raw(b, (const uint8_t *)s, n);
}

typedef struct { const char *k; const char *v; int ord; } AttrOrd;
static int cmp_ord(const void *a, const void *b) {
    const AttrOrd *x = (const AttrOrd *)a, *y = (const AttrOrd *)b;
    int c = strcmp(x->k, y->k);
    if (c) return c;
    return x->ord - y->ord;   /* chaves iguais: o ultimo vence (como tabela Lua) */
}

static int w_node(Buf *b, const ZNode *n, int prof) {
    if (prof > ZAP_MAX_PROF || !n || !n->tag) return -1;
    AttrOrd *ord = (AttrOrd *)malloc(sizeof(AttrOrd) * (size_t)(n->na ? n->na : 1));
    if (!ord) return -1;
    for (int i = 0; i < n->na; i++) { ord[i].k = n->ak[i]; ord[i].v = n->av[i]; ord[i].ord = i; }
    if (n->na > 1) qsort(ord, (size_t)n->na, sizeof(AttrOrd), cmp_ord);
    /* conta chaves unicas (mantendo a ultima ocorrencia) */
    int nk = 0;
    for (int i = 0; i < n->na; i++) if (i + 1 >= n->na || strcmp(ord[i].k, ord[i + 1].k) != 0) nk++;
    int tem_conteudo = n->kind != ZK_NADA;
    w_lista(b, (size_t)(2 * nk + 1 + (tem_conteudo ? 1 : 0)));
    w_string(b, n->tag, strlen(n->tag));
    for (int i = 0; i < n->na; i++) {
        if (i + 1 < n->na && strcmp(ord[i].k, ord[i + 1].k) == 0) continue;
        w_string(b, ord[i].k, strlen(ord[i].k));
        w_string(b, ord[i].v, strlen(ord[i].v));
    }
    free(ord);
    if (n->kind == ZK_BYTES) {
        if (n->binario) w_raw(b, n->dados, n->dn);
        else w_string(b, (const char *)n->dados, n->dn);
    } else if (n->kind == ZK_LISTA) {
        w_lista(b, (size_t)n->nf);
        for (int i = 0; i < n->nf; i++)
            if (w_node(b, n->filhos[i], prof + 1) != 0) return -1;
    }
    return b->erro ? -1 : 0;
}

/* Quadro claro = byte 0 (sem compressao) + no codificado. */
static int encode_quadro(const ZNode *n, Buf *out) {
    tok_init();
    bputc(out, 0);
    if (w_node(out, n, 0) != 0 || out->erro) return -1;
    return 0;
}

/* ------------------------------------------------------------ protobuf minimo */
static void pb_varint(Buf *b, uint64_t v) {
    while (v >= 128) { bputc(b, (unsigned)(v % 128 + 128)); v /= 128; }
    bputc(b, (unsigned)v);
}
static void pb_bytes(Buf *b, int campo, const uint8_t *s, size_t n) {
    pb_varint(b, (uint64_t)campo * 8 + 2);
    pb_varint(b, n);
    bput(b, s, n);
}
/* Primeiro campo com wire type 2 (bytes). Retorna 1 se achou, 0 se nao, -1 se malformado. */
static int pb_find(const uint8_t *p, size_t n, int campo, const uint8_t **v, size_t *vn) {
    size_t pos = 0;
    while (pos < n) {
        uint64_t tag = 0; int shift = 0;
        for (;;) {
            if (pos >= n || shift > 63) return -1;
            uint8_t c = p[pos++];
            tag |= (uint64_t)(c & 127) << shift;
            shift += 7;
            if (c < 128) break;
        }
        int wt = (int)(tag & 7), fld = (int)(tag >> 3);
        if (wt == 0) {
            while (pos < n && (p[pos] & 128)) pos++;
            if (pos >= n) return -1;
            pos++;
        } else if (wt == 2) {
            uint64_t len = 0; shift = 0;
            for (;;) {
                if (pos >= n || shift > 63) return -1;
                uint8_t c = p[pos++];
                len |= (uint64_t)(c & 127) << shift;
                shift += 7;
                if (c < 128) break;
            }
            if (len > n - pos) return -1;
            if (fld == campo) { *v = p + pos; *vn = (size_t)len; return 1; }
            pos += (size_t)len;
        } else if (wt == 1) { if (n - pos < 8) return -1; pos += 8; }
        else if (wt == 5) { if (n - pos < 4) return -1; pos += 4; }
        else return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ Noise */
typedef struct {
    uint8_t h[32], salt[32], key[32];
    uint32_t ctr;
    int pronto;               /* finish() ja chamado */
} Noise;

static void ns_auth(Noise *z, const uint8_t *d, size_t n) {
    if (z->pronto) return;
    uint8_t *tmp = (uint8_t *)malloc(32 + n);
    if (!tmp) return;
    memcpy(tmp, z->h, 32);
    if (n) memcpy(tmp + 32, d, n);
    sha256(tmp, 32 + n, z->h);
    free(tmp);
}
static void iv_de(uint32_t c, uint8_t iv[12]) {
    memset(iv, 0, 8);
    iv[8] = (uint8_t)(c >> 24); iv[9] = (uint8_t)(c >> 16); iv[10] = (uint8_t)(c >> 8); iv[11] = (uint8_t)c;
}
/* Cifra: devolve ct||tag (n+16 bytes) em *out. */
static int ns_encrypt(Noise *z, const uint8_t *pt, size_t n, uint8_t **out, size_t *on) {
    uint8_t iv[12], tag[16];
    iv_de(z->ctr, iv);
    uint8_t *o = (uint8_t *)malloc(n + 16);
    if (!o) return -1;
    if (gcm_run(z->key, 32, iv, 12, z->h, 32, pt ? pt : VAZIO, n, o, tag, 1) != 0) { free(o); return -1; }
    memcpy(o + n, tag, 16);
    z->ctr++;
    ns_auth(z, o, n + 16);
    *out = o; *on = n + 16;
    return 0;
}
/* Decifra ct||tag. Falha => nao mexe no contador (igual noise.lua). */
static int ns_decrypt(Noise *z, const uint8_t *ct, size_t n, uint8_t **out, size_t *on) {
    if (n < 16) return -1;
    uint8_t iv[12], tag[16];
    iv_de(z->ctr, iv);
    size_t cl = n - 16;
    uint8_t *o = (uint8_t *)malloc(cl + 1);
    if (!o) return -1;
    if (gcm_run(z->key, 32, iv, 12, z->h, 32, ct, cl, o, tag, 0) != 0 || memcmp(tag, ct + cl, 16) != 0) {
        free(o);
        return -1;
    }
    z->ctr++;
    ns_auth(z, ct, n);
    *out = o; *on = cl;
    return 0;
}
static void ns_mix_into_key(Noise *z, const uint8_t *ikm, size_t n) {
    uint8_t o[64];
    hkdf256(ikm, n, z->salt, 32, VAZIO, 0, o, 64);
    memcpy(z->salt, o, 32);
    memcpy(z->key, o + 32, 32);
    z->ctr = 0;
}
static void ns_mix_dh(Noise *z, const uint8_t priv[32], const uint8_t pub[32]) {
    uint8_t q[32];
    x25519(q, priv, pub);
    ns_mix_into_key(z, q, 32);
}
static void ns_finish(Noise *z, uint8_t wkey[32], uint8_t rkey[32]) {
    uint8_t o[64];
    hkdf256(VAZIO, 0, z->salt, 32, VAZIO, 0, o, 64);
    z->pronto = 1;
    memcpy(wkey, o, 32);
    memcpy(rkey, o + 32, 32);
}

/* ------------------------------------------------------------ sessoes */
typedef struct {
    int usada;
    int fase;                 /* 0 = esperando ServerHello, 1 = transporte */
    uint8_t priv[32], pub[32];
    Noise ns;
    uint8_t wkey[32], rkey[32];
    uint32_t wc, rc;
    Buf rx;                   /* bytes recebidos ainda nao separados em quadros */
} Sessao;

static Sessao sessoes[ZAP_SESSOES + 1];
static char zap_erro[160] = "";

static void erro(const char *m) { snprintf(zap_erro, sizeof zap_erro, "%s", m); }

static int aleatorio(uint8_t *b, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(b, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

static int sessao_id_ok(int h) { return h >= 1 && h <= ZAP_SESSOES && sessoes[h].usada; }

static int zap_iniciar_com_chave(const uint8_t *priv_opt) {
    tok_init();
    int h = 0;
    for (int i = 1; i <= ZAP_SESSOES; i++) if (!sessoes[i].usada) { h = i; break; }
    if (!h) { erro("limite de sessoes atingido"); return -1; }
    Sessao *s = &sessoes[h];
    memset(s, 0, sizeof *s);
    s->usada = 1;
    if (priv_opt) memcpy(s->priv, priv_opt, 32);
    else if (aleatorio(s->priv, 32) != 0) { erro("sem /dev/urandom"); memset(s, 0, sizeof *s); return -1; }
    x25519(s->pub, s->priv, BASE_X25519);
    /* noise.new(crypto, WA_HEADER) + authenticate(eph_pub) */
    memcpy(s->ns.h, NOISE_MODE, 32);
    memcpy(s->ns.salt, NOISE_MODE, 32);
    memcpy(s->ns.key, NOISE_MODE, 32);
    ns_auth(&s->ns, WA_HEADER, 4);
    ns_auth(&s->ns, s->pub, 32);
    return h;
}

static void zap_fechar(int h) {
    if (!sessao_id_ok(h)) return;
    buf_free(&sessoes[h].rx);
    memset(&sessoes[h], 0, sizeof sessoes[h]);
}

/* Separa quadros completos (3 bytes de tamanho + corpo) do buffer de recepcao. */
static int quadros_extrair(Sessao *s, char ***lista, size_t *nq) {
    *lista = NULL; *nq = 0;
    size_t cap = 0, pos = 0;
    while (s->rx.n - pos >= 3) {
        size_t len = ((size_t)s->rx.p[pos] << 16) | ((size_t)s->rx.p[pos + 1] << 8) | s->rx.p[pos + 2];
        if (len > ZAP_MAX_QUADRO) { erro("quadro maior que o limite"); return -1; }
        if (s->rx.n - pos - 3 < len) break;
        if (*nq == cap) {
            size_t c = cap ? cap * 2 : 8;
            char **q = (char **)realloc(*lista, c * sizeof(char *));
            if (!q) return -1;
            *lista = q; cap = c;
        }
        char *hx = tohex(s->rx.p + pos + 3, len);
        if (!hx) return -1;
        (*lista)[(*nq)++] = hx;
        pos += 3 + len;
    }
    if (pos) {
        memmove(s->rx.p, s->rx.p + pos, s->rx.n - pos);
        s->rx.n -= pos;
    }
    return 0;
}

/* Quadro com prefixo de tamanho de 3 bytes. Retorna malloc'd. */
static uint8_t *quadro3(const uint8_t *corpo, size_t n, size_t *on) {
    uint8_t *o = (uint8_t *)malloc(n + 3);
    if (!o) return NULL;
    o[0] = (uint8_t)(n >> 16); o[1] = (uint8_t)(n >> 8); o[2] = (uint8_t)n;
    memcpy(o + 3, corpo, n);
    *on = n + 3;
    return o;
}

/* ServerHello -> ClientFinish. Mesma sequencia de pair.lua (on_frame, stage 'hello'). */
static int zap_processar_servidor(int h, const uint8_t *corpo, size_t cn,
                                  const uint8_t npriv[32], const uint8_t npub[32],
                                  const uint8_t *payload, size_t pn, uint8_t **out, size_t *on) {
    Sessao *s = &sessoes[h];
    const uint8_t *hm3; size_t hm3n;
    if (s->fase != 0) { erro("handshake ja concluido"); return -1; }
    if (pb_find(corpo, cn, 3, &hm3, &hm3n) != 1) { erro("FALHA: ServerHello invalido"); return -1; }
    const uint8_t *seph, *sst, *spay; size_t sephn, sstn, spayn;
    if (pb_find(hm3, hm3n, 1, &seph, &sephn) != 1 || pb_find(hm3, hm3n, 2, &sst, &sstn) != 1 ||
        pb_find(hm3, hm3n, 3, &spay, &spayn) != 1 || sephn != 32) {
        erro("FALHA: ServerHello invalido");
        return -1;
    }
    Noise *ns = &s->ns;
    uint8_t *tmp = NULL; size_t tn = 0;
    ns_auth(ns, seph, sephn);
    ns_mix_dh(ns, s->priv, seph);
    if (ns_decrypt(ns, sst, sstn, &tmp, &tn) != 0) { erro("FALHA: decrypt do static"); return -1; }
    if (tn != 32) { free(tmp); erro("FALHA: static com tamanho errado"); return -1; }
    uint8_t sstatic[32];
    memcpy(sstatic, tmp, 32);
    free(tmp); tmp = NULL;
    ns_mix_dh(ns, s->priv, sstatic);
    if (ns_decrypt(ns, spay, spayn, &tmp, &tn) != 0) { erro("FALHA: decrypt do certificado"); return -1; }
    free(tmp); tmp = NULL;

    uint8_t *key_enc = NULL, *pay_enc = NULL; size_t kel = 0, pel = 0;
    if (ns_encrypt(ns, npub, 32, &key_enc, &kel) != 0) { erro("FALHA: cifra da chave"); return -1; }
    ns_mix_dh(ns, npriv, seph);
    if (ns_encrypt(ns, payload, pn, &pay_enc, &pel) != 0) { free(key_enc); erro("FALHA: cifra do payload"); return -1; }

    Buf msg = {0};
    Buf interno = {0};
    pb_bytes(&interno, 1, key_enc, kel);
    pb_bytes(&interno, 2, pay_enc, pel);
    pb_bytes(&msg, 4, interno.p, interno.n);
    free(key_enc); free(pay_enc);
    buf_free(&interno);
    uint8_t *q = quadro3(msg.p, msg.n, on);
    buf_free(&msg);
    if (!q) { erro("sem memoria"); return -1; }
    ns_finish(ns, s->wkey, s->rkey);
    s->fase = 1;
    *out = q;
    return 0;
}

/* Cifra um no no transporte: quadro = 3 bytes + GCM(wkey, iv(wc), [0]+no). */
static int zap_cifrar(int h, const ZNode *no, uint8_t **out, size_t *on) {
    Sessao *s = &sessoes[h];
    if (s->fase != 1) { erro("handshake nao concluido"); return -1; }
    Buf claro = {0};
    if (encode_quadro(no, &claro) != 0) { buf_free(&claro); erro("no invalido para codificar"); return -1; }
    uint8_t iv[12], tag[16];
    iv_de(s->wc, iv);
    size_t cn = claro.n;                     /* guarda antes de liberar o buffer */
    uint8_t *ct = (uint8_t *)malloc(cn + 16);
    if (!ct) { buf_free(&claro); erro("sem memoria"); return -1; }
    gcm_run(s->wkey, 32, iv, 12, VAZIO, 0, claro.p, cn, ct, tag, 1);
    memcpy(ct + cn, tag, 16);
    s->wc++;
    buf_free(&claro);
    uint8_t *q = quadro3(ct, cn + 16, on);
    free(ct);
    if (!q) { erro("sem memoria"); return -1; }
    *out = q;
    return 0;
}

/* Decifra um corpo de quadro do transporte. */
static ZNode *zap_decifrar(int h, const uint8_t *corpo, size_t cn, Arena *ar) {
    Sessao *s = &sessoes[h];
    if (s->fase != 1) { erro("handshake nao concluido"); return NULL; }
    if (cn < 16) { s->rc++; erro("quadro curto demais"); return NULL; }
    uint8_t iv[12], tag[16];
    iv_de(s->rc, iv);
    size_t pl = cn - 16;
    uint8_t *claro = (uint8_t *)malloc(pl + 1);
    if (!claro) { erro("sem memoria"); return NULL; }
    gcm_run(s->rkey, 32, iv, 12, VAZIO, 0, corpo, pl, claro, tag, 0);
    s->rc++;
    if (memcmp(tag, corpo + pl, 16) != 0) { free(claro); erro("GCM: tag invalida"); return NULL; }
    const char *e = NULL;
    ZNode *no = decode_quadro(claro, pl, ar, &e);
    free(claro);
    if (!no) erro(e ? e : "falha ao decodificar");
    return no;
}

/* ------------------------------------------------------------ NG <-> no */
static const char *tab_str(NGTable *t, const char *k) {
    NGValue v;
    if (ng_table_get(t, k, &v) && v.type == NG_VAL_STRING) return v.as.string;
    return NULL;
}
static NGTable *tab_tab(NGTable *t, const char *k) {
    NGValue v;
    if (ng_table_get(t, k, &v) && v.type == NG_VAL_TABLE) return v.as.table;
    return NULL;
}
static int tab_bool(NGTable *t, const char *k) {
    NGValue v;
    return ng_table_get(t, k, &v) && v.type == NG_VAL_BOOL && v.as.boolean;
}
static char *dup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static ZNode *no_de_ng(NGTable *t, Arena *ar, int prof);

/* Valor de atributo como texto (string, numero inteiro, bool). */
static int valor_texto(NGValue v, char *buf, size_t bs, const char **out) {
    if (v.type == NG_VAL_STRING && v.as.string) { *out = v.as.string; return 1; }
    if (v.type == NG_VAL_NUMBER) { snprintf(buf, bs, "%lld", (long long)v.as.number); *out = buf; return 1; }
    if (v.type == NG_VAL_BOOL) { *out = v.as.boolean ? "true" : "false"; return 1; }
    return 0;
}

static ZNode *no_de_ng(NGTable *t, Arena *ar, int prof) {
    if (!t || prof > ZAP_MAX_PROF) return NULL;
    const char *tag = tab_str(t, "tag");
    if (!tag || !*tag) return NULL;
    ZNode *n = (ZNode *)arena_get(ar, sizeof *n);
    if (!n) return NULL;
    n->tag = arena_str(ar, tag, strlen(tag));
    NGTable *at = tab_tab(t, "attrs");
    if (at) {
        size_t cnt = 0;
        for (size_t i = 0; i < at->size; i++)
            for (NGEntry *e = at->buckets[i]; e; e = e->next) {
                if (e->key[0] == '_') continue;
                char b2[32]; const char *s;
                if (valor_texto(e->value, b2, sizeof b2, &s)) cnt++;
            }
        n->na = (int)cnt;
        if (cnt) {
            n->ak = (char **)arena_get(ar, sizeof(char *) * cnt);
            n->av = (char **)arena_get(ar, sizeof(char *) * cnt);
            if (!n->ak || !n->av) return NULL;
        }
        size_t k = 0;
        for (size_t i = 0; i < at->size; i++)
            for (NGEntry *e = at->buckets[i]; e; e = e->next) {
                if (e->key[0] == '_') continue;
                char b2[32]; const char *s;
                if (!valor_texto(e->value, b2, sizeof b2, &s)) continue;
                n->ak[k] = arena_str(ar, e->key, strlen(e->key));
                n->av[k] = arena_str(ar, s, strlen(s));
                if (!n->ak[k] || !n->av[k]) return NULL;
                k++;
            }
        n->na = (int)k;
    }
    NGValue c;
    if (!ng_table_get(t, "content", &c)) { n->kind = ZK_NADA; return n; }
    if (c.type == NG_VAL_STRING && c.as.string) {
        size_t cl = strlen(c.as.string);
        if (tab_bool(t, "binario")) {
            size_t bn = 0;
            uint8_t *bytes = unhex(c.as.string, &bn);
            if (!bytes) return NULL;
            uint8_t *cp = (uint8_t *)arena_get(ar, bn + 1);
            if (!cp) { free(bytes); return NULL; }
            if (bn) memcpy(cp, bytes, bn);
            free(bytes);
            n->kind = ZK_BYTES; n->binario = 1; n->dados = cp; n->dn = bn;
        } else {
            n->kind = ZK_BYTES; n->binario = 0;
            n->dados = (uint8_t *)arena_str(ar, c.as.string, cl);
            n->dn = cl;
        }
        return n;
    }
    if (c.type == NG_VAL_NUMBER) {
        char b2[32];
        snprintf(b2, sizeof b2, "%lld", (long long)c.as.number);
        n->kind = ZK_BYTES; n->binario = 0;
        n->dados = (uint8_t *)arena_str(ar, b2, strlen(b2));
        n->dn = strlen(b2);
        return n;
    }
    if (c.type == NG_VAL_TABLE && c.as.table) {
        NGTable *lt = c.as.table;
        NGValue len;
        size_t cnt = 0;
        if (ng_table_get(lt, "length", &len) && len.type == NG_VAL_NUMBER && len.as.number > 0) cnt = (size_t)len.as.number;
        n->kind = ZK_LISTA;
        n->nf = (int)cnt;
        if (cnt) {
            n->filhos = (ZNode **)arena_get(ar, sizeof(ZNode *) * cnt);
            if (!n->filhos) return NULL;
        }
        char ib[24];
        for (size_t i = 0; i < cnt; i++) {
            const char *ks = ng_index_key((long long)i, ib, sizeof ib);
            NGTable *filho = tab_tab(lt, ks);
            if (!filho) return NULL;
            n->filhos[i] = no_de_ng(filho, ar, prof + 1);
            if (!n->filhos[i]) return NULL;
        }
        return n;
    }
    return NULL;
}

static NGValue ng_string_copia(const uint8_t *p, size_t n) {
    char *s = dup_n((const char *)p, n);
    if (!s) return ng_val_null();
    return ng_val_string_take(s);
}

/* Bytes "de texto": UTF-8 valido sem controles (exceto \t \n \r). Usado para decidir
 * se um conteudo BINARY_* deve ser entregue como texto ou como hex. */
static int e_texto(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return 0;
            if (c == 0x7f) return 0;
            i++;
            continue;
        }
        int extra = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || (size_t)extra >= n - i) return 0;
        for (int k = 1; k <= extra; k++) if ((p[i + k] >> 6) != 2) return 0;
        i += (size_t)extra + 1;
    }
    return 1;
}

static NGTable *no_para_ng(const ZNode *n, int prof) {
    NGTable *t = ng_table_new(8);
    ng_table_set(t, "tag", ng_val_string(n->tag));
    NGTable *at = ng_table_new(8);
    for (int i = 0; i < n->na; i++) ng_table_set(at, n->ak[i], ng_val_string(n->av[i]));
    ng_table_set(t, "attrs", ng_val_table(at));
    if (n->kind == ZK_BYTES) {
        if (n->binario && !e_texto(n->dados, n->dn)) {
            char *hx = tohex(n->dados, n->dn);
            ng_table_set(t, "content", ng_val_string_take(hx ? hx : dup_n("", 0)));
            ng_table_set(t, "binario", ng_val_bool(true));
        } else {
            ng_table_set(t, "content", ng_string_copia(n->dados, n->dn));
        }
    } else if (n->kind == ZK_LISTA) {
        NGTable *arr = ng_table_new(8);
        ng_table_set(arr, "_is_array", ng_val_bool(true));
        ng_table_set(arr, "length", ng_val_number((double)n->nf));
        char ib[24];
        for (int i = 0; i < n->nf && prof < ZAP_MAX_PROF; i++) {
            const char *ks = ng_index_key((long long)i, ib, sizeof ib);
            ng_table_set(arr, ks, ng_val_table(no_para_ng(n->filhos[i], prof + 1)));
        }
        ng_table_set(t, "content", ng_val_table(arr));
    }
    return t;
}

/* ------------------------------------------------------------ ponte NG */
static int h_arg(NGValue *args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_NUMBER) return -1;
    int h = (int)args[0].as.number;
    return sessao_id_ok(h) ? h : -1;
}
static NGValue hex_ou_nulo(char *hx) {
    if (!hx) return ng_val_null();
    return ng_val_string_take(hx);
}

static NGValue f_iniciar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    const uint8_t *priv = NULL;
    uint8_t chave[32];
    if (n >= 1 && a[0].type == NG_VAL_STRING) {
        size_t kn = 0;
        uint8_t *p = unhex(a[0].as.string, &kn);
        if (!p || kn != 32) { free(p); erro("chave noise deve ter 32 bytes em hex"); return ng_val_null(); }
        memcpy(chave, p, 32);
        free(p);
        priv = chave;
    }
    int h = zap_iniciar_com_chave(priv);
    return h < 0 ? ng_val_null() : ng_val_number((double)h);
}
static NGValue f_quadro_inicial(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0) { erro("sessao invalida"); return ng_val_null(); }
    Sessao *s = &sessoes[h];
    Buf b = {0};
    bput(&b, WA_HEADER, 4);
    Buf inner = {0};
    pb_bytes(&inner, 1, s->pub, 32);
    Buf msg = {0};
    pb_bytes(&msg, 2, inner.p, inner.n);
    size_t ql = 0;
    uint8_t *q = quadro3(msg.p, msg.n, &ql);
    if (q) bput(&b, q, ql);
    free(q);
    buf_free(&inner); buf_free(&msg);
    char *hx = tohex(b.p, b.n);
    buf_free(&b);
    return hex_ou_nulo(hx);
}
static NGValue f_receber(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0 || n < 2 || a[1].type != NG_VAL_STRING) { erro("uso: Zap.Receber(h, hex)"); return ng_val_null(); }
    Sessao *s = &sessoes[h];
    size_t cn = 0;
    uint8_t *chunk = unhex(a[1].as.string, &cn);
    if (!chunk) { erro("hex invalido"); return ng_val_null(); }
    bput(&s->rx, chunk, cn);
    free(chunk);
    if (s->rx.erro) { erro("sem memoria"); return ng_val_null(); }
    char **lista = NULL; size_t nq = 0;
    if (quadros_extrair(s, &lista, &nq) != 0) { free(lista); return ng_val_null(); }
    NGTable *arr = ng_table_new(8);
    ng_table_set(arr, "_is_array", ng_val_bool(true));
    ng_table_set(arr, "length", ng_val_number((double)nq));
    char ib[24];
    for (size_t i = 0; i < nq; i++) {
        ng_table_set(arr, ng_index_key((long long)i, ib, sizeof ib), ng_val_string_take(lista[i]));
    }
    free(lista);
    return ng_val_table(arr);
}
static NGValue f_processar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0 || n < 5) { erro("uso: Zap.ProcessarServidor(h, hex, nPriv, nPub, payloadHex)"); return ng_val_null(); }
    for (int i = 1; i <= 4; i++) if (a[i].type != NG_VAL_STRING) { erro("argumentos devem ser hex"); return ng_val_null(); }
    size_t cn = 0, pn = 0, npn = 0, nqn = 0;
    uint8_t *corpo = unhex(a[1].as.string, &cn);
    uint8_t *npriv = unhex(a[2].as.string, &npn);
    uint8_t *npub = unhex(a[3].as.string, &nqn);
    uint8_t *payload = unhex(a[4].as.string, &pn);
    NGValue res = ng_val_null();
    if (corpo && npriv && npub && payload && npn == 32 && nqn == 32) {
        uint8_t *q = NULL; size_t ql = 0;
        if (zap_processar_servidor(h, corpo, cn, npriv, npub, payload, pn, &q, &ql) == 0) {
            res = ng_val_string_take(tohex(q, ql));
        }
        free(q);
    } else if (!(npn == 32 && nqn == 32)) {
        erro("chaves do noise devem ter 32 bytes");
    } else {
        erro("hex invalido");
    }
    free(corpo); free(npriv); free(npub); free(payload);
    return res;
}
static NGValue f_cifrar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0 || n < 2 || a[1].type != NG_VAL_TABLE) { erro("uso: Zap.Cifrar(h, no)"); return ng_val_null(); }
    Arena ar = {0};
    ZNode *no = no_de_ng(a[1].as.table, &ar, 0);
    if (!no) { arena_free(&ar); erro("no invalido: precisa de tag"); return ng_val_null(); }
    uint8_t *q = NULL; size_t ql = 0;
    NGValue res = ng_val_null();
    if (zap_cifrar(h, no, &q, &ql) == 0) res = ng_val_string_take(tohex(q, ql));
    free(q);
    arena_free(&ar);
    return res;
}
static NGValue f_decifrar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0 || n < 2 || a[1].type != NG_VAL_STRING) { erro("uso: Zap.Decifrar(h, hex)"); return ng_val_null(); }
    size_t cn = 0;
    uint8_t *corpo = unhex(a[1].as.string, &cn);
    if (!corpo) { erro("hex invalido"); return ng_val_null(); }
    Arena ar = {0};
    ZNode *no = zap_decifrar(h, corpo, cn, &ar);
    free(corpo);
    NGValue res = ng_val_null();
    if (no) res = ng_val_table(no_para_ng(no, 0));
    arena_free(&ar);
    return res;
}
static NGValue f_codificar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    if (n < 1 || a[0].type != NG_VAL_TABLE) { erro("uso: Zap.Codificar(no)"); return ng_val_null(); }
    Arena ar = {0};
    ZNode *no = no_de_ng(a[0].as.table, &ar, 0);
    if (!no) { arena_free(&ar); erro("no invalido: precisa de tag"); return ng_val_null(); }
    Buf b = {0};
    NGValue res = ng_val_null();
    if (encode_quadro(no, &b) == 0) res = ng_val_string_take(tohex(b.p, b.n));
    else erro("falha ao codificar");
    buf_free(&b);
    arena_free(&ar);
    return res;
}
static NGValue f_decodificar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    if (n < 1 || a[0].type != NG_VAL_STRING) { erro("uso: Zap.Decodificar(hex)"); return ng_val_null(); }
    size_t cn = 0;
    uint8_t *p = unhex(a[0].as.string, &cn);
    if (!p) { erro("hex invalido"); return ng_val_null(); }
    Arena ar = {0};
    const char *e = NULL;
    ZNode *no = decode_quadro(p, cn, &ar, &e);
    free(p);
    NGValue res = ng_val_null();
    if (no) res = ng_val_table(no_para_ng(no, 0));
    else erro(e ? e : "falha ao decodificar");
    arena_free(&ar);
    return res;
}
static NGValue f_fechar(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in;
    int h = h_arg(a, n);
    if (h < 0) return ng_val_bool(false);
    zap_fechar(h);
    return ng_val_bool(true);
}
static NGValue f_erro(NGInterpreter *in, NGValue *a, size_t n) {
    (void)in; (void)a; (void)n;
    return ng_val_string(zap_erro);
}

static void load_zap(NGInterpreter *interp) {
    tok_init();
    NGTable *t = ng_table_new(16);
    ng_table_set(t, "Iniciar", ng_val_native_func("Iniciar", f_iniciar));
    ng_table_set(t, "QuadroInicial", ng_val_native_func("QuadroInicial", f_quadro_inicial));
    ng_table_set(t, "Receber", ng_val_native_func("Receber", f_receber));
    ng_table_set(t, "ProcessarServidor", ng_val_native_func("ProcessarServidor", f_processar));
    ng_table_set(t, "Cifrar", ng_val_native_func("Cifrar", f_cifrar));
    ng_table_set(t, "Decifrar", ng_val_native_func("Decifrar", f_decifrar));
    ng_table_set(t, "Codificar", ng_val_native_func("Codificar", f_codificar));
    ng_table_set(t, "Decodificar", ng_val_native_func("Decodificar", f_decodificar));
    ng_table_set(t, "Fechar", ng_val_native_func("Fechar", f_fechar));
    ng_table_set(t, "Erro", ng_val_native_func("Erro", f_erro));
    ng_env_set(interp->globals, "Zap", ng_val_table(t));
}

void ng_zap_module_register(void) {
    ng_native_module_register("zap", load_zap);
}
