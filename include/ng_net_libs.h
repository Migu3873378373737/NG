#ifndef NG_NET_LIBS_H
#define NG_NET_LIBS_H

#include "ng_interpreter.h"

/* Inicializa os módulos de rede: http, https, dns, proxy, reverse_proxy, websocket, mylanguage, n3cache */
void ng_net_modules_register(void);

#endif /* NG_NET_LIBS_H */
