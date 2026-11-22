#ifndef NG_INTERPRETER_H
#define NG_INTERPRETER_H

#include "ng_ast.h"
#include "ng_runtime.h"

typedef struct NGInterpreter {
    NGEnv* globals;
    NGEnv* current_env;
    NGValue reg_y; // Registrador semântico especial 'Y'
    const char* current_file;
    bool is_subinterpreter;
    struct NGTable* receiver_table; // Tabela receptora do metodo atual
    bool diag_enabled; // Cache do flag NG_DIAG (evita getenv por operacao em loops)
} NGInterpreter;

void ng_interp_init(NGInterpreter* interp, const char* filename, bool is_subinterpreter);
void ng_interp_cleanup(NGInterpreter* interp);
bool ng_interp_run(NGInterpreter* interp, NGAstNode* program);
NGValue ng_interp_call_func(NGInterpreter* interp, const NGValue* func, NGValue* args, size_t arg_count);

#endif // NG_INTERPRETER_H
