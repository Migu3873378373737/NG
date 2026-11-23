#ifndef NG_NET_LIBS_H
#define NG_NET_LIBS_H

#include "ng_interpreter.h"

/* Inicializa os módulos de rede: http, https, dns, proxy, reverse_proxy, websocket, mylanguage, n3cache */
void ng_net_modules_register(void);

/* Backend de linguagem (Lua/Python/JavaScript/Go): transpila codigo NG -> `lang`
   e executa no runtime externo. Com use_cache=true, usa cache de sessao em
   memoria (evita recompilacao repetida do mesmo fonte no mesmo processo). */
char* ng_net_libs_transpile(const char* source, const char* lang);
NGValue ng_net_libs_exec_source(const char* source, const char* lang, bool use_cache);

#endif /* NG_NET_LIBS_H */
