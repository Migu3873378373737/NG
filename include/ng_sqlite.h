#ifndef NG_SQLITE_H
#define NG_SQLITE_H

#include "ng_interpreter.h"

/* Carrega o modulo nativo @sqlite (tabela global SQLite + funcao list()). */
void ng_sqlite_module_load(NGInterpreter* interp);

#endif /* NG_SQLITE_H */
