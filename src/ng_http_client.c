/* ng_http_client.c - HTTP client usando libcurl real */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_http_client.h"
#include "ng_runtime.h"
#include "ng_native_module.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Buffer de resposta                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} WriteBuffer;

static void wb_init(WriteBuffer* wb) {
    wb->data = (char*)malloc(4096);
    wb->len  = 0;
    wb->cap  = wb->data ? 4096 : 0;
    if (wb->data) wb->data[0] = '\0';
}

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    WriteBuffer* wb = (WriteBuffer*)userdata;
    size_t incoming = size * nmemb;
    if (incoming == 0) return 0;

    if (wb->len + incoming + 1 > wb->cap) {
        size_t new_cap = wb->cap * 2 + incoming + 1;
        char* new_data = (char*)realloc(wb->data, new_cap);
        if (!new_data) return 0; /* signal error to curl */
        wb->data = new_data;
        wb->cap  = new_cap;
    }
    memcpy(wb->data + wb->len, ptr, incoming);
    wb->len += incoming;
    wb->data[wb->len] = '\0';
    return incoming;
}

/* ------------------------------------------------------------------ */
/* Resposta                                                             */
/* ------------------------------------------------------------------ */
void ng_http_response_free(NGHttpResponse* r) {
    if (!r) return;
    free(r->body);
    free(r->content_type);
    free(r);
}

/* ------------------------------------------------------------------ */
/* Implementacao generica                                               */
/* ------------------------------------------------------------------ */
NGHttpResponse* ng_http_request(const char* method, const char* url,
                                 const char** header_keys, const char** header_vals,
                                 size_t header_count,
                                 const char* body, size_t body_len,
                                 long timeout_ms) {
    if (!method || !url) return NULL;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        return NULL;
    }

    NGHttpResponse* resp = (NGHttpResponse*)calloc(1, sizeof(NGHttpResponse));
    if (!resp) {
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return NULL;
    }

    WriteBuffer wb;
    wb_init(&wb);

    /* URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* SSL validation - obrigatorio */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Timeout */
    if (timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L); /* 30s default */
    }

    /* Follow redirects */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    /* User-Agent */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NG-Language/1.0 libcurl");

    /* Write callback */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wb);

    /* Method */
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        }
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else {
        /* GET is default */
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* Custom headers */
    struct curl_slist* headers = NULL;
    for (size_t i = 0; i < header_count; i++) {
        if (header_keys[i] && header_vals[i]) {
            char hbuf[1024];
            snprintf(hbuf, sizeof(hbuf), "%s: %s", header_keys[i], header_vals[i]);
            headers = curl_slist_append(headers, hbuf);
        }
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    /* Execute */
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp->status_code = (int)http_code;

        char* ct = NULL;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        resp->content_type = ct ? strdup(ct) : strdup("application/octet-stream");

        resp->body     = wb.data;
        resp->body_len = wb.len;
        wb.data = NULL; /* ownership transferred */
    } else {
        resp->status_code = -1;
        resp->body        = strdup(curl_easy_strerror(res));
        resp->body_len    = strlen(resp->body);
        resp->content_type = strdup("text/plain");
        free(wb.data);
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return resp;
}

NGHttpResponse* ng_http_get(const char* url, long timeout_ms) {
    return ng_http_request("GET", url, NULL, NULL, 0, NULL, 0, timeout_ms);
}

NGHttpResponse* ng_http_post(const char* url, const char* body,
                              const char* content_type, long timeout_ms) {
    const char* hk[] = { "Content-Type" };
    const char* hv[] = { content_type ? content_type : "application/x-www-form-urlencoded" };
    size_t body_len = body ? strlen(body) : 0;
    return ng_http_request("POST", url, hk, hv, 1, body, body_len, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Funcoes nativas expostas ao runtime NG                              */
/* ------------------------------------------------------------------ */

/* HTTP.Get(url) -> string com body ou "<erro: ...>" */
static NGValue native_http_get(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) {
        return ng_val_string("<erro: HTTP.Get requer argumento URL (string)>");
    }
    const char* url = args[0].as.string;
    NGHttpResponse* resp = ng_http_get(url, 30000L);
    if (!resp) {
        return ng_val_string("<erro: falha ao criar requisicao curl>");
    }

    /* Monta tabela de resposta */
    NGTable* tbl = ng_table_new(8);
    ng_table_set(tbl, "status", ng_val_number((double)resp->status_code));
    ng_table_set(tbl, "corpo",  ng_val_string(resp->body ? resp->body : ""));
    ng_table_set(tbl, "tipo",   ng_val_string(resp->content_type ? resp->content_type : ""));

    ng_http_response_free(resp);

    NGValue v;
    v.type     = NG_VAL_TABLE;
    v.as.table = tbl;
    return v;
}

/* HTTP.Post(url, corpo) -> tabela de resposta */
static NGValue native_http_post(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING) {
        return ng_val_string("<erro: HTTP.Post requer URL e corpo (string)>");
    }
    const char* url  = args[0].as.string;
    const char* body = (args[1].type == NG_VAL_STRING) ? args[1].as.string : "";
    const char* ct   = (argc >= 3 && args[2].type == NG_VAL_STRING)
                       ? args[2].as.string : "application/json";

    NGHttpResponse* resp = ng_http_post(url, body, ct, 30000L);
    if (!resp) {
        return ng_val_string("<erro: falha ao criar requisicao curl>");
    }

    NGTable* tbl = ng_table_new(8);
    ng_table_set(tbl, "status", ng_val_number((double)resp->status_code));
    ng_table_set(tbl, "corpo",  ng_val_string(resp->body ? resp->body : ""));
    ng_table_set(tbl, "tipo",   ng_val_string(resp->content_type ? resp->content_type : ""));

    ng_http_response_free(resp);

    NGValue v;
    v.type     = NG_VAL_TABLE;
    v.as.table = tbl;
    return v;
}

/* HTTP.StatusTexto(codigo) -> string descritiva */
static NGValue native_http_status_text(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_NUMBER) {
        return ng_val_string("Desconhecido");
    }
    int code = (int)args[0].as.number;
    switch (code) {
        case 200: return ng_val_string("OK");
        case 201: return ng_val_string("Criado");
        case 204: return ng_val_string("Sem Conteudo");
        case 301: return ng_val_string("Movido Permanentemente");
        case 302: return ng_val_string("Encontrado");
        case 400: return ng_val_string("Requisicao Invalida");
        case 401: return ng_val_string("Nao Autorizado");
        case 403: return ng_val_string("Proibido");
        case 404: return ng_val_string("Nao Encontrado");
        case 500: return ng_val_string("Erro Interno do Servidor");
        case 503: return ng_val_string("Servico Indisponivel");
        default: {
            char buf[32];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            return ng_val_string(buf);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Loader do modulo @http (cliente apenas)                             */
/* ------------------------------------------------------------------ */
static void http_client_module_loader(NGInterpreter* interp) {
    /* Cria tabela HTTP */
    NGTable* http_tbl = ng_table_new(16);

    ng_table_set(http_tbl, "Get",         ng_val_native_func("HTTP.Get",         native_http_get));
    ng_table_set(http_tbl, "Post",        ng_val_native_func("HTTP.Post",        native_http_post));
    ng_table_set(http_tbl, "StatusTexto", ng_val_native_func("HTTP.StatusTexto", native_http_status_text));

    NGValue http_val;
    http_val.type     = NG_VAL_TABLE;
    http_val.as.table = http_tbl;

    ng_env_set(interp->globals, "HTTP", http_val);
}

void ng_http_module_register(NGInterpreter* interp) {
    (void)interp;
    ng_native_module_register("http", http_client_module_loader);
}
