#ifndef NG_LUA_OPTIMIZER_H
#define NG_LUA_OPTIMIZER_H

#include "ng_ast.h"
#include <stdint.h>

/* Forward decl do estado Lua (lua.h e incluído quando o build detecta Lua). */
typedef struct lua_State lua_State;

typedef struct NGLuaStats {
    uint64_t folds;
    uint64_t dce;
    uint64_t alg;
    uint64_t functions;
    uint64_t globals;
    uint64_t loops;
    uint64_t calls;
} NGLuaStats;

/* Roda o otimizador semântico de AST no estado Lua embedded (Core 1).
  Marshaling AST <-> Lua tables; o script "optimize_ast" faz constant folding,
   simplificação algébrica, dead-code elimination e contagem de símbolos.
   Devolve a AST otimizada em *out (nova árvore, a original não é modificada)
   e estatísticas em stats. Retorna false se Lua não estiver disponível ou
   falhar (caller faz fallback seguro). */
bool ng_lua_optimize_ast(lua_State* L, NGAstNode* ast, NGLuaStats* stats,
                         NGAstNode** out);

/* O script do otimizador Lua embutido (para depuração/testes). */
const char* ng_lua_optimizer_script(void);

#endif /* NG_LUA_OPTIMIZER_H */
