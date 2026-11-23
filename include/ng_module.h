#ifndef NG_MODULE_H
#define NG_MODULE_H

#include <stdbool.h>
#include "ng_interpreter.h"

bool ng_module_load_into_env(const char* module_name, NGInterpreter* interp);

#endif
