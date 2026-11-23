#ifndef NG_NATIVE_MODULE_H
#define NG_NATIVE_MODULE_H

#include "ng_interpreter.h"
#include <stdbool.h>

typedef void (*NGNativeModuleLoader)(NGInterpreter* interp);

void ng_native_module_register(const char* name, NGNativeModuleLoader loader);
bool ng_native_module_load(const char* name, NGInterpreter* interp);

/* Fronteira script <-> modulos nativos: invoca funcao nativa de um modulo com
   args em buffers NGValue (uso do chamador; sem alocacao por invocacao). */
NGValue ng_native_module_invoke(NGInterpreter* interp, const char* module_name, const char* func_name, NGValue* args, size_t argc);

#endif /* NG_NATIVE_MODULE_H */
