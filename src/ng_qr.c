/* ng_qr.c - Biblioteca nativa @qr da NG (gerador de QR Code, C11 puro, sem dependencias).
 *
 * Uso na NG:
 *   Importar @qr
 *   Print QR.Terminal("texto")             # QR colorido (ANSI) para o terminal
 *   QR.SalvarPNG("texto", "qr.png", 8)     # grava PNG (escala em pixels/modulo)
 *   QR.SalvarSVG("texto", "qr.svg")        # grava SVG
 *   M = QR.Matriz("texto")                 # linhas de '0'/'1' separadas por \n
 *
 * Suporta modo byte, correcao de erro L/M/Q/H (padrao M), versoes 1 a 40.
 * Algoritmo conforme ISO/IEC 18004 (mascaras escolhidas pela menor penalidade).
 */
#include "ng_qr.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QR_VMIN 1
#define QR_VMAX 40

/* ordem das tabelas: 0=L 1=M 2=Q 3=H */
static const int8_t ECC_PER_BLOCK[4][41] = {
    {-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};
static const int8_t NUM_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};
/* bits de formato: L=01 M=00 Q=11 H=10 */
static const int FORMAT_BITS[4] = {1, 0, 3, 2};

typedef struct {
    int version, size, ecc, mask;
    uint8_t *mod;   /* 1 = escuro */
    uint8_t *fn;    /* 1 = modulo de funcao (nao recebe dados) */
} QR;

static int qr_get(const QR *q, int x, int y) { return q->mod[y * q->size + x]; }
static void qr_set(QR *q, int x, int y, int dark) { q->mod[y * q->size + x] = (uint8_t)(dark != 0); }
static void qr_fn(QR *q, int x, int y, int dark) {
    q->mod[y * q->size + x] = (uint8_t)(dark != 0);
    q->fn[y * q->size + x] = 1;
}

static int num_raw_modules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        result -= (25 * na - 10) * na - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}
static int num_data_codewords(int ver, int ecc) {
    return num_raw_modules(ver) / 8 - ECC_PER_BLOCK[ecc][ver] * NUM_BLOCKS[ecc][ver];
}

/* ---- Reed-Solomon em GF(256) / 0x11D ---- */
static uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}
static void rs_divisor(int degree, uint8_t *result) {
    memset(result, 0, (size_t)degree);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            result[j] = gf_mul(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}
static void rs_remainder(const uint8_t *data, int len, const uint8_t *div, int degree, uint8_t *result) {
    memset(result, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ result[0];
        memmove(result, result + 1, (size_t)degree - 1);
        result[degree - 1] = 0;
        for (int j = 0; j < degree; j++) result[j] ^= gf_mul(div[j], factor);
    }
}

/* ---- desenho dos padroes de funcao ---- */
static void draw_finder(QR *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            int x = cx + dx, y = cy + dy;
            if (x >= 0 && x < q->size && y >= 0 && y < q->size) qr_fn(q, x, y, dist != 2 && dist != 4);
        }
}
static void draw_align(QR *q, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            qr_fn(q, cx + dx, cy + dy, dist != 1);
        }
}
static int align_positions(int ver, int *out) {
    if (ver == 1) return 0;
    int n = ver / 7 + 2;
    int size = ver * 4 + 17;
    int step = (ver == 32) ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    out[0] = 6;
    for (int i = n - 1, pos = size - 7; i >= 1; i--, pos -= step) out[i] = pos;
    return n;
}
static void draw_format(QR *q, int mask) {
    int data = FORMAT_BITS[q->ecc] << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    for (int i = 0; i <= 5; i++) qr_fn(q, 8, i, (bits >> i) & 1);
    qr_fn(q, 8, 7, (bits >> 6) & 1);
    qr_fn(q, 8, 8, (bits >> 7) & 1);
    qr_fn(q, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) qr_fn(q, 14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) qr_fn(q, q->size - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) qr_fn(q, 8, q->size - 15 + i, (bits >> i) & 1);
    qr_fn(q, 8, q->size - 8, 1);
}
static void draw_version(QR *q) {
    if (q->version < 7) return;
    int rem = q->version;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    long bits = (long)q->version << 12 | rem;
    for (int i = 0; i < 18; i++) {
        int bit = (int)((bits >> i) & 1);
        int a = q->size - 11 + i % 3, b = i / 3;
        qr_fn(q, a, b, bit);
        qr_fn(q, b, a, bit);
    }
}
static void draw_function_patterns(QR *q) {
    for (int i = 0; i < q->size; i++) {
        qr_fn(q, 6, i, i % 2 == 0);
        qr_fn(q, i, 6, i % 2 == 0);
    }
    draw_finder(q, 3, 3);
    draw_finder(q, q->size - 4, 3);
    draw_finder(q, 3, q->size - 4);
    int pos[8];
    int n = align_positions(q->version, pos);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            draw_align(q, pos[i], pos[j]);
        }
    draw_format(q, 0);
    draw_version(q);
}

static void draw_codewords(QR *q, const uint8_t *data, int len) {
    int i = 0;
    for (int right = q->size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < q->size; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? q->size - 1 - vert : vert;
                if (!q->fn[y * q->size + x] && i < len * 8) {
                    qr_set(q, x, y, (data[i >> 3] >> (7 - (i & 7))) & 1);
                    i++;
                }
            }
    }
}
static void apply_mask(QR *q, int mask) {
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++) {
            int inv;
            switch (mask) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if (!q->fn[y * q->size + x] && inv) q->mod[y * q->size + x] ^= 1;
        }
}

/* ---- penalidade (regras N1..N4) ---- */
typedef struct { int h[7]; int size; } RunHist;
static int fp_count(const RunHist *r) {
    int n = r->h[1];
    int core = n > 0 && r->h[2] == n && r->h[3] == n * 3 && r->h[4] == n && r->h[5] == n;
    return (core && r->h[0] >= n * 4 && r->h[6] >= n ? 1 : 0) + (core && r->h[6] >= n * 4 && r->h[0] >= n ? 1 : 0);
}
static void fp_add(RunHist *r, int run) {
    if (r->h[0] == 0) run += r->size; /* margem clara virtual no inicio */
    memmove(r->h + 1, r->h, 6 * sizeof(int));
    r->h[0] = run;
}
static int fp_terminate(RunHist *r, int color, int run) {
    if (color) { fp_add(r, run); run = 0; }
    run += r->size; /* margem clara virtual no fim */
    fp_add(r, run);
    return fp_count(r);
}
static int line_penalty(const QR *q, int fixed, int horizontal) {
    int result = 0, color = 0, run = 0;
    RunHist r; memset(&r, 0, sizeof r); r.size = q->size;
    for (int k = 0; k < q->size; k++) {
        int bit = horizontal ? qr_get(q, k, fixed) : qr_get(q, fixed, k);
        if (bit == color) {
            run++;
            if (run == 5) result += 3;
            else if (run > 5) result++;
        } else {
            fp_add(&r, run);
            if (!color) result += fp_count(&r) * 40;
            color = bit;
            run = 1;
        }
    }
    result += fp_terminate(&r, color, run) * 40;
    return result;
}
static int penalty(const QR *q) {
    int result = 0, s = q->size;
    for (int i = 0; i < s; i++) {
        result += line_penalty(q, i, 1);
        result += line_penalty(q, i, 0);
    }
    for (int y = 0; y < s - 1; y++)
        for (int x = 0; x < s - 1; x++) {
            int c = qr_get(q, x, y);
            if (c == qr_get(q, x + 1, y) && c == qr_get(q, x, y + 1) && c == qr_get(q, x + 1, y + 1)) result += 3;
        }
    int dark = 0;
    for (int i = 0; i < s * s; i++) dark += q->mod[i];
    int total = s * s;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += (k < 0 ? 0 : k) * 10;
    return result;
}

static void add_bits(uint8_t *buf, int *bitlen, unsigned val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        buf[*bitlen >> 3] |= (uint8_t)(((val >> i) & 1) << (7 - (*bitlen & 7)));
        (*bitlen)++;
    }
}

/* Gera o QR. Retorna 0 em sucesso. O chamador libera q->mod e q->fn. */
static int qr_encode(const uint8_t *text, size_t len, int ecc, QR *q) {
    int ver, capbits = 0;
    for (ver = QR_VMIN; ver <= QR_VMAX; ver++) {
        int cc = ver < 10 ? 8 : 16;
        capbits = num_data_codewords(ver, ecc) * 8;
        long used = 4 + cc + (long)len * 8;
        if (used <= capbits && (cc == 16 || len < 256)) break;
    }
    if (ver > QR_VMAX) return -1;

    int datacw = capbits / 8;
    uint8_t *data = (uint8_t *)calloc((size_t)datacw + 1, 1);
    int bl = 0;
    add_bits(data, &bl, 4, 4);
    add_bits(data, &bl, (unsigned)len, ver < 10 ? 8 : 16);
    for (size_t i = 0; i < len; i++) add_bits(data, &bl, text[i], 8);
    int term = capbits - bl; if (term > 4) term = 4;
    bl += term;
    bl = (bl + 7) & ~7;
    for (uint8_t pad = 0xEC; bl < capbits; pad ^= (0xEC ^ 0x11)) add_bits(data, &bl, pad, 8);

    /* blocos + ECC + intercalacao */
    int nb = NUM_BLOCKS[ecc][ver], eccl = ECC_PER_BLOCK[ecc][ver];
    int raw = num_raw_modules(ver) / 8;
    int nshort = nb - raw % nb, shortlen = raw / nb;
    uint8_t div[40]; rs_divisor(eccl, div);
    uint8_t *all = (uint8_t *)malloc((size_t)raw);
    int off = 0;
    for (int i = 0; i < nb; i++) {
        int dlen = shortlen - eccl + (i < nshort ? 0 : 1);
        uint8_t rem[40];
        rs_remainder(data + off, dlen, div, eccl, rem);
        for (int j = 0, k = i; j < dlen; j++, k += nb) {
            if (j == shortlen - eccl) k -= nshort;
            all[k] = data[off + j];
        }
        /* ECC intercalado, apos todos os dados */
        for (int j = 0; j < eccl; j++) all[datacw + j * nb + i] = rem[j];
        off += dlen;
    }
    free(data);

    q->version = ver; q->size = ver * 4 + 17; q->ecc = ecc;
    q->mod = (uint8_t *)calloc((size_t)q->size * q->size, 1);
    q->fn = (uint8_t *)calloc((size_t)q->size * q->size, 1);
    draw_function_patterns(q);
    draw_codewords(q, all, raw);
    free(all);

    int best = 0, bestp = 1 << 30;
    for (int m = 0; m < 8; m++) {
        apply_mask(q, m);
        draw_format(q, m);
        int p = penalty(q);
        if (p < bestp) { bestp = p; best = m; }
        apply_mask(q, m); /* desfaz */
    }
    apply_mask(q, best);
    draw_format(q, best);
    q->mask = best;
    return 0;
}

static void qr_free(QR *q) { free(q->mod); free(q->fn); q->mod = q->fn = NULL; }

static int parse_ecc(NGValue *args, size_t argc, size_t idx) {
    if (argc > idx && args[idx].type == NG_VAL_STRING && args[idx].as.string[0]) {
        switch (args[idx].as.string[0]) {
            case 'L': case 'l': return 0;
            case 'Q': case 'q': return 2;
            case 'H': case 'h': return 3;
            default: return 1;
        }
    }
    return 1;
}

/* ---- saidas ---- */
static NGValue ng_qr_matriz(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    QR q;
    if (qr_encode((const uint8_t *)args[0].as.string, strlen(args[0].as.string), parse_ecc(args, argc, 1), &q) != 0) return ng_val_null();
    size_t n = (size_t)(q.size + 1) * q.size + 1;
    char *out = (char *)malloc(n);
    size_t p = 0;
    for (int y = 0; y < q.size; y++) {
        for (int x = 0; x < q.size; x++) out[p++] = qr_get(&q, x, y) ? '1' : '0';
        out[p++] = '\n';
    }
    out[p] = 0;
    qr_free(&q);
    return ng_val_string_take(out);
}

#define QUIET 2
static int is_dark(const QR *q, int x, int y) {
    if (x < 0 || y < 0 || x >= q->size || y >= q->size) return 0;
    return qr_get(q, x, y);
}

/* Cada celula do terminal mostra 2 modulos verticais com "▀": fg = modulo de cima,
   bg = modulo de baixo. Cores ANSI explicitas (preto/branco), independentes do tema. */
static NGValue ng_qr_terminal(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    QR q;
    if (qr_encode((const uint8_t *)args[0].as.string, strlen(args[0].as.string), parse_ecc(args, argc, 1), &q) != 0) return ng_val_null();
    int total = q.size + 2 * QUIET;
    size_t cap = (size_t)(total / 2 + 2) * ((size_t)total * 24 + 16) + 64;
    char *out = (char *)malloc(cap);
    size_t p = 0;
    for (int y = -QUIET; y < q.size + QUIET; y += 2) {
        for (int x = -QUIET; x < q.size + QUIET; x++) {
            int top = is_dark(&q, x, y), bot = is_dark(&q, x, y + 1);
            p += (size_t)sprintf(out + p, "\x1b[%d;%dm\xE2\x96\x80", top ? 30 : 97, bot ? 40 : 107);
        }
        p += (size_t)sprintf(out + p, "\x1b[0m\n");
    }
    out[p] = 0;
    qr_free(&q);
    return ng_val_string_take(out);
}

/* ---- PNG (deflate "stored", sem dependencias) ---- */
static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_update(uint32_t c, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, f);
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    put32(f, (uint32_t)len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    c = crc_update(c, data, len) ^ 0xFFFFFFFFu;
    put32(f, c);
}
static int write_png(const QR *q, const char *path, int scale) {
    crc_init();
    int dim = (q->size + 2 * 4) * scale;       /* margem de 4 modulos */
    size_t rowlen = (size_t)dim + 1;           /* 1 byte de filtro + 1 byte/pixel (cinza) */
    size_t rawlen = rowlen * (size_t)dim;
    uint8_t *raw = (uint8_t *)malloc(rawlen);
    if (!raw) return 0;
    for (int py = 0; py < dim; py++) {
        raw[(size_t)py * rowlen] = 0;
        for (int px = 0; px < dim; px++) {
            int mx = px / scale - 4, my = py / scale - 4;
            raw[(size_t)py * rowlen + 1 + (size_t)px] = is_dark(q, mx, my) ? 0 : 255;
        }
    }
    size_t nblocks = (rawlen + 65534) / 65535;
    size_t zlen = 2 + rawlen + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < rawlen; off += 65535) {
        size_t n = rawlen - off > 65535 ? 65535 : rawlen - off;
        z[zp++] = (off + n >= rawlen) ? 1 : 0;
        z[zp++] = (uint8_t)(n & 0xFF); z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 0xFF); z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n); zp += n;
    }
    uint32_t ad = (b << 16) | a;
    z[zp++] = (uint8_t)(ad >> 24); z[zp++] = (uint8_t)(ad >> 16); z[zp++] = (uint8_t)(ad >> 8); z[zp++] = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(dim >> 24), (uint8_t)(dim >> 16), (uint8_t)(dim >> 8), (uint8_t)dim,
                        (uint8_t)(dim >> 24), (uint8_t)(dim >> 16), (uint8_t)(dim >> 8), (uint8_t)dim, 8, 0, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zp);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}

static NGValue ng_qr_png(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    int scale = (argc >= 3 && args[2].type == NG_VAL_NUMBER) ? (int)args[2].as.number : 8;
    if (scale < 1) scale = 1;
    if (scale > 32) scale = 32;
    QR q;
    if (qr_encode((const uint8_t *)args[0].as.string, strlen(args[0].as.string), parse_ecc(args, argc, 3), &q) != 0) return ng_val_bool(false);
    int ok = write_png(&q, args[1].as.string, scale);
    qr_free(&q);
    return ng_val_bool(ok != 0);
}

/* ---- SVG ---- */
static NGValue ng_qr_svg(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    QR q;
    if (qr_encode((const uint8_t *)args[0].as.string, strlen(args[0].as.string), parse_ecc(args, argc, 2), &q) != 0) return ng_val_bool(false);
    FILE *f = fopen(args[1].as.string, "wb");
    if (!f) { qr_free(&q); return ng_val_bool(false); }
    int dim = q.size + 8;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" shape-rendering=\"crispEdges\">\n"
               "<rect width=\"%d\" height=\"%d\" fill=\"#fff\"/>\n<path fill=\"#000\" d=\"", dim, dim, dim, dim);
    for (int y = 0; y < q.size; y++) {
        int x = 0;
        while (x < q.size) {
            if (!qr_get(&q, x, y)) { x++; continue; }
            int x0 = x;
            while (x < q.size && qr_get(&q, x, y)) x++;
            fprintf(f, "M%d %dh%dv1h-%dz", x0 + 4, y + 4, x - x0, x - x0);
        }
    }
    fprintf(f, "\"/>\n</svg>\n");
    fclose(f);
    qr_free(&q);
    return ng_val_bool(true);
}

static void load_qr(NGInterpreter *interp) {
    NGTable *t = ng_table_new(8);
    ng_table_set(t, "Terminal", ng_val_native_func("Terminal", ng_qr_terminal));
    ng_table_set(t, "Matriz", ng_val_native_func("Matriz", ng_qr_matriz));
    ng_table_set(t, "SalvarPNG", ng_val_native_func("SalvarPNG", ng_qr_png));
    ng_table_set(t, "SalvarSVG", ng_val_native_func("SalvarSVG", ng_qr_svg));
    ng_env_set(interp->globals, "QR", ng_val_table(t));
}

void ng_qr_module_register(void) {
    ng_native_module_register("qr", load_qr);
    ng_native_module_register("qrcode", load_qr);
}
