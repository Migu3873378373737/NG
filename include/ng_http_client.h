#ifndef NG_HTTP_CLIENT_H
#define NG_HTTP_CLIENT_H

#include "ng_value.h"
#include "ng_interpreter.h"
#include <stddef.h>

typedef struct {
    int status_code;
    char* body;
    size_t body_len;
    char* content_type;
} NGHttpResponse;

void ng_http_response_free(NGHttpResponse* r);

/* Realiza requisicao HTTP GET real */
NGHttpResponse* ng_http_get(const char* url, long timeout_ms);

/* Realiza requisicao HTTP POST real */
NGHttpResponse* ng_http_post(const char* url, const char* body,
                              const char* content_type, long timeout_ms);

/* Realiza requisicao HTTP generica */
NGHttpResponse* ng_http_request(const char* method, const char* url,
                                 const char** header_keys, const char** header_vals,
                                 size_t header_count,
                                 const char* body, size_t body_len, long timeout_ms);

/* Registra o modulo @http no interpreter */
void ng_http_module_register(NGInterpreter* interp);

#endif /* NG_HTTP_CLIENT_H */
