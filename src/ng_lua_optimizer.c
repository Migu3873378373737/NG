#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_lua_optimizer.h"
#include "ng_lexer.h"
#include "ng_value.h"
#include <stdlib.h>
#include <string.h>

#ifdef NG_HAS_EMBEDDED_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* ------------------------------------------------------------
 * Marshaling AST <-> tabelas Lua (Core 1 worker)
 * ------------------------------------------------------------ */

static int ast_push(lua_State* L, const NGAstNode* n);
static NGAstNode* ast_pop(lua_State* L, int idx);
static NGAstNode* ast_pop_field(lua_State* L, int idx, const char* key);

static void set_str(lua_State* L, int t, const char* key, const char* val) {
    lua_pushstring(L, val ? val : "");
    lua_setfield(L, t, key);
}

static void set_arr(lua_State* L, int t, const char* key, size_t n,
                    NGAstNode** arr) {
    lua_createtable(L, (int)n, 0);
    int a = lua_gettop(L);
    for (size_t i = 0; i < n; i++) {
        ast_push(L, arr[i]);
        lua_rawseti(L, a, (int)i + 1);
    }
    lua_setfield(L, t, key);
}

static void set_str_arr(lua_State* L, int t, const char* key, size_t n,
                        char** arr) {
    lua_createtable(L, (int)n, 0);
    int a = lua_gettop(L);
    for (size_t i = 0; i < n; i++) {
        lua_pushstring(L, arr[i] ? arr[i] : "");
        lua_rawseti(L, a, (int)i + 1);
    }
    lua_setfield(L, t, key);
}

static int ast_push(lua_State* L, const NGAstNode* n) {
    if (!n) { lua_pushnil(L); return -1; }
    lua_createtable(L, 0, 10);
    int t = lua_gettop(L);
    lua_pushinteger(L, (lua_Integer)n->type);
    lua_setfield(L, t, "t");
    lua_pushinteger(L, (lua_Integer)n->line);
    lua_setfield(L, t, "l");
    lua_pushinteger(L, (lua_Integer)n->column);
    lua_setfield(L, t, "c");
    switch (n->type) {
        case NG_AST_PROGRAM:
            set_arr(L, t, "kids", n->as.program.count, n->as.program.statements);
            break;
        case NG_AST_STMT_PRINT:
            ast_push(L, n->as.print_stmt.expr); lua_setfield(L, t, "e");
            break;
        case NG_AST_STMT_G:
            ast_push(L, n->as.g_stmt.expr); lua_setfield(L, t, "e");
            break;
        case NG_AST_STMT_Y:
            ast_push(L, n->as.y_stmt.expr); lua_setfield(L, t, "e");
            break;
        case NG_AST_STMT_ASSIGN:
            set_str(L, t, "name", n->as.assign_stmt.name);
            ast_push(L, n->as.assign_stmt.value); lua_setfield(L, t, "v");
            break;
        case NG_AST_STMT_IF:
            ast_push(L, n->as.if_stmt.condition); lua_setfield(L, t, "c");
            ast_push(L, n->as.if_stmt.then_block); lua_setfield(L, t, "tb");
            ast_push(L, n->as.if_stmt.else_block); lua_setfield(L, t, "eb");
            break;
        case NG_AST_STMT_WHILE:
            ast_push(L, n->as.while_stmt.condition); lua_setfield(L, t, "c");
            ast_push(L, n->as.while_stmt.block); lua_setfield(L, t, "b");
            break;
        case NG_AST_STMT_FUNC:
            set_str(L, t, "name", n->as.func_stmt.name);
            set_str_arr(L, t, "params", n->as.func_stmt.param_count,
                        n->as.func_stmt.params);
            ast_push(L, n->as.func_stmt.body); lua_setfield(L, t, "b");
            break;
        case NG_AST_STMT_IMPORT:
            set_str(L, t, "name", n->as.import_stmt.module_name);
            set_str(L, t, "alias", n->as.import_stmt.alias);
            break;
        case NG_AST_EXPR_BINARY:
            lua_pushinteger(L, (lua_Integer)n->as.binary_expr.op);
            lua_setfield(L, t, "op");
            ast_push(L, n->as.binary_expr.left); lua_setfield(L, t, "lft");
            ast_push(L, n->as.binary_expr.right); lua_setfield(L, t, "rgt");
            break;
        case NG_AST_EXPR_LITERAL: {
            const NGValue* v = &n->as.literal_expr.value;
            lua_pushinteger(L, (lua_Integer)v->type); lua_setfield(L, t, "vt");
            switch (v->type) {
                case NG_VAL_NUMBER:
                    lua_pushnumber(L, v->as.number); lua_setfield(L, t, "n");
                    break;
                case NG_VAL_STRING:
                    lua_pushstring(L, v->as.string); lua_setfield(L, t, "s");
                    break;
                case NG_VAL_BOOL:
                    lua_pushboolean(L, v->as.boolean); lua_setfield(L, t, "b");
                    break;
                default: break;
            }
            break;
        }
        case NG_AST_EXPR_IDENTIFIER:
            set_str(L, t, "name", n->as.id_expr.name);
            break;
        case NG_AST_EXPR_CALL:
            ast_push(L, n->as.call_expr.callee); lua_setfield(L, t, "fn");
            set_arr(L, t, "args", n->as.call_expr.arg_count, n->as.call_expr.args);
            break;
        case NG_AST_EXPR_TABLE_GET:
            ast_push(L, n->as.table_get.table); lua_setfield(L, t, "tbl");
            set_str(L, t, "f", n->as.table_get.field);
            break;
        case NG_AST_STMT_TABLE_SET:
            ast_push(L, n->as.table_set.table); lua_setfield(L, t, "tbl");
            set_str(L, t, "f", n->as.table_set.field);
            ast_push(L, n->as.table_set.value); lua_setfield(L, t, "v");
            break;
        case NG_AST_EXPR_FUNC:
            if (n->as.func_expr.name) set_str(L, t, "name", n->as.func_expr.name);
            set_str_arr(L, t, "params", n->as.func_expr.param_count,
                        n->as.func_expr.params);
            ast_push(L, n->as.func_expr.body); lua_setfield(L, t, "b");
            break;
        case NG_AST_STMT_LOOP:
            set_str(L, t, "var", n->as.loop_stmt.var_name);
            ast_push(L, n->as.loop_stmt.iterable); lua_setfield(L, t, "it");
            ast_push(L, n->as.loop_stmt.body); lua_setfield(L, t, "b");
            break;
        case NG_AST_EXPR_ARRAY:
            set_arr(L, t, "kids", n->as.array_expr.count, n->as.array_expr.elements);
            break;
        case NG_AST_EXPR_INDEX:
            ast_push(L, n->as.index_expr.target); lua_setfield(L, t, "t");
            ast_push(L, n->as.index_expr.index); lua_setfield(L, t, "i");
            break;
        default: break;
    }
    return t;
}

static int get_int(lua_State* L, int idx, const char* k, int dflt) {
    int v = dflt;
    lua_getfield(L, idx, k);
    if (!lua_isnil(L, -1)) v = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

static double get_num(lua_State* L, int idx, const char* k, double dflt) {
    double v = dflt;
    lua_getfield(L, idx, k);
    if (!lua_isnil(L, -1)) v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static char* get_str(lua_State* L, int idx, const char* k) {
    char* r = NULL;
    lua_getfield(L, idx, k);
    const char* s = lua_tostring(L, -1);
    if (s) r = strdup(s);
    lua_pop(L, 1);
    return r;
}

static NGAstNode* ast_pop_field(lua_State* L, int idx, const char* key) {
    lua_getfield(L, idx, key);
    NGAstNode* r = ast_pop(L, lua_gettop(L));
    lua_pop(L, 1);
    return r;
}

static NGAstNode** arr_pop(lua_State* L, int idx, const char* key, size_t* out_n) {
    *out_n = 0;
    lua_getfield(L, idx, key);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return NULL; }
    size_t n = lua_rawlen(L, -1);
    NGAstNode** arr = n ? (NGAstNode**)calloc(n, sizeof(NGAstNode*)) : NULL;
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, -1, (int)i + 1);
        arr[i] = ast_pop(L, lua_gettop(L));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    *out_n = n;
    return arr;
}

static void params_pop(lua_State* L, int idx, const char* key, char*** out_params,
                       size_t* out_count, size_t* out_cap) {
    *out_params = NULL;
    *out_count = 0;
    *out_cap = 0;
    lua_getfield(L, idx, key);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    size_t n = lua_rawlen(L, -1);
    char** arr = n ? (char**)calloc(n, sizeof(char*)) : NULL;
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, -1, (int)i + 1);
        const char* s = lua_tostring(L, -1);
        arr[i] = s ? strdup(s) : NULL;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    *out_params = arr;
    *out_count = n;
    *out_cap = n;
}

static NGAstNode* ast_pop(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return NULL;
    int t = get_int(L, idx, "t", -1);
    if (t < 0) return NULL;
    int line = get_int(L, idx, "l", 0);
    int col = get_int(L, idx, "c", 0);
    NGAstNode* n = ng_ast_new((NGAstNodeType)t, line, col);
    switch (t) {
        case NG_AST_PROGRAM:
            n->as.program.statements = arr_pop(L, idx, "kids", &n->as.program.count);
            n->as.program.capacity = n->as.program.count;
            break;
        case NG_AST_STMT_PRINT: n->as.print_stmt.expr = ast_pop_field(L, idx, "e"); break;
        case NG_AST_STMT_G: n->as.g_stmt.expr = ast_pop_field(L, idx, "e"); break;
        case NG_AST_STMT_Y: n->as.y_stmt.expr = ast_pop_field(L, idx, "e"); break;
        case NG_AST_STMT_ASSIGN:
            n->as.assign_stmt.name = get_str(L, idx, "name");
            n->as.assign_stmt.value = ast_pop_field(L, idx, "v");
            break;
        case NG_AST_STMT_IF:
            n->as.if_stmt.condition = ast_pop_field(L, idx, "c");
            n->as.if_stmt.then_block = ast_pop_field(L, idx, "tb");
            n->as.if_stmt.else_block = ast_pop_field(L, idx, "eb");
            break;
        case NG_AST_STMT_WHILE:
            n->as.while_stmt.condition = ast_pop_field(L, idx, "c");
            n->as.while_stmt.block = ast_pop_field(L, idx, "b");
            break;
        case NG_AST_STMT_FUNC:
            n->as.func_stmt.name = get_str(L, idx, "name");
            params_pop(L, idx, "params", &n->as.func_stmt.params,
                       &n->as.func_stmt.param_count, &n->as.func_stmt.param_capacity);
            n->as.func_stmt.body = ast_pop_field(L, idx, "b");
            break;
        case NG_AST_STMT_IMPORT:
            n->as.import_stmt.module_name = get_str(L, idx, "name");
            n->as.import_stmt.alias = get_str(L, idx, "alias");
            break;
        case NG_AST_EXPR_BINARY:
            n->as.binary_expr.op = get_int(L, idx, "op", 0);
            n->as.binary_expr.left = ast_pop_field(L, idx, "lft");
            n->as.binary_expr.right = ast_pop_field(L, idx, "rgt");
            break;
        case NG_AST_EXPR_LITERAL: {
            int vt = get_int(L, idx, "vt", NG_VAL_NULL);
            if (vt == NG_VAL_NUMBER) {
                n->as.literal_expr.value = ng_val_number(get_num(L, idx, "n", 0.0));
            } else if (vt == NG_VAL_STRING) {
                char* s = get_str(L, idx, "s");
                n->as.literal_expr.value = ng_val_string(s ? s : "");
                free(s);
            } else if (vt == NG_VAL_BOOL) {
                n->as.literal_expr.value = ng_val_bool(get_int(L, idx, "b", 0) != 0);
            } else {
                n->as.literal_expr.value = ng_val_null();
            }
            break;
        }
        case NG_AST_EXPR_IDENTIFIER: n->as.id_expr.name = get_str(L, idx, "name"); break;
        case NG_AST_EXPR_CALL:
            n->as.call_expr.callee = ast_pop_field(L, idx, "fn");
            n->as.call_expr.args = arr_pop(L, idx, "args", &n->as.call_expr.arg_count);
            n->as.call_expr.arg_capacity = n->as.call_expr.arg_count;
            break;
        case NG_AST_EXPR_TABLE_GET:
            n->as.table_get.table = ast_pop_field(L, idx, "tbl");
            n->as.table_get.field = get_str(L, idx, "f");
            break;
        case NG_AST_STMT_TABLE_SET:
            n->as.table_set.table = ast_pop_field(L, idx, "tbl");
            n->as.table_set.field = get_str(L, idx, "f");
            n->as.table_set.value = ast_pop_field(L, idx, "v");
            break;
        case NG_AST_EXPR_FUNC:
            n->as.func_expr.name = get_str(L, idx, "name");
            params_pop(L, idx, "params", &n->as.func_expr.params,
                       &n->as.func_expr.param_count, &n->as.func_expr.param_capacity);
            n->as.func_expr.body = ast_pop_field(L, idx, "b");
            break;
        case NG_AST_STMT_LOOP:
            n->as.loop_stmt.var_name = get_str(L, idx, "var");
            n->as.loop_stmt.iterable = ast_pop_field(L, idx, "it");
            n->as.loop_stmt.body = ast_pop_field(L, idx, "b");
            break;
        case NG_AST_EXPR_ARRAY:
            n->as.array_expr.elements = arr_pop(L, idx, "kids", &n->as.array_expr.count);
            n->as.array_expr.capacity = n->as.array_expr.count;
            break;
        case NG_AST_EXPR_INDEX:
            n->as.index_expr.target = ast_pop_field(L, idx, "t");
            n->as.index_expr.index = ast_pop_field(L, idx, "i");
            break;
        default: break;
    }
    return n;
}

/* ------------------------------------------------------------
 * Otimizador semântico em Lua (Core 1)
 * ------------------------------------------------------------ */

static const char* NG_LUA_OPTIMIZER_SCRIPT_STR =
    "local T_PROGRAM=0; local T_PRINT=1; local T_G=2; local T_Y=3; local T_ASSIGN=4\n"
    "local T_IF=5; local T_WHILE=6; local T_FUNC=7; local T_IMPORT=8; local T_BIN=9\n"
    "local T_LIT=10; local T_ID=11; local T_CALL=12; local T_TABLE_GET=13\n"
    "local T_TABLE_SET=14; local T_EXPR_FUNC=15; local T_LOOP=16; local T_ARRAY=17\n"
    "local T_INDEX=18\n"
    "local OP_PLUS=27; local OP_MINUS=28; local OP_STAR=29; local OP_SLASH=30\n"
    "local OP_PERCENT=31; local OP_EQ=32; local OP_NEQ=33; local OP_LT=34\n"
    "local OP_LTE=35; local OP_GT=36; local OP_GTE=37\n"
    "local stats={folds=0,dce=0,alg=0,functions=0,globals=0,loops=0,calls=0}\n"
    "local function is_num(n) return n and n.t==T_LIT and n.vt=='number' end\n"
    "local function is_str(n) return n and n.t==T_LIT and n.vt=='string' end\n"
    "local function truthy(n)\n"
    "  if is_num(n) then return n.n~=0 end\n"
    "  if n and n.t==T_LIT then return n.b==true end\n"
    "  return true\n"
    "end\n"
    "local function walk_all(node,fn)\n"
    "  if not node then return end\n"
    "  fn(node)\n"
    "  local function w(v)\n"
    "    if v and type(v)=='table' and v.t then walk_all(v,fn) end\n"
    "  end\n"
    "  for _,c in ipairs(node.kids or {}) do w(c) end\n"
    "  for _,c in ipairs(node.args or {}) do w(c) end\n"
    "  w(node.e); w(node.c); w(node.tb); w(node.eb); w(node.b)\n"
    "  w(node.it); w(node.i); w(node.tbl); w(node.v)\n"
    "end\n"
    "local function fold_binary(node)\n"
    "  local l=node.lft; local r=node.rgt\n"
    "  if is_num(l) and is_num(r) then\n"
    "    local a=l.n; local b=r.n; local res,isb,op=nil,false,node.op\n"
    "    if op==OP_PLUS then res=a+b\n"
    "    elseif op==OP_MINUS then res=a-b\n"
    "    elseif op==OP_STAR then res=a*b\n"
    "    elseif op==OP_SLASH then if b~=0 then res=a/b end\n"
    "    elseif op==OP_PERCENT then if b~=0 and a>=0 and b>0 then res=a%b end\n"
    "    elseif op==OP_EQ then res=(a==b); isb=true\n"
    "    elseif op==OP_NEQ then res=(a~=b); isb=true\n"
    "    elseif op==OP_LT then res=(a<b); isb=true\n"
    "    elseif op==OP_LTE then res=(a<=b); isb=true\n"
    "    elseif op==OP_GT then res=(a>b); isb=true\n"
    "    elseif op==OP_GTE then res=(a>=b); isb=true\n"
    "    end\n"
    "    if res~=nil then\n"
    "      stats.folds=stats.folds+1\n"
    "      if isb then return {t=T_LIT,l=node.l,c=node.c,vt='bool',b=res} end\n"
    "      return {t=T_LIT,l=node.l,c=node.c,vt='number',n=res}\n"
    "    end\n"
    "  end\n"
    "  if node.op==OP_PLUS and is_str(l) and is_str(r) then\n"
    "    stats.folds=stats.folds+1\n"
    "    return {t=T_LIT,l=node.l,c=node.c,vt='string',s=l.s..r.s}\n"
    "  end\n"
    "  if is_num(r) then\n"
    "    local b=r.n\n"
    "    if (node.op==OP_PLUS or node.op==OP_MINUS) and b==0 then\n"
    "      stats.alg=stats.alg+1; return l\n"
    "    end\n"
    "    if node.op==OP_STAR and b==1 then\n"
    "      stats.alg=stats.alg+1; return l\n"
    "    end\n"
    "  end\n"
    "  if is_num(l) then\n"
    "    local a=l.n\n"
    "    if node.op==OP_PLUS and a==0 then\n"
    "      stats.alg=stats.alg+1; return r\n"
    "    end\n"
    "    if node.op==OP_STAR and a==1 then\n"
    "      stats.alg=stats.alg+1; return r\n"
    "    end\n"
    "  end\n"
    "  return node\n"
    "end\n"
    "local function opt_node(node)\n"
    "  if not node then return nil end\n"
    "  if node.t==T_BIN then\n"
    "    node.lft=opt_node(node.lft); node.rgt=opt_node(node.rgt)\n"
    "    return fold_binary(node)\n"
    "  end\n"
    "  local function opt_if(nd)\n"
    "    nd.c=opt_node(nd.c); nd.tb=opt_node(nd.tb); nd.eb=opt_node(nd.eb)\n"
    "    if truthy(nd.c) and nd.tb then\n"
    "      stats.dce=stats.dce+1; return nd.tb\n"
    "    end\n"
    "    if not truthy(nd.c) and nd.eb then\n"
    "      stats.dce=stats.dce+1; return nd.eb\n"
    "    end\n"
    "    return nd\n"
    "  end\n"
    "  if node.t==T_IF then return opt_if(node) end\n"
    "  if node.kids then for i=1,#node.kids do node.kids[i]=opt_node(node.kids[i]) end end\n"
    "  for _,f in ipairs({'e','c','tb','eb','b','it','i','tbl','v'}) do\n"
    "    if node[f] then node[f]=opt_node(node[f]) end\n"
    "  end\n"
    "  return node\n"
    "end\n"
    "local function count_symbols(node)\n"
    "  walk_all(node,function(n)\n"
    "    if n.t==T_FUNC then stats.functions=stats.functions+1 end\n"
    "    if n.t==T_EXPR_FUNC then stats.functions=stats.functions+1 end\n"
    "    if n.t==T_ASSIGN and type(n.name)=='string' then stats.globals=stats.globals+1 end\n"
    "    if n.t==T_WHILE or n.t==T_LOOP then stats.loops=stats.loops+1 end\n"
    "    if n.t==T_CALL then stats.calls=stats.calls+1 end\n"
    "  end)\n"
    "end\n"
    "function optimize_ast(ast)\n"
    "  local out=opt_node(ast)\n"
    "  if out then count_symbols(out) end\n"
    "  return out,stats\n"
    "end\n";

const char* ng_lua_optimizer_script(void) {
    return NG_LUA_OPTIMIZER_SCRIPT_STR;
}

bool ng_lua_optimize_ast(lua_State* L, NGAstNode* ast, NGLuaStats* stats,
                         NGAstNode** out) {
    *out = ast;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!L || !ast) return false;

    lua_getglobal(L, "optimize_ast");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    ast_push(L, ast);
    if (lua_pcall(L, 1, 2, 0) != 0) {
        lua_pop(L, 1); /* descarta mensagem de erro */
        return false;
    }
    int st_idx = lua_gettop(L);
    int opt_idx = st_idx - 1;
    NGAstNode* opt = ast_pop(L, opt_idx);
    if (opt) *out = opt;
    if (stats) {
        stats->folds = (uint64_t)get_int(L, st_idx, "folds", 0);
        stats->dce = (uint64_t)get_int(L, st_idx, "dce", 0);
        stats->alg = (uint64_t)get_int(L, st_idx, "alg", 0);
        stats->functions = (uint64_t)get_int(L, st_idx, "functions", 0);
        stats->globals = (uint64_t)get_int(L, st_idx, "globals", 0);
        stats->loops = (uint64_t)get_int(L, st_idx, "loops", 0);
        stats->calls = (uint64_t)get_int(L, st_idx, "calls", 0);
    }
    lua_pop(L, 2);
    lua_gc(L, LUA_GCCOLLECT);
    return opt != NULL;
}

#else /* !NG_HAS_EMBEDDED_LUA: build sem Lua -> fallback seguro para o Core 0 */

const char* ng_lua_optimizer_script(void) {
    return "";
}

bool ng_lua_optimize_ast(lua_State* L, NGAstNode* ast, NGLuaStats* stats,
                         NGAstNode** out) {
    (void)L;
    (void)ast;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (out) *out = NULL;
    return false;
}

#endif /* NG_HAS_EMBEDDED_LUA */
