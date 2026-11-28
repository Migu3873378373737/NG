/* ng_ws.c - Biblioteca nativa @ws da NG: cliente WebSocket (RFC 6455), escrito do zero em C.
 *
 *   Importar @ws
 *   Id = WS.Conectar("wss://servidor/caminho", Opcoes)   # Opcoes e opcional (tabela)
 *   WS.EnviarTexto(Id, "ola")                            # -> true/false
 *   WS.EnviarBinario(Id, "00ff10")                       # dados em HEX
 *   Msg = WS.Receber(Id, 5000)                           # espera ate 5000 ms; null se nao chegou nada
 *       Msg.tipo  = "texto" | "binario" | "fechado"
 *       Msg.dados = texto, ou HEX quando binario
 *   WS.Aberto(Id)    WS.Fechar(Id)    WS.Erro()          # Erro(): texto do ultimo erro
 *
 *   Opcoes (todas opcionais):
 *       Origin     = "https://web.whatsapp.com"   cabecalho Origin
 *       Cabecalhos = "X-Teste: 1\r\nOutro: 2"      cabecalhos extras
 *       Verificar  = false                         nao validar o certificado TLS (padrao: valida)
 *       CaArquivo  = "/caminho/cert.pem"           arquivo de CAs (padrao: o do sistema)
 *       Timeout    = 10000                         ms para conectar/handshake
 *
 * ping/pong e close sao tratados automaticamente. wss:// exige NG compilada com OpenSSL
 * (o Makefile ja detecta e liga -DNG_HAS_OPENSSL quando o OpenSSL esta instalado).
 */
#include "ng_ws.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_value.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include "../compat/windows/net_compat.h"
#include "../compat/windows/poll_compat.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef NG_HAS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifdef _WIN32
/* Winsock nao tem MSG_NOSIGNAL: erros de socket chegam como retorno. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
/* sockets Winsock usam closesocket(); o fd e guardado como int na Conn. */
#define close(fd) closesocket((SOCKET)(fd))
#endif

#define MAX_CONN 16
#define MAX_MSG (64u * 1024u * 1024u)

typedef struct {
    int used, fd, open;
    void *ssl;                 /* SSL* quando wss */
    void *ctx;                 /* SSL_CTX* */
    uint8_t *rb; size_t rlen, rcap;   /* bytes ja lidos e nao consumidos */
} Conn;

static Conn conns[MAX_CONN + 1];
static char last_error[256] = "";
static void set_err(const char *m) { snprintf(last_error, sizeof last_error, "%s", m); }

/* ------------------------------- SHA-1 (so para o handshake) ------------------------------- */
static void sha1(const uint8_t *d, size_t n, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t *m = (uint8_t *)calloc(total, 1);
    memcpy(m, d, n); m[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4*(size_t)i] << 24 | (uint32_t)m[off + 4*(size_t)i+1] << 16 | (uint32_t)m[off + 4*(size_t)i+2] << 8 | m[off + 4*(size_t)i+3];
        for (int i = 16; i < 80; i++) { uint32_t x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]; w[i] = (x << 1) | (x >> 31); }
        uint32_t a = h0, b = h1, c = h2, dd = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & dd); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ dd; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & dd) | (c & dd); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ dd; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = dd; dd = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += dd; h4 += e;
    }
    free(m);
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) { out[4*i] = (uint8_t)(hs[i] >> 24); out[4*i+1] = (uint8_t)(hs[i] >> 16); out[4*i+2] = (uint8_t)(hs[i] >> 8); out[4*i+3] = (uint8_t)hs[i]; }
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t *b, size_t n, char *s) {
    size_t p = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16;
        if (i + 1 < n) v |= (uint32_t)b[i+1] << 8;
        if (i + 2 < n) v |= b[i+2];
        s[p++] = B64[(v >> 18) & 63]; s[p++] = B64[(v >> 12) & 63];
        s[p++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        s[p++] = i + 2 < n ? B64[v & 63] : '=';
    }
    s[p] = 0;
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* ------------------------------- aleatoriedade e sockets ------------------------------- */
/* BCRYPT_USE_SYSTEM_RNG (0x00000008) nao esta definido nos headers deste
   MinGW; valor oficial do SDK. hAlgorithm=NULL exige esta flag. */
#ifndef BCRYPT_USE_SYSTEM_RNG
#define BCRYPT_USE_SYSTEM_RNG 0x00000008
#endif
/* bytes aleatorios: BCryptGenRandom no Windows, /dev/urandom no POSIX. */
static int ng_ws_random(uint8_t *out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_RNG) == 0 ? 0 : -1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    int ok = fread(out, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
#endif
}

/* connect() em andamento: EINPROGRESS no POSIX, WSAEWOULDBLOCK/INPROGRESS no Winsock. */
static int ng_ws_connect_in_progress(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

/* operacao de E/S interrompida/repetivel: EAGAIN/EINTR no POSIX,
   WSAEWOULDBLOCK/WSAEINTR no Winsock (recv/send nao setam errno la). */
static int ng_ws_retry_io(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EINTR;
#endif
}

/* ------------------------------- E/S de baixo nivel ------------------------------- */
static long io_read(Conn *c, uint8_t *buf, size_t n) {
#ifdef NG_HAS_OPENSSL
    if (c->ssl) {
        int r = SSL_read((SSL *)c->ssl, buf, (int)n);
        if (r > 0) return r;
        int e = SSL_get_error((SSL *)c->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return -2;
        return -1;
    }
#endif
    ssize_t r = recv(c->fd, (char *)buf, (int)n, 0);
    if (r > 0) return (long)r;
    if (r < 0 && ng_ws_retry_io()) return -2;
    return -1;
}
static int io_write_all(Conn *c, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        long w;
#ifdef NG_HAS_OPENSSL
        if (c->ssl) {
            int r = SSL_write((SSL *)c->ssl, buf + off, (int)(n - off));
            if (r <= 0) {
                int e = SSL_get_error((SSL *)c->ssl, r);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { struct pollfd p = {c->fd, POLLIN | POLLOUT, 0}; poll(&p, 1, 1000); continue; }
                return -1;
            }
            w = r;
        } else
#endif
        {
            ssize_t r = send(c->fd, (const char *)(buf + off), (int)(n - off), MSG_NOSIGNAL);
            if (r < 0) { if (ng_ws_retry_io()) { struct pollfd p = {c->fd, POLLOUT, 0}; poll(&p, 1, 1000); continue; } return -1; }
            w = (long)r;
        }
        off += (size_t)w;
    }
    return 0;
}
/* garante ao menos 'need' bytes em rb, esperando ate timeout_ms. 1=ok 0=timeout -1=erro/fechado */
static int fill(Conn *c, size_t need, int timeout_ms) {
    while (c->rlen < need) {
        int pending = 0;
#ifdef NG_HAS_OPENSSL
        if (c->ssl) pending = SSL_pending((SSL *)c->ssl) > 0;
#endif
        if (!pending) {
            struct pollfd p = {c->fd, POLLIN, 0};
            int pr = poll(&p, 1, timeout_ms);
            if (pr == 0) return 0;
            if (pr < 0) { if (ng_ws_retry_io()) continue; return -1; }
        }
        if (c->rcap - c->rlen < 4096) {
            size_t ncap = c->rcap ? c->rcap * 2 : 8192;
            uint8_t *nb = (uint8_t *)realloc(c->rb, ncap);
            if (!nb) return -1;
            c->rb = nb; c->rcap = ncap;
        }
        long r = io_read(c, c->rb + c->rlen, c->rcap - c->rlen);
        if (r == -2) continue;
        if (r < 0) return -1;
        c->rlen += (size_t)r;
    }
    return 1;
}
static void consume(Conn *c, size_t n) {
    memmove(c->rb, c->rb + n, c->rlen - n);
    c->rlen -= n;
}

static void conn_close(Conn *c) {
#ifdef NG_HAS_OPENSSL
    if (c->ssl) { SSL_shutdown((SSL *)c->ssl); SSL_free((SSL *)c->ssl); c->ssl = NULL; }
    if (c->ctx) { SSL_CTX_free((SSL_CTX *)c->ctx); c->ctx = NULL; }
#endif
    if (c->fd > 0) close(c->fd);
    free(c->rb);
    memset(c, 0, sizeof *c);
}

/* ------------------------------- quadros WebSocket ------------------------------- */
static int send_frame(Conn *c, int opcode, const uint8_t *data, size_t n) {
    uint8_t hdr[14]; size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (n < 126) hdr[h++] = (uint8_t)(0x80 | n);
    else if (n < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (uint8_t)(n >> 8); hdr[h++] = (uint8_t)n; }
    else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)n >> (8 * i)); }
    uint8_t mask[4];
    if (ng_ws_random(mask, 4) != 0) return -1;
    memcpy(hdr + h, mask, 4); h += 4;
    uint8_t *out = (uint8_t *)malloc(h + n);
    if (!out) return -1;
    memcpy(out, hdr, h);
    for (size_t i = 0; i < n; i++) out[h + i] = data[i] ^ mask[i & 3];
    int r = io_write_all(c, out, h + n);
    free(out);
    return r;
}

/* Le uma mensagem completa. Retorna 1 (msg), 0 (timeout), -1 (fechou/erro).
   *op = 1 texto, 2 binario, 8 close. Em caso de 1, *out e *outlen (malloc). */
static int recv_message(Conn *c, int timeout_ms, int *op, uint8_t **out, size_t *outlen) {
    uint8_t *acc = NULL; size_t acclen = 0; int accop = 0;
    int first = 1;
    for (;;) {
        int st = fill(c, 2, first ? timeout_ms : 30000);
        if (st <= 0) { free(acc); return st; }
        first = 0;
        int fin = (c->rb[0] & 0x80) != 0, opc = c->rb[0] & 0x0f, masked = (c->rb[1] & 0x80) != 0;
        uint64_t len = c->rb[1] & 0x7f; size_t hl = 2;
        if (len == 126) { if ((st = fill(c, 4, 30000)) <= 0) { free(acc); return -1; } len = (uint64_t)c->rb[2] << 8 | c->rb[3]; hl = 4; }
        else if (len == 127) { if ((st = fill(c, 10, 30000)) <= 0) { free(acc); return -1; } len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | c->rb[2 + i]; hl = 10; }
        if (len > MAX_MSG) { free(acc); return -1; }
        size_t mk = masked ? 4 : 0;
        if ((st = fill(c, hl + mk + (size_t)len, 30000)) <= 0) { free(acc); return -1; }
        uint8_t *pl = c->rb + hl + mk;
        if (masked) for (size_t i = 0; i < len; i++) pl[i] ^= c->rb[hl + (i & 3)];
        if (opc == 9) { send_frame(c, 10, pl, (size_t)len); consume(c, hl + mk + (size_t)len); continue; }   /* ping -> pong */
        if (opc == 10) { consume(c, hl + mk + (size_t)len); continue; }                                        /* pong */
        if (opc == 8) {
            send_frame(c, 8, pl, (size_t)len < 2 ? (size_t)len : 2);
            consume(c, hl + mk + (size_t)len);
            c->open = 0; free(acc); *op = 8; *out = NULL; *outlen = 0; return 1;
        }
        if (opc == 1 || opc == 2) accop = opc;
        uint8_t *na = (uint8_t *)realloc(acc, acclen + (size_t)len + 1);
        if (!na) { free(acc); return -1; }
        acc = na; memcpy(acc + acclen, pl, (size_t)len); acclen += (size_t)len; acc[acclen] = 0;
        consume(c, hl + mk + (size_t)len);
        if (fin) { *op = accop ? accop : 2; *out = acc; *outlen = acclen; return 1; }
    }
}

/* ------------------------------- conexao ------------------------------- */
static int parse_url(const char *url, int *tls, char *host, size_t hs, char *port, size_t ps, char *path, size_t pas) {
    const char *p;
    if (strncmp(url, "wss://", 6) == 0) { *tls = 1; p = url + 6; snprintf(port, ps, "443"); }
    else if (strncmp(url, "ws://", 5) == 0) { *tls = 0; p = url + 5; snprintf(port, ps, "80"); }
    else return -1;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    char hp[300]; if (hl >= sizeof hp) return -1;
    memcpy(hp, p, hl); hp[hl] = 0;
    char *colon = strrchr(hp, ':');
    if (colon && hp[0] != '[') { *colon = 0; snprintf(port, ps, "%s", colon + 1); }
    snprintf(host, hs, "%s", hp);
    snprintf(path, pas, "%s", slash ? slash : "/");
    return 0;
}

static int tcp_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) { set_err("DNS: nao foi possivel resolver o host"); return -1; }
#ifdef _WIN32
    /* Winsock precisa de WSAStartup antes de socket(); idempotente. */
    if (ng_win_net_init() != 0) { freeaddrinfo(res); set_err("TCP: falha ao iniciar o Winsock"); return -1; }
#endif
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#ifdef _WIN32
        { u_long nb = 1;
          if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) != 0) { close(fd); fd = -1; continue; } }
#else
        int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
        int r = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
        if (r < 0 && ng_ws_connect_in_progress()) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, timeout_ms) == 1) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
                r = err ? -1 : 0;
            } else r = -1;
        }
        if (r == 0) {
#ifdef _WIN32
            u_long nb = 0; ioctlsocket((SOCKET)fd, FIONBIO, &nb);   /* volta a bloqueante; esperas usam poll() */
#else
            fcntl(fd, F_SETFL, fl);
#endif
            break;
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) set_err("TCP: conexao recusada ou expirou");
    return fd;
}

static int opt_str(NGTable *t, const char *k, const char **out) {
    NGValue v;
    if (t && ng_table_get(t, k, &v) && v.type == NG_VAL_STRING) { *out = v.as.string; return 1; }
    return 0;
}

static NGValue ws_conectar(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (argc < 1 || args[0].type != NG_VAL_STRING) { set_err("uso: WS.Conectar(url, opcoes)"); return ng_val_null(); }
    NGTable *opt = (argc >= 2 && args[1].type == NG_VAL_TABLE) ? args[1].as.table : NULL;
    const char *origin = NULL, *extra = NULL, *ca = NULL;
    opt_str(opt, "Origin", &origin); opt_str(opt, "Cabecalhos", &extra); opt_str(opt, "CaArquivo", &ca);
    int verify = 1, timeout = 10000; NGValue v;
    if (opt && ng_table_get(opt, "Verificar", &v) && v.type == NG_VAL_BOOL && !v.as.boolean) verify = 0;
    if (opt && ng_table_get(opt, "Timeout", &v) && v.type == NG_VAL_NUMBER) timeout = (int)v.as.number;

    int tls; char host[256], port[16], path[2048];
    if (parse_url(args[0].as.string, &tls, host, sizeof host, port, sizeof port, path, sizeof path) != 0) { set_err("URL invalida (use ws:// ou wss://)"); return ng_val_null(); }
    int id = 0;
    for (int i = 1; i <= MAX_CONN; i++) if (!conns[i].used) { id = i; break; }
    if (!id) { set_err("limite de conexoes atingido"); return ng_val_null(); }
#ifndef NG_HAS_OPENSSL
    if (tls) { set_err("wss:// requer a NG compilada com OpenSSL (-DNG_HAS_OPENSSL)"); return ng_val_null(); }
#endif
    int fd = tcp_connect(host, port, timeout);
    if (fd < 0) return ng_val_null();
    Conn *c = &conns[id];
    memset(c, 0, sizeof *c); c->used = 1; c->fd = fd;
#ifdef NG_HAS_OPENSSL
    if (tls) {
        static int inited = 0;
        if (!inited) { SSL_library_init(); SSL_load_error_strings(); inited = 1; }
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { set_err("TLS: falha ao criar contexto"); conn_close(c); return ng_val_null(); }
        c->ctx = ctx;
        if (verify) {
            if (ca) SSL_CTX_load_verify_locations(ctx, ca, NULL); else SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        }
        SSL *ssl = SSL_new(ctx);
        c->ssl = ssl;
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (verify) SSL_set1_host(ssl, host);
        if (SSL_connect(ssl) <= 0) {
            set_err(verify ? "TLS: handshake falhou (certificado invalido? use Verificar=false so para teste)" : "TLS: handshake falhou");
            conn_close(c); return ng_val_null();
        }
    }
#endif
    /* handshake HTTP -> WebSocket */
    uint8_t kraw[16];
    if (ng_ws_random(kraw, 16) != 0) { set_err("falha ao obter bytes aleatorios"); conn_close(c); return ng_val_null(); }
    char key[32]; b64(kraw, 16, key);
    char req[4096];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
        path, host, port, key);
    if (origin) rl += snprintf(req + rl, sizeof req - (size_t)rl, "Origin: %s\r\n", origin);
    if (extra) rl += snprintf(req + rl, sizeof req - (size_t)rl, "%s\r\n", extra);
    rl += snprintf(req + rl, sizeof req - (size_t)rl, "\r\n");
    if (rl <= 0 || (size_t)rl >= sizeof req || io_write_all(c, (uint8_t *)req, (size_t)rl) != 0) { set_err("falha ao enviar handshake"); conn_close(c); return ng_val_null(); }

    /* le a resposta ate o fim dos cabecalhos */
    size_t hend = 0;
    for (;;) {
        for (size_t i = 3; i < c->rlen; i++) if (!memcmp(c->rb + i - 3, "\r\n\r\n", 4)) { hend = i + 1; break; }
        if (hend) break;
        int st = fill(c, c->rlen + 1, timeout);
        if (st <= 0 || c->rlen > 16384) { set_err("handshake: sem resposta do servidor"); conn_close(c); return ng_val_null(); }
    }
    char *resp = (char *)malloc(hend + 1); memcpy(resp, c->rb, hend); resp[hend] = 0;
    consume(c, hend);
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        snprintf(last_error, sizeof last_error, "handshake recusado: %.60s", strtok(resp, "\r\n"));
        free(resp); conn_close(c); return ng_val_null();
    }
    /* valida Sec-WebSocket-Accept */
    char acc_in[80]; snprintf(acc_in, sizeof acc_in, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t dg[20]; sha1((uint8_t *)acc_in, strlen(acc_in), dg);
    char expect[40]; b64(dg, 20, expect);
    int ok = 0;
    for (char *line = strtok(resp, "\r\n"); line; line = strtok(NULL, "\r\n"))
        if (strncasecmp(line, "Sec-WebSocket-Accept:", 21) == 0) {
            const char *vv = line + 21; while (*vv == ' ') vv++;
            ok = strcmp(vv, expect) == 0;
        }
    free(resp);
    if (!ok) { set_err("handshake: Sec-WebSocket-Accept invalido"); conn_close(c); return ng_val_null(); }
    c->open = 1;
    last_error[0] = 0;
    return ng_val_number((double)id);
}

static Conn *get_conn(NGValue *args, size_t argc) {
    if (argc < 1 || args[0].type != NG_VAL_NUMBER) return NULL;
    int id = (int)args[0].as.number;
    if (id < 1 || id > MAX_CONN || !conns[id].used) return NULL;
    return &conns[id];
}

static NGValue ws_enviar_texto(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; Conn *c = get_conn(args, argc);
    if (!c || !c->open || argc < 2 || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    return ng_val_bool(send_frame(c, 1, (const uint8_t *)args[1].as.string, strlen(args[1].as.string)) == 0);
}
static NGValue ws_enviar_binario(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; Conn *c = get_conn(args, argc);
    if (!c || !c->open || argc < 2 || args[1].type != NG_VAL_STRING) return ng_val_bool(false);
    const char *h = args[1].as.string; size_t n = strlen(h);
    if (n % 2) return ng_val_bool(false);
    uint8_t *b = (uint8_t *)malloc(n / 2 + 1);
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(h[2*i]), d = hexval(h[2*i+1]);
        if (a < 0 || d < 0) { free(b); return ng_val_bool(false); }
        b[i] = (uint8_t)(a * 16 + d);
    }
    int r = send_frame(c, 2, b, n / 2);
    free(b);
    return ng_val_bool(r == 0);
}
static NGValue ws_receber(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; Conn *c = get_conn(args, argc);
    if (!c) return ng_val_null();
    int timeout = (argc >= 2 && args[1].type == NG_VAL_NUMBER) ? (int)args[1].as.number : 0;
    int op; uint8_t *data = NULL; size_t n = 0;
    int r = recv_message(c, timeout, &op, &data, &n);
    if (r == 0) return ng_val_null();
    NGTable *t = ng_table_new(4);
    if (r < 0 || op == 8) {
        c->open = 0;
        ng_table_set(t, "tipo", ng_val_string("fechado"));
        ng_table_set(t, "dados", ng_val_string(""));
        free(data);
        return ng_val_table(t);
    }
    if (op == 1) {
        ng_table_set(t, "tipo", ng_val_string("texto"));
        ng_table_set(t, "dados", ng_val_string((const char *)data));
    } else {
        static const char *d = "0123456789abcdef";
        char *hx = (char *)malloc(n * 2 + 1);
        for (size_t i = 0; i < n; i++) { hx[2*i] = d[data[i] >> 4]; hx[2*i+1] = d[data[i] & 15]; }
        hx[n * 2] = 0;
        ng_table_set(t, "tipo", ng_val_string("binario"));
        ng_table_set(t, "dados", ng_val_string_take(hx));
    }
    free(data);
    return ng_val_table(t);
}
static NGValue ws_aberto(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; Conn *c = get_conn(args, argc);
    return ng_val_bool(c && c->open);
}
static NGValue ws_fechar(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; Conn *c = get_conn(args, argc);
    if (!c) return ng_val_bool(false);
    if (c->open) { uint8_t code[2] = {0x03, 0xE8}; send_frame(c, 8, code, 2); }
    conn_close(c);
    return ng_val_bool(true);
}
static NGValue ws_erro(NGInterpreter *interp, NGValue *args, size_t argc) {
    (void)interp; (void)args; (void)argc;
    return ng_val_string(last_error);
}

/* ------------------------------- API C ------------------------------- */
int ng_ws_c_connect(const char *url, const char *origin, const char *extra_headers, int timeout_ms) {
    if (!url) return 0;
    NGValue args[2];
    args[0] = ng_val_string(url);
    NGTable *opt = ng_table_new(4);
    if (origin) ng_table_set(opt, "Origin", ng_val_string(origin));
    if (extra_headers) ng_table_set(opt, "Cabecalhos", ng_val_string(extra_headers));
    ng_table_set(opt, "Timeout", ng_val_number(timeout_ms > 0 ? timeout_ms : 10000));
    args[1] = ng_val_table(opt);
    NGValue r = ws_conectar(NULL, args, 2);
    int id = (r.type == NG_VAL_NUMBER) ? (int)r.as.number : 0;
    ng_val_free(&args[0]);
    ng_val_free(&args[1]);
    return id;
}

int ng_ws_c_send_binary(int id, const uint8_t *data, size_t n) {
    if (id < 1 || id > MAX_CONN || !conns[id].used || !conns[id].open) return 0;
    return send_frame(&conns[id], 2, data, n) == 0;
}

int ng_ws_c_recv(int id, int timeout_ms, uint8_t **out, size_t *out_n, int *op) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (id < 1 || id > MAX_CONN || !conns[id].used) return -1;
    uint8_t *data = NULL;
    size_t n = 0;
    int o = 0;
    int r = recv_message(&conns[id], timeout_ms, &o, &data, &n);
    if (r <= 0) {
        if (r < 0) conns[id].open = 0;
        free(data);
        return r;
    }
    if (op) *op = o;
    if (out) *out = data; else free(data);
    if (out_n) *out_n = n;
    return 1;
}

int ng_ws_c_open(int id) {
    return id >= 1 && id <= MAX_CONN && conns[id].used && conns[id].open;
}

void ng_ws_c_close(int id) {
    if (id < 1 || id > MAX_CONN || !conns[id].used) return;
    if (conns[id].open) {
        uint8_t code[2] = {0x03, 0xE8};
        send_frame(&conns[id], 8, code, 2);
    }
    conn_close(&conns[id]);
}

const char *ng_ws_c_error(void) { return last_error; }

static void load_ws(NGInterpreter *interp) {
    NGTable *t = ng_table_new(10);
    ng_table_set(t, "Conectar", ng_val_native_func("Conectar", ws_conectar));
    ng_table_set(t, "EnviarTexto", ng_val_native_func("EnviarTexto", ws_enviar_texto));
    ng_table_set(t, "EnviarBinario", ng_val_native_func("EnviarBinario", ws_enviar_binario));
    ng_table_set(t, "Receber", ng_val_native_func("Receber", ws_receber));
    ng_table_set(t, "Aberto", ng_val_native_func("Aberto", ws_aberto));
    ng_table_set(t, "Fechar", ng_val_native_func("Fechar", ws_fechar));
    ng_table_set(t, "Erro", ng_val_native_func("Erro", ws_erro));
    ng_env_set(interp->globals, "WS", ng_val_table(t));
}

void ng_ws_module_register(void) {
    ng_native_module_register("ws", load_ws);
}
