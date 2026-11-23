#ifndef NG_TRANSPILER_LUA_H
#define NG_TRANSPILER_LUA_H

#include <stddef.h>

/* Traduz codigo fonte NG para codigo Lua em memoria.
 * Retorna buffer alocado com malloc (deve ser liberado com free),
 * ou NULL em caso de erro de alocacao. */
char *ng_transpile_to_lua(const char *ng_source, size_t *out_len);

#endif
