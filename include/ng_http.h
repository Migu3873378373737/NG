#ifndef NG_HTTP_H
#define NG_HTTP_H

#include "ng_value.h"
#include "ng_interpreter.h"

/* Carrega o modulo nativo HTTP no interpretador NG (@http e objeto HTTP) */
void ng_http_module_load(NGInterpreter* interp);

/* Inicializacao global dos modulos nativos da NG */
void ng_native_modules_init(void);

#endif /* NG_HTTP_H */
