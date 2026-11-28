#ifndef NG_WS_H
#define NG_WS_H
#include <stddef.h>
#include <stdint.h>

/* Registra o modulo nativo @ws (cliente WebSocket). */
void ng_ws_module_register(void);

/* API C usada por outros modulos nativos (ex.: @wacore).
 * Conecta, envia/recebe bytes binarios e fecha. Os identificadores sao os
 * mesmos de WS.Conectar, portanto as duas APIs podem coexistir. */
int  ng_ws_c_connect(const char *url, const char *origin, const char *extra_headers, int timeout_ms);
int  ng_ws_c_send_binary(int id, const uint8_t *data, size_t n);
/* 1 = mensagem recebida, 0 = timeout, -1 = fechado/erro. *op: 1=texto, 2=binario, 8=close. */
int  ng_ws_c_recv(int id, int timeout_ms, uint8_t **out, size_t *out_n, int *op);
int  ng_ws_c_open(int id);
void ng_ws_c_close(int id);
const char *ng_ws_c_error(void);
#endif
