#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "ng_http.h"
#include "ng_json.h"
#include "ng_native_module.h"
#include "ng_runtime.h"
#include "ng_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#ifdef NG_HAS_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
static bool g_ssl_initialized = false;
static void init_openssl(void) {
    if (!g_ssl_initialized) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        g_ssl_initialized = true;
    }
}
#endif

/* Estruturas Internas do Servidor HTTP */

typedef struct NGHttpRoute {
    char method[16];        /* "GET", "POST", "PUT", "PATCH", "DELETE", "USE" */
    char path[256];         /* Caminho da rota, ex: "/", "/api", "/static" */
    bool is_prefix;         /* true para rotas prefixadas / middleware / arquivos estaticos */
    bool is_static;         /* true para servir arquivos locais */
    char base_dir[512];     /* Diretorio base para arquivos estaticos */
    NGValue handler;        /* Funcao NG de callback (req, res) */
    struct NGHttpRoute* next;
} NGHttpRoute;

typedef struct NGHttpServerInstance {
    int server_fd;
    int port;
    bool is_running;
    NGHttpRoute* routes;
    NGInterpreter* interp;
} NGHttpServerInstance;

#define MAX_SERVERS 32
static NGHttpServerInstance g_servers[MAX_SERVERS];
static size_t g_server_count = 0;

static NGHttpServerInstance* server_instance_new(NGInterpreter* interp) {
    if (g_server_count >= MAX_SERVERS) return NULL;
    NGHttpServerInstance* s = &g_servers[g_server_count++];
    memset(s, 0, sizeof(NGHttpServerInstance));
    s->server_fd = -1;
    s->port = 8080;
    s->is_running = false;
    s->routes = NULL;
    s->interp = interp;
    return s;
}

static NGHttpServerInstance* server_find_by_table(NGTable* tbl) {
    if (!tbl) return NULL;
    NGValue id_val;
    if (ng_table_get(tbl, "_server_id", &id_val) && id_val.type == NG_VAL_NUMBER) {
        size_t id = (size_t)id_val.as.number;
        if (id < g_server_count) {
            return &g_servers[id];
        }
    }
    return NULL;
}

/* Utilitarios MIME e Manipulacao de Arquivos */

static const char* get_mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".xml") == 0) return "application/xml; charset=utf-8";
    return "application/octet-stream";
}

static char* read_file_bytes(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* Parser de URL */

static bool parse_url(const char* url, char* proto, size_t proto_sz,
                      char* host, size_t host_sz, int* port,
                      char* path, size_t path_sz) {
    if (!url) return false;
    proto[0] = '\0';
    host[0] = '\0';
    path[0] = '\0';
    *port = 80;

    const char* p = url;
    const char* proto_end = strstr(p, "://");
    if (proto_end) {
        size_t plen = proto_end - p;
        if (plen >= proto_sz) plen = proto_sz - 1;
        strncpy(proto, p, plen);
        proto[plen] = '\0';
        p = proto_end + 3;
    } else {
        strncpy(proto, "http", proto_sz - 1);
        proto[proto_sz - 1] = '\0';
    }

    if (strcasecmp(proto, "https") == 0) {
        *port = 443;
    }

    /* Extrai host e porta */
    const char* path_start = strchr(p, '/');
    size_t host_len = path_start ? (size_t)(path_start - p) : strlen(p);
    char host_port[512];
    if (host_len >= sizeof(host_port)) host_len = sizeof(host_port) - 1;
    strncpy(host_port, p, host_len);
    host_port[host_len] = '\0';

    char* colon = strchr(host_port, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
    }
    strncpy(host, host_port, host_sz - 1);
    host[host_sz - 1] = '\0';

    if (path_start) {
        strncpy(path, path_start, path_sz - 1);
        path[path_sz - 1] = '\0';
    } else {
        strncpy(path, "/", path_sz - 1);
        path[path_sz - 1] = '\0';
    }

    return true;
}

/* ========================================================================= */
/* CLIENTE HTTP / HTTPS NATIVO C11                                          */
/* ========================================================================= */

typedef struct {
    int status;
    char* body;
    size_t body_len;
    NGTable* headers;
    bool ok;
} ClientResponse;

static ClientResponse execute_http_client(const char* method, const char* url,
                                         const char* req_body, NGTable* custom_headers,
                                         int timeout_sec) {
    ClientResponse resp;
    resp.status = 0;
    resp.body = NULL;
    resp.body_len = 0;
    resp.headers = ng_table_new(16);
    resp.ok = false;

    char proto[16];
    char host[256];
    char path[2048];
    int port = 80;

    if (!parse_url(url, proto, sizeof(proto), host, sizeof(host), &port, path, sizeof(path))) {
        resp.body = strdup("URL invalida");
        return resp;
    }

    bool is_https = (strcasecmp(proto, "https") == 0);

#ifndef NG_HAS_OPENSSL
    if (is_https) {
        resp.body = strdup("Erro: HTTPS requer suporte a OpenSSL compilado no runtime NG.");
        return resp;
    }
#endif

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res0 = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(host, port_str, &hints, &res0);
    if (gai_err != 0 || !res0) {
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "Erro DNS ao resolver host '%s'", host);
        resp.body = strdup(err_buf);
        return resp;
    }

    int sock = socket(res0->ai_family, res0->ai_socktype, res0->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res0);
        resp.body = strdup("Erro ao criar socket TCP nativo");
        return resp;
    }

    /* Timeout de conexao e leitura */
    struct timeval tv;
    tv.tv_sec = timeout_sec > 0 ? timeout_sec : 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(sock, res0->ai_addr, res0->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res0);
        resp.body = strdup("Erro ao conectar ao servidor remoto (Connection refused ou timeout)");
        return resp;
    }
    freeaddrinfo(res0);

#ifdef NG_HAS_OPENSSL
    SSL_CTX* ssl_ctx = NULL;
    SSL* ssl = NULL;
    if (is_https) {
        init_openssl();
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            close(sock);
            resp.body = strdup("Erro ao inicializar contexto TLS OpenSSL");
            return resp;
        }
        ssl = SSL_new(ssl_ctx);
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, host); /* SNI essencial para HTTPS moderno */
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            close(sock);
            resp.body = strdup("Erro no handshake TLS/HTTPS com o servidor");
            return resp;
        }
    }
#endif

    /* Monta a requisicao HTTP/1.1 */
    size_t req_cap = 4096 + (req_body ? strlen(req_body) : 0);
    char* req_buf = (char*)malloc(req_cap);
    size_t req_len = snprintf(req_buf, req_cap,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: NG-Language/1.0 (C11 Native Runtime)\r\n"
        "Connection: close\r\n"
        "Accept: */*\r\n",
        method, path, host);

    /* Headers customizados */
    bool has_content_type = false;
    if (custom_headers) {
        for (size_t i = 0; i < custom_headers->size; i++) {
            NGEntry* e = custom_headers->buckets[i];
            while (e) {
                if (e->key && e->key[0] != '_') {
                    if (strcasecmp(e->key, "content-type") == 0) has_content_type = true;
                    char* val_str = ng_val_to_string(&e->value);
                    req_len += snprintf(req_buf + req_len, req_cap - req_len, "%s: %s\r\n", e->key, val_str);
                    free(val_str);
                }
                e = e->next;
            }
        }
    }

    if (req_body && strlen(req_body) > 0) {
        if (!has_content_type) {
            req_len += snprintf(req_buf + req_len, req_cap - req_len, "Content-Type: application/json; charset=utf-8\r\n");
        }
        req_len += snprintf(req_buf + req_len, req_cap - req_len, "Content-Length: %zu\r\n\r\n%s", strlen(req_body), req_body);
    } else {
        req_len += snprintf(req_buf + req_len, req_cap - req_len, "\r\n");
    }

    /* Envia os bytes da requisicao */
    bool send_ok = true;
#ifdef NG_HAS_OPENSSL
    if (is_https) {
        int w = SSL_write(ssl, req_buf, (int)req_len);
        if (w <= 0) send_ok = false;
    } else
#endif
    {
        ssize_t w = send(sock, req_buf, req_len, 0);
        if (w <= 0) send_ok = false;
    }
    free(req_buf);

    if (!send_ok) {
#ifdef NG_HAS_OPENSSL
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sock);
        resp.body = strdup("Erro ao enviar dados pela conexao TCP/TLS");
        return resp;
    }

    /* Leitura da resposta */
    size_t raw_cap = 8192;
    size_t raw_len = 0;
    char* raw_buf = (char*)malloc(raw_cap);

    while (true) {
        int n = 0;
#ifdef NG_HAS_OPENSSL
        if (is_https) {
            n = SSL_read(ssl, raw_buf + raw_len, (int)(raw_cap - raw_len - 1));
        } else
#endif
        {
            n = recv(sock, raw_buf + raw_len, raw_cap - raw_len - 1, 0);
        }

        if (n <= 0) break;
        raw_len += n;
        if (raw_len + 4096 >= raw_cap) {
            raw_cap *= 2;
            raw_buf = (char*)realloc(raw_buf, raw_cap);
        }
    }
    raw_buf[raw_len] = '\0';

#ifdef NG_HAS_OPENSSL
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
    close(sock);

    if (raw_len == 0) {
        free(raw_buf);
        resp.body = strdup("Resposta vazia do servidor");
        return resp;
    }

    /* Parsing do Status HTTP e Headers */
    char* header_end = strstr(raw_buf, "\r\n\r\n");
    char* body_start = NULL;
    if (header_end) {
        *header_end = '\0';
        body_start = header_end + 4;
    } else {
        header_end = strstr(raw_buf, "\n\n");
        if (header_end) {
            *header_end = '\0';
            body_start = header_end + 2;
        }
    }

    /* Status line: "HTTP/1.1 200 OK" */
    char* line_save = NULL;
    char* line = strtok_r(raw_buf, "\r\n", &line_save);
    if (line) {
        char proto_ver[32];
        int status_num = 0;
        char status_msg[64];
        if (sscanf(line, "%31s %d %63[^\r\n]", proto_ver, &status_num, status_msg) >= 2) {
            resp.status = status_num;
            resp.ok = (status_num >= 200 && status_num < 300);
        }
    }

    /* Headers */
    while ((line = strtok_r(NULL, "\r\n", &line_save)) != NULL) {
        char* colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char* h_key = line;
            char* h_val = colon + 1;
            while (*h_val == ' ') h_val++;
            /* Normaliza chave para minusculo */
            char key_lower[128];
            size_t klen = strlen(h_key);
            if (klen >= sizeof(key_lower)) klen = sizeof(key_lower) - 1;
            for (size_t i = 0; i < klen; i++) key_lower[i] = tolower(h_key[i]);
            key_lower[klen] = '\0';

            ng_table_set(resp.headers, key_lower, ng_val_string(h_val));
        }
    }

    if (body_start) {
        resp.body_len = raw_len - (body_start - raw_buf);
        resp.body = strdup(body_start);
    } else {
        resp.body = strdup("");
    }

    free(raw_buf);
    return resp;
}

/* Metodo auxiliar .JSON() na resposta HTTP do cliente */
static NGValue resp_json_method(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)args; (void)arg_count;
    NGTable* resp_tbl = interp->receiver_table;
    if (!resp_tbl) return ng_val_null();
    NGValue body_val;
    if (ng_table_get(resp_tbl, "Body", &body_val) && body_val.type == NG_VAL_STRING) {
        return ng_json_parse(body_val.as.string);
    }
    return ng_val_null();
}

static NGValue make_ng_response_table(ClientResponse* cr) {
    NGTable* t = ng_table_new(16);
    ng_table_set(t, "Status", ng_val_number(cr->status));
    ng_table_set(t, "Body", ng_val_string(cr->body ? cr->body : ""));
    ng_table_set(t, "Ok", ng_val_bool(cr->ok));
    ng_table_set(t, "Headers", ng_val_table(cr->headers));
    ng_table_set(t, "JSON", ng_val_native_func("JSON", resp_json_method));
    if (cr->body) free(cr->body);
    return ng_val_table(t);
}

/* Funcoes Nativas do Cliente HTTP da NG */

static NGValue native_http_get(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) {
        fprintf(stderr, "[NG-HTTP] Erro: HTTP.Get espera uma URL em formato de texto.\n");
        return ng_val_null();
    }
    const char* url = args[0].as.string;
    NGTable* headers = (arg_count >= 2 && args[1].type == NG_VAL_TABLE) ? args[1].as.table : NULL;
    ClientResponse cr = execute_http_client("GET", url, NULL, headers, 10);
    return make_ng_response_table(&cr);
}

static NGValue native_http_post(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) {
        fprintf(stderr, "[NG-HTTP] Erro: HTTP.Post espera uma URL em formato de texto.\n");
        return ng_val_null();
    }
    const char* url = args[0].as.string;
    char* body_str = NULL;
    if (arg_count >= 2) {
        if (args[1].type == NG_VAL_STRING) {
            body_str = strdup(args[1].as.string);
        } else if (args[1].type == NG_VAL_TABLE) {
            body_str = ng_json_stringify(&args[1]);
        } else {
            body_str = ng_val_to_string(&args[1]);
        }
    }
    NGTable* headers = (arg_count >= 3 && args[2].type == NG_VAL_TABLE) ? args[2].as.table : NULL;
    ClientResponse cr = execute_http_client("POST", url, body_str ? body_str : "", headers, 10);
    if (body_str) free(body_str);
    return make_ng_response_table(&cr);
}

static NGValue native_http_put(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const char* url = args[0].as.string;
    char* body_str = (arg_count >= 2 && args[1].type == NG_VAL_STRING) ? strdup(args[1].as.string) :
                     (arg_count >= 2 && args[1].type == NG_VAL_TABLE) ? ng_json_stringify(&args[1]) : strdup("");
    NGTable* headers = (arg_count >= 3 && args[2].type == NG_VAL_TABLE) ? args[2].as.table : NULL;
    ClientResponse cr = execute_http_client("PUT", url, body_str, headers, 10);
    free(body_str);
    return make_ng_response_table(&cr);
}

static NGValue native_http_patch(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const char* url = args[0].as.string;
    char* body_str = (arg_count >= 2 && args[1].type == NG_VAL_STRING) ? strdup(args[1].as.string) :
                     (arg_count >= 2 && args[1].type == NG_VAL_TABLE) ? ng_json_stringify(&args[1]) : strdup("");
    NGTable* headers = (arg_count >= 3 && args[2].type == NG_VAL_TABLE) ? args[2].as.table : NULL;
    ClientResponse cr = execute_http_client("PATCH", url, body_str, headers, 10);
    free(body_str);
    return make_ng_response_table(&cr);
}

static NGValue native_http_delete(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const char* url = args[0].as.string;
    NGTable* headers = (arg_count >= 2 && args[1].type == NG_VAL_TABLE) ? args[1].as.table : NULL;
    ClientResponse cr = execute_http_client("DELETE", url, NULL, headers, 10);
    return make_ng_response_table(&cr);
}

static NGValue native_http_request(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_null();
    const char* method = args[0].as.string;
    const char* url = args[1].as.string;
    char* body = (arg_count >= 3 && args[2].type == NG_VAL_STRING) ? args[2].as.string : NULL;
    NGTable* headers = (arg_count >= 4 && args[3].type == NG_VAL_TABLE) ? args[3].as.table : NULL;
    ClientResponse cr = execute_http_client(method, url, body, headers, 10);
    return make_ng_response_table(&cr);
}

/* ========================================================================= */
/* SERVIDOR HTTP NATIVO C11 (INSPIRADO NO EXPRESS)                           */
/* ========================================================================= */

/* Metodos do Objeto Response (res) passado para o handler */

static NGValue res_html(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl) return ng_val_null();
    const char* content = (arg_count >= 1 && args[0].type == NG_VAL_STRING) ? args[0].as.string : "";

    ng_table_set(res_tbl, "Body", ng_val_string(content));
    ng_table_set(res_tbl, "StatusCode", ng_val_number(200));

    NGValue hdr_val;
    if (ng_table_get(res_tbl, "Headers", &hdr_val) && hdr_val.type == NG_VAL_TABLE) {
        ng_table_set(hdr_val.as.table, "Content-Type", ng_val_string("text/html; charset=utf-8"));
    }
    return ng_val_null();
}

static NGValue res_json(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl) return ng_val_null();

    char* json_str = NULL;
    if (arg_count >= 1) {
        if (args[0].type == NG_VAL_STRING) {
            json_str = strdup(args[0].as.string);
        } else {
            json_str = ng_json_stringify(&args[0]);
        }
    } else {
        json_str = strdup("{}");
    }

    ng_table_set(res_tbl, "Body", ng_val_string(json_str ? json_str : "{}"));
    ng_table_set(res_tbl, "StatusCode", ng_val_number(200));

    NGValue hdr_val;
    if (ng_table_get(res_tbl, "Headers", &hdr_val) && hdr_val.type == NG_VAL_TABLE) {
        ng_table_set(hdr_val.as.table, "Content-Type", ng_val_string("application/json; charset=utf-8"));
    }
    if (json_str) free(json_str);
    return ng_val_null();
}

static NGValue res_text(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl) return ng_val_null();
    char* content = (arg_count >= 1) ? ng_val_to_string(&args[0]) : strdup("");

    ng_table_set(res_tbl, "Body", ng_val_string(content));
    ng_table_set(res_tbl, "StatusCode", ng_val_number(200));

    NGValue hdr_val;
    if (ng_table_get(res_tbl, "Headers", &hdr_val) && hdr_val.type == NG_VAL_TABLE) {
        ng_table_set(hdr_val.as.table, "Content-Type", ng_val_string("text/plain; charset=utf-8"));
    }
    free(content);
    return ng_val_null();
}

static NGValue res_status(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl) return ng_val_null();
    if (arg_count >= 1 && args[0].type == NG_VAL_NUMBER) {
        ng_table_set(res_tbl, "StatusCode", ng_val_number(args[0].as.number));
    }
    return ng_val_table(res_tbl); /* Permite encadeamento: res.Status(404).HTML("...") */
}

static NGValue res_header(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl || arg_count < 2) return ng_val_null();
    if (args[0].type == NG_VAL_STRING) {
        NGValue hdr_val;
        if (ng_table_get(res_tbl, "Headers", &hdr_val) && hdr_val.type == NG_VAL_TABLE) {
            char* val_str = ng_val_to_string(&args[1]);
            ng_table_set(hdr_val.as.table, args[0].as.string, ng_val_string(val_str));
            free(val_str);
        }
    }
    return ng_val_table(res_tbl);
}

static NGValue res_file(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGTable* res_tbl = interp->receiver_table;
    if (!res_tbl || arg_count < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    const char* path = args[0].as.string;

    size_t sz = 0;
    char* content = read_file_bytes(path, &sz);
    if (!content) {
        ng_table_set(res_tbl, "StatusCode", ng_val_number(404));
        ng_table_set(res_tbl, "Body", ng_val_string("<h1>404 Arquivo Nao Encontrado</h1>"));
        return ng_val_null();
    }

    const char* mime = get_mime_type(path);
    ng_table_set(res_tbl, "Body", ng_val_string(content));
    ng_table_set(res_tbl, "StatusCode", ng_val_number(200));

    NGValue hdr_val;
    if (ng_table_get(res_tbl, "Headers", &hdr_val) && hdr_val.type == NG_VAL_TABLE) {
        ng_table_set(hdr_val.as.table, "Content-Type", ng_val_string(mime));
    }
    free(content);
    return ng_val_null();
}

/* Metodo .JSON() no objeto Request (req) */
static NGValue req_json_method(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)args; (void)arg_count;
    NGTable* req_tbl = interp->receiver_table;
    if (!req_tbl) return ng_val_null();
    NGValue body_val;
    if (ng_table_get(req_tbl, "Body", &body_val) && body_val.type == NG_VAL_STRING) {
        return ng_json_parse(body_val.as.string);
    }
    return ng_val_null();
}

/* Registro de Rotas no Servidor */

static void server_add_route(NGHttpServerInstance* s, const char* method, const char* path,
                             NGValue handler, bool is_prefix, bool is_static, const char* base_dir) {
    NGHttpRoute* r = (NGHttpRoute*)malloc(sizeof(NGHttpRoute));
    strncpy(r->method, method, sizeof(r->method) - 1);
    r->method[sizeof(r->method) - 1] = '\0';
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';
    r->is_prefix = is_prefix;
    r->is_static = is_static;
    if (base_dir) {
        strncpy(r->base_dir, base_dir, sizeof(r->base_dir) - 1);
        r->base_dir[sizeof(r->base_dir) - 1] = '\0';
    } else {
        r->base_dir[0] = '\0';
    }
    r->handler = ng_val_clone(&handler);
    r->next = NULL;

    if (!s->routes) {
        s->routes = r;
    } else {
        NGHttpRoute* curr = s->routes;
        while (curr->next) curr = curr->next;
        curr->next = r;
    }
}

static NGValue server_route_method(NGInterpreter* interp, NGValue* args, size_t arg_count, const char* http_method) {
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (!s) return ng_val_null();
    if (arg_count < 2 || args[0].type != NG_VAL_STRING) return ng_val_null();
    server_add_route(s, http_method, args[0].as.string, args[1], false, false, NULL);
    return ng_val_table(interp->receiver_table);
}

static NGValue native_server_get(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    return server_route_method(interp, args, arg_count, "GET");
}

static NGValue native_server_post(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    return server_route_method(interp, args, arg_count, "POST");
}

static NGValue native_server_put(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    return server_route_method(interp, args, arg_count, "PUT");
}

static NGValue native_server_patch(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    return server_route_method(interp, args, arg_count, "PATCH");
}

static NGValue native_server_delete(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    return server_route_method(interp, args, arg_count, "DELETE");
}

static NGValue native_server_use(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (!s) return ng_val_null();
    if (arg_count == 1) {
        server_add_route(s, "*", "/", args[0], true, false, NULL);
    } else if (arg_count >= 2 && args[0].type == NG_VAL_STRING) {
        server_add_route(s, "*", args[0].as.string, args[1], true, false, NULL);
    }
    return ng_val_table(interp->receiver_table);
}

/* Servidor de Arquivos Estaticos: Servidor.ServirArquivos("/static", "./public") */
static NGValue native_server_static(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (!s || arg_count < 2 || args[0].type != NG_VAL_STRING || args[1].type != NG_VAL_STRING) return ng_val_null();
    server_add_route(s, "GET", args[0].as.string, ng_val_null(), true, true, args[1].as.string);
    return ng_val_table(interp->receiver_table);
}

/* Processamento de Requisicao do Servidor */

static void handle_client_connection(NGHttpServerInstance* s, int client_fd) {
    /* Timeout de leitura */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    size_t req_cap = 8192;
    size_t req_len = 0;
    char* req_buf = (char*)malloc(req_cap);

    while (true) {
        ssize_t n = recv(client_fd, req_buf + req_len, req_cap - req_len - 1, 0);
        if (n <= 0) break;
        req_len += n;
        req_buf[req_len] = '\0';
        if (strstr(req_buf, "\r\n\r\n") || strstr(req_buf, "\n\n")) {
            /* Verifica Content-Length se houver body */
            char* cl_hdr = strcasestr(req_buf, "Content-Length:");
            if (cl_hdr) {
                size_t expected = atoi(cl_hdr + 15);
                char* b_start = strstr(req_buf, "\r\n\r\n");
                size_t current_body_len = b_start ? (req_len - (b_start + 4 - req_buf)) : 0;
                if (current_body_len < expected && req_len + 4096 < 1048576) {
                    if (req_len + 4096 >= req_cap) {
                        req_cap *= 2;
                        req_buf = (char*)realloc(req_buf, req_cap);
                    }
                    continue;
                }
            }
            break;
        }
        if (req_len + 2048 >= req_cap) {
            req_cap *= 2;
            req_buf = (char*)realloc(req_buf, req_cap);
        }
    }

    if (req_len == 0) {
        free(req_buf);
        close(client_fd);
        return;
    }

    /* Extrai Metodo, URL e Protocolo */
    char method[16] = {0};
    char full_url[2048] = {0};
    char proto[16] = {0};

    char* line_end = strstr(req_buf, "\r\n");
    if (!line_end) line_end = strstr(req_buf, "\n");
    if (line_end) {
        *line_end = '\0';
        sscanf(req_buf, "%15s %2047s %15s", method, full_url, proto);
        *line_end = '\r';
    }

    /* Separa Path e Query string */
    char path[1024];
    char query[1024];
    path[0] = '\0';
    query[0] = '\0';

    char* qmark = strchr(full_url, '?');
    if (qmark) {
        size_t plen = qmark - full_url;
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        strncpy(path, full_url, plen);
        path[plen] = '\0';
        strncpy(query, qmark + 1, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
    } else {
        strncpy(path, full_url, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    /* Parsing de Headers e Body */
    NGTable* headers_tbl = ng_table_new(16);
    NGTable* params_tbl = ng_table_new(16);

    /* Parse query string em params_tbl */
    if (query[0] != '\0') {
        char qcopy[1024];
        strncpy(qcopy, query, sizeof(qcopy) - 1);
        qcopy[sizeof(qcopy) - 1] = '\0';
        char* qsave = NULL;
        char* pair = strtok_r(qcopy, "&", &qsave);
        while (pair) {
            char* eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                ng_table_set(params_tbl, pair, ng_val_string(eq + 1));
            } else {
                ng_table_set(params_tbl, pair, ng_val_string(""));
            }
            pair = strtok_r(NULL, "&", &qsave);
        }
    }

    char* body_start = strstr(req_buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
    } else {
        body_start = strstr(req_buf, "\n\n");
        if (body_start) body_start += 2;
    }

    /* Cria os objetos Request (req) e Response (res) para a NG */
    NGTable* req_tbl = ng_table_new(16);
    ng_table_set(req_tbl, "Method", ng_val_string(method));
    ng_table_set(req_tbl, "Path", ng_val_string(path));
    ng_table_set(req_tbl, "Query", ng_val_string(query));
    ng_table_set(req_tbl, "Body", ng_val_string(body_start ? body_start : ""));
    ng_table_set(req_tbl, "Headers", ng_val_table(headers_tbl));
    ng_table_set(req_tbl, "Params", ng_val_table(params_tbl));
    ng_table_set(req_tbl, "JSON", ng_val_native_func("JSON", req_json_method));

    NGTable* res_tbl = ng_table_new(16);
    NGTable* res_headers = ng_table_new(8);
    ng_table_set(res_headers, "Content-Type", ng_val_string("text/html; charset=utf-8"));
    ng_table_set(res_headers, "Server", ng_val_string("NG-HTTP/1.0"));

    ng_table_set(res_tbl, "StatusCode", ng_val_number(200));
    ng_table_set(res_tbl, "Body", ng_val_string(""));
    ng_table_set(res_tbl, "Headers", ng_val_table(res_headers));
    ng_table_set(res_tbl, "HTML", ng_val_native_func("HTML", res_html));
    ng_table_set(res_tbl, "JSON", ng_val_native_func("JSON", res_json));
    ng_table_set(res_tbl, "Texto", ng_val_native_func("Texto", res_text));
    ng_table_set(res_tbl, "Text", ng_val_native_func("Text", res_text));
    ng_table_set(res_tbl, "Status", ng_val_native_func("Status", res_status));
    ng_table_set(res_tbl, "Header", ng_val_native_func("Header", res_header));
    ng_table_set(res_tbl, "Arquivo", ng_val_native_func("Arquivo", res_file));
    ng_table_set(res_tbl, "File", ng_val_native_func("File", res_file));

    /* Casamento de Rotas */
    bool route_matched = false;
    NGHttpRoute* r = s->routes;

    while (r) {
        bool method_match = (strcmp(r->method, "*") == 0 || strcasecmp(r->method, method) == 0);
        bool path_match = false;

        if (r->is_prefix) {
            size_t plen = strlen(r->path);
            if (strncmp(r->path, path, plen) == 0) {
                path_match = true;
            }
        } else {
            if (strcmp(r->path, path) == 0) {
                path_match = true;
            }
        }

        if (method_match && path_match) {
            route_matched = true;
            if (r->is_static) {
                /* Servico de arquivos estaticos */
                const char* subpath = path + strlen(r->path);
                while (*subpath == '/') subpath++;
                /* Seguranca: bloqueia traversal .. */
                if (strstr(subpath, "..")) {
                    ng_table_set(res_tbl, "StatusCode", ng_val_number(403));
                    ng_table_set(res_tbl, "Body", ng_val_string("<h1>403 Proibido</h1>"));
                } else {
                    char full_file_path[1024];
                    snprintf(full_file_path, sizeof(full_file_path), "%s/%s", r->base_dir, subpath);
                    size_t fsz = 0;
                    char* fcontent = read_file_bytes(full_file_path, &fsz);
                    if (fcontent) {
                        ng_table_set(res_tbl, "StatusCode", ng_val_number(200));
                        ng_table_set(res_tbl, "Body", ng_val_string(fcontent));
                        ng_table_set(res_headers, "Content-Type", ng_val_string(get_mime_type(full_file_path)));
                        free(fcontent);
                    } else {
                        ng_table_set(res_tbl, "StatusCode", ng_val_number(404));
                        ng_table_set(res_tbl, "Body", ng_val_string("<h1>404 Arquivo Nao Encontrado</h1>"));
                    }
                }
            } else {
                /* Invoca o handler da rota (req, res) */
                NGValue args[2];
                args[0] = ng_val_table(req_tbl);
                args[1] = ng_val_table(res_tbl);
                s->interp->receiver_table = res_tbl;
                ng_interp_call_func(s->interp, &r->handler, args, 2);
            }
            break;
        }
        r = r->next;
    }

    if (!route_matched) {
        ng_table_set(res_tbl, "StatusCode", ng_val_number(404));
        ng_table_set(res_tbl, "Body", ng_val_string("<h1>404 Nao Encontrado - NG Language Server</h1>"));
    }

    /* Monta e Envia a Resposta HTTP */
    NGValue st_val;
    int resp_status = 200;
    if (ng_table_get(res_tbl, "StatusCode", &st_val) && st_val.type == NG_VAL_NUMBER) {
        resp_status = (int)st_val.as.number;
    }

    const char* status_text = "OK";
    switch (resp_status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 204: status_text = "No Content"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "OK"; break;
    }

    NGValue body_val;
    const char* resp_body = "";
    if (ng_table_get(res_tbl, "Body", &body_val) && body_val.type == NG_VAL_STRING) {
        resp_body = body_val.as.string;
    }

    size_t body_len = strlen(resp_body);
    char out_buf[4096];
    size_t out_len = snprintf(out_buf, sizeof(out_buf),
        "HTTP/1.1 %d %s\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n",
        resp_status, status_text, body_len);

    /* Adiciona headers da resposta */
    for (size_t i = 0; i < res_headers->size; i++) {
        NGEntry* e = res_headers->buckets[i];
        while (e) {
            if (e->key && e->key[0] != '_') {
                char* vstr = ng_val_to_string(&e->value);
                out_len += snprintf(out_buf + out_len, sizeof(out_buf) - out_len, "%s: %s\r\n", e->key, vstr);
                free(vstr);
            }
            e = e->next;
        }
    }
    out_len += snprintf(out_buf + out_len, sizeof(out_buf) - out_len, "\r\n");

    /* Envia cabecalhos e corpo */
    send(client_fd, out_buf, out_len, 0);
    if (body_len > 0) {
        send(client_fd, resp_body, body_len, 0);
    }

    free(req_buf);
    close(client_fd);
}

/* Servidor.Listen(port, [max_requests]) */
static NGValue native_server_listen(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (!s) return ng_val_null();

    int port = 8080;
    if (arg_count >= 1 && args[0].type == NG_VAL_NUMBER) {
        port = (int)args[0].as.number;
    }
    s->port = port;

    /* Opcional: maximo de requisicoes (otimo para testes automatizados) */
    int max_requests = -1;
    if (arg_count >= 2 && args[1].type == NG_VAL_NUMBER) {
        max_requests = (int)args[1].as.number;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[NG-HTTP] Erro ao criar socket do servidor: %s\n", strerror(errno));
        return ng_val_null();
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[NG-HTTP] Erro ao vincular porta %d: %s\n", port, strerror(errno));
        close(fd);
        return ng_val_null();
    }

    if (listen(fd, 64) < 0) {
        fprintf(stderr, "[NG-HTTP] Erro ao escutar na porta %d: %s\n", port, strerror(errno));
        close(fd);
        return ng_val_null();
    }

    s->server_fd = fd;
    s->is_running = true;

    printf("[NG-HTTP] Servidor iniciado com sucesso em http://localhost:%d\n", port);
    fflush(stdout);

    int processed = 0;
    while (s->is_running) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int poll_res = poll(&pfd, 1, 1000); /* 1 segundo timeout para verificar is_running */
        if (poll_res < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_res == 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            handle_client_connection(s, client_fd);
            processed++;
            if (max_requests > 0 && processed >= max_requests) {
                break;
            }
        }
    }

    close(fd);
    s->server_fd = -1;
    s->is_running = false;
    return ng_val_null();
}

/* Servidor.Processar([timeout_ms]) - Processa 1 requisicao com timeout (otimo para loop de eventos) */
static NGValue native_server_processar(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (!s || s->server_fd < 0) return ng_val_bool(false);
    int timeout_ms = (arg_count >= 1 && args[0].type == NG_VAL_NUMBER) ? (int)args[0].as.number : 1000;

    struct pollfd pfd;
    pfd.fd = s->server_fd;
    pfd.events = POLLIN;
    int res = poll(&pfd, 1, timeout_ms);
    if (res > 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(s->server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd >= 0) {
            handle_client_connection(s, client_fd);
            return ng_val_bool(true);
        }
    }
    return ng_val_bool(false);
}

/* Servidor.Parar() */
static NGValue native_server_parar(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)args; (void)arg_count;
    NGHttpServerInstance* s = server_find_by_table(interp->receiver_table);
    if (s) {
        s->is_running = false;
    }
    return ng_val_null();
}

/* Construtor de Servidor: HTTP() ou HTTP.Novo() */
static NGValue native_http_novo(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)args; (void)arg_count;
    NGHttpServerInstance* s = server_instance_new(interp);
    if (!s) return ng_val_null();

    size_t id = g_server_count - 1;
    NGTable* server_tbl = ng_table_new(16);
    ng_table_set(server_tbl, "_server_id", ng_val_number((double)id));
    ng_table_set(server_tbl, "Get", ng_val_native_func("Get", native_server_get));
    ng_table_set(server_tbl, "Post", ng_val_native_func("Post", native_server_post));
    ng_table_set(server_tbl, "Put", ng_val_native_func("Put", native_server_put));
    ng_table_set(server_tbl, "Patch", ng_val_native_func("Patch", native_server_patch));
    ng_table_set(server_tbl, "Delete", ng_val_native_func("Delete", native_server_delete));
    ng_table_set(server_tbl, "Use", ng_val_native_func("Use", native_server_use));
    ng_table_set(server_tbl, "ServirArquivos", ng_val_native_func("ServirArquivos", native_server_static));
    ng_table_set(server_tbl, "Static", ng_val_native_func("Static", native_server_static));
    ng_table_set(server_tbl, "Listen", ng_val_native_func("Listen", native_server_listen));
    ng_table_set(server_tbl, "Processar", ng_val_native_func("Processar", native_server_processar));
    ng_table_set(server_tbl, "Parar", ng_val_native_func("Parar", native_server_parar));
    ng_table_set(server_tbl, "Close", ng_val_native_func("Close", native_server_parar));

    return ng_val_table(server_tbl);
}

/* Modulo Nativo @http */

void ng_http_module_load(NGInterpreter* interp) {
    if (!interp || !interp->globals) return;

    /* Garante que o modulo JSON tambem seja carregado */
    ng_json_module_load(interp);

    NGTable* http_tbl = ng_table_new(16);
    /* Permite instanciar servidor chamando HTTP() diretamente */
    ng_table_set(http_tbl, "__call", ng_val_native_func("__call", native_http_novo));
    ng_table_set(http_tbl, "novo", ng_val_native_func("novo", native_http_novo));
    ng_table_set(http_tbl, "Novo", ng_val_native_func("Novo", native_http_novo));
    ng_table_set(http_tbl, "Servidor", ng_val_native_func("Servidor", native_http_novo));

    /* Cliente HTTP / HTTPS */
    ng_table_set(http_tbl, "Get", ng_val_native_func("Get", native_http_get));
    ng_table_set(http_tbl, "Post", ng_val_native_func("Post", native_http_post));
    ng_table_set(http_tbl, "Put", ng_val_native_func("Put", native_http_put));
    ng_table_set(http_tbl, "Patch", ng_val_native_func("Patch", native_http_patch));
    ng_table_set(http_tbl, "Delete", ng_val_native_func("Delete", native_http_delete));
    ng_table_set(http_tbl, "Requisicao", ng_val_native_func("Requisicao", native_http_request));

    ng_env_set(interp->globals, "HTTP", ng_val_table(http_tbl));
}
