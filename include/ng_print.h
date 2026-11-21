#ifndef NG_PRINT_H
#define NG_PRINT_H

#include "ng_interpreter.h"

/* Registra @format, @print, @info, @diag e @help. */
void ng_print_modules_register(void);
/* Carrega as tabelas globais Format, Info, Diag e Ajuda. */
void ng_print_modules_load(NGInterpreter* interp);

#endif /* NG_PRINT_H */
