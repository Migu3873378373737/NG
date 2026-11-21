#ifndef NG_NATIVE_MODULE_H
#define NG_NATIVE_MODULE_H

#include "ng_interpreter.h"
#include <stdbool.h>

typedef void (*NGNativeModuleLoader)(NGInterpreter* interp);

void ng_native_module_register(const char* name, NGNativeModuleLoader loader);
bool ng_native_module_load(const char* name, NGInterpreter* interp);

#endif /* NG_NATIVE_MODULE_H */
