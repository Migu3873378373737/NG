#ifndef NG_INTEROP_H
#define NG_INTEROP_H

#include "ng_interpreter.h"

/* Registra o módulo nativo @interop (tabela global Interop). */
void ng_interop_module_register(void);
void ng_interop_module_load(NGInterpreter* interp);

#endif /* NG_INTEROP_H */
