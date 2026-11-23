#ifndef NG_OUTBUF_H
#define NG_OUTBUF_H

#include <stdbool.h>
#include <stddef.h>

/* Buffer de saída em RAM (Dynamic V "RAMP"): stdout de execução acumulado em
   memória e escrito uma única vez no fim, com flush seguro em erro/encerramento.
   Desativável via NG_OUTBUF_DISABLE=1 (comportamento antigo: imprimir direto). */

void ng_outbuf_write(const char* data, size_t len);
void ng_outbuf_printf(const char* fmt, ...);
void ng_outbuf_flush(void);        /* escreve tudo no stdout e libera o buffer */
bool ng_outbuf_enabled(void);
size_t ng_outbuf_len(void);        /* bytes acumulados no buffer atual */

#endif /* NG_OUTBUF_H */
