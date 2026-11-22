#include "ng_transpiler_lua.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static bool sb_init(StrBuf *sb, size_t initial_cap) {
    sb->cap = initial_cap > 64 ? initial_cap : 64;
    sb->data = (char *)malloc(sb->cap);
    if (!sb->data) {
        sb->len = 0;
        sb->cap = 0;
        return false;
    }
    sb->len = 0;
    sb->data[0] = '\0';
    return true;
}

static bool sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) {
        return true;
    }
    size_t next_cap = sb->cap;
    while (next_cap < sb->len + extra + 1) {
        next_cap *= 2;
    }
    char *grown = (char *)realloc(sb->data, next_cap);
    if (!grown) {
        return false;
    }
    sb->data = grown;
    sb->cap = next_cap;
    return true;
}

static bool sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (!s || n == 0) {
        return true;
    }
    if (!sb_reserve(sb, n)) {
        return false;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append(StrBuf *sb, const char *s) {
    return sb_append_n(sb, s, s ? strlen(s) : 0);
}

static bool sb_append_char(StrBuf *sb, char c) {
    return sb_append_n(sb, &c, 1);
}

static char *trim_whitespace(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static bool starts_with_word(const char *s, const char *word) {
    size_t wlen = strlen(word);
    if (strncmp(s, word, wlen) != 0) {
        return false;
    }
    char next = s[wlen];
    return next == '\0' || isspace((unsigned char)next) || next == '(' || next == '[';
}

/* Funcao/Função com acento suportados */
static bool starts_with_funcao(const char *s, const char **sig_out) {
    if (starts_with_word(s, "Funcao")) {
        *sig_out = s + strlen("Funcao");
        return true;
    }
    if (starts_with_word(s, "Função")) {
        *sig_out = s + strlen("Função");
        return true;
    }
    return false;
}

/* Traduz expressoes simples de NG para Lua sem deixar '@' solto.
   - listas NG ["a", "b"] -> Lua {"a", "b"}
   - acesso por indice NG lista[0] mantem o formato Lua lista[0]
   - identificadores com modulo (Time.Now) -> modulo.minha_funcao */
static bool translate_expr(const char *expr, StrBuf *out) {
    bool in_single = false;
    bool in_double = false;
    bool index_mode = false;
    size_t i = 0;
    size_t n = strlen(expr);

    while (i < n) {
        char c = expr[i];

        if (c == '\'' && !in_double) {
            bool escaped = (i > 0 && expr[i - 1] == '\\');
            if (!escaped) {
                in_single = !in_single;
            }
            if (!sb_append_char(out, c)) return false;
            i++;
            continue;
        }

        if (c == '"' && !in_single) {
            bool escaped = (i > 0 && expr[i - 1] == '\\');
            if (!escaped) {
                in_double = !in_double;
            }
            if (!sb_append_char(out, c)) return false;
            i++;
            continue;
        }

        if (!in_single && !in_double) {
            /* Remove '@' residual em identificadores/modulos */
            if (c == '@') {
                i++;
                continue;
            }
            /* Listas [...] -> {...} (literais) ; acesso por indice mantem [...] */
            if (c == '[') {
                if (expr[i + 1] == '"' || expr[i + 1] == '\'') {
                    if (!sb_append_char(out, '{')) return false;
                    index_mode = false;
                } else {
                    if (!sb_append_char(out, '[')) return false;
                    index_mode = true;
                }
                i++;
                continue;
            }
            if (c == ']') {
                if (index_mode) {
                    if (!sb_append_char(out, ']')) return false;
                    index_mode = false;
                } else {
                    if (!sb_append_char(out, '}')) return false;
                }
                i++;
                continue;
            }
            /* != -> ~= */
            if (c == '!' && expr[i + 1] == '=') {
                if (!sb_append(out, "~=")) return false;
                i += 2;
                continue;
            }
            /* NG: '+' entre strings vira Lua '..' (heuristic: frente eh string literal) */
            if (c == '+' && !in_single && !in_double) {
                size_t k = i + 1;
                while (k < n && isspace((unsigned char)expr[k])) k++;
                if (k < n && (expr[k] == '"' || expr[k] == '\'')) {
                    if (!sb_append(out, "..")) return false;
                } else {
                    if (!sb_append_char(out, c)) return false;
                }
                i++;
                continue;
            }
            /* && / || */
            if (c == '&' && expr[i + 1] == '&') {
                if (!sb_append(out, " and ")) return false;
                i += 2;
                continue;
            }
            if (c == '|' && expr[i + 1] == '|') {
                if (!sb_append(out, " or ")) return false;
                i += 2;
                continue;
            }
            /* Palavras reservadas logicas em portugues */
            if (isalpha((unsigned char)c) || c == '_') {
                size_t start = i;
                while (i < n && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) {
                    i++;
                }
                size_t wlen = i - start;
                if (i < n && expr[i] == '.') {
                    /* Identificador.modulo -> modulo.funcao (minusculacao do modulo) */
                    char lower[64];
                    size_t l = 0;
                    for (size_t k = start; k < start + wlen && l < sizeof(lower) - 1; k++) {
                        lower[l++] = (char)tolower((unsigned char)expr[k]);
                    }
                    lower[l] = '\0';
                    if (!sb_append_n(out, lower, l)) return false;
                    if (!sb_append_char(out, '.')) return false;
                    i++;
                    size_t s2 = i;
                    while (i < n && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) {
                        i++;
                    }
                    size_t w2 = (size_t)(i - s2);
                    if (w2 > 0) {
                        if (strcmp(lower, "time") == 0) {
                            if (w2 == 3 && strncmp(expr + s2, "Now", 3) == 0) {
                                if (!sb_append(out, "agora")) return false;
                            } else if (w2 == 5 && strncmp(expr + s2, "Epoch", 5) == 0) {
                                if (!sb_append(out, "epoch")) return false;
                            } else if (w2 == 6 && strncmp(expr + s2, "Dormir", 6) == 0) {
                                if (!sb_append(out, "dormir")) return false;
                            } else if (!sb_append_n(out, expr + s2, w2)) return false;
                        } else {
                            if (!sb_append_n(out, expr + s2, w2)) return false;
                        }
                    }
                    continue;
                }
                if (wlen == 1 && strncmp(expr + start, "E", 1) == 0) {
                    if (!sb_append(out, "and")) return false;
                } else if (wlen == 2 && strncmp(expr + start, "OU", 2) == 0) {
                    if (!sb_append(out, "or")) return false;
                } else if (wlen == 3 && strncmp(expr + start, "Nao", 3) == 0) {
                    if (!sb_append(out, "not ")) return false;
                } else if (wlen == 9 && strncmp(expr + start, "Verdadeiro", 9) == 0) {
                    if (!sb_append(out, "true")) return false;
                } else if (wlen == 5 && strncmp(expr + start, "Falso", 5) == 0) {
                    if (!sb_append(out, "false")) return false;
                } else if (wlen == 4 && strncmp(expr + start, "Nulo", 4) == 0) {
                    if (!sb_append(out, "nil")) return false;
                } else {
                    if (!sb_append_n(out, expr + start, wlen)) return false;
                }
                continue;
            }
        }

        if (!sb_append_char(out, c)) return false;
        i++;
    }

    return true;
}

static bool emit_module_import(StrBuf *out, const char *raw_target) {
    char mod_name[128];
    size_t j = 0;

    while (*raw_target && isspace((unsigned char)*raw_target)) {
        raw_target++;
    }
    if (*raw_target == '@') {
        raw_target++;
    }
    while (*raw_target && !isspace((unsigned char)*raw_target) && j + 1 < sizeof(mod_name)) {
        mod_name[j++] = *raw_target++;
    }
    mod_name[j] = '\0';

    if (mod_name[0] == '\0') {
        return true;
    }

    if (strcmp(mod_name, "time") == 0) {
        return sb_append(
            out,
            "local time = {\n"
            "  agora = function() return os.clock() end,\n"
            "  epoch = function() return os.time() end,\n"
            "  dormir = function(seg) local t0 = os.clock(); while os.clock() - t0 < (seg or 0) do end end\n"
            "}\n"
        );
    }

    if (!sb_append(out, "local ") ||
        !sb_append(out, mod_name) ||
        !sb_append(out, " = rawget(_G, \"") ||
        !sb_append(out, mod_name) ||
        !sb_append(out, "\") or {}\n")) {
        return false;
    }
    return true;
}

/* Pilha de blocos abertos para emitir 'end' nos fechamentos corretos */
#define NG_LUA_TRANSPILER_STACK_MAX 64
static char g_stack[NG_LUA_TRANSPILER_STACK_MAX][16];
static int g_stack_depth = 0;

static void ng_lua_transpiler_push(const char* kind) {
    if (g_stack_depth < NG_LUA_TRANSPILER_STACK_MAX) {
        snprintf(g_stack[g_stack_depth], 16, "%s", kind);
        g_stack_depth++;
    }
}

static void ng_lua_transpiler_pop(void) {
    if (g_stack_depth > 0) {
        g_stack_depth--;
    }
}

static const char* ng_lua_transpiler_top(void) {
    return g_stack_depth > 0 ? g_stack[g_stack_depth - 1] : "";
}

/* Localiza '=' (sem '==') fora de strings; retorna pos e comprimento do resto. */
static bool find_top_level_assign(const char* s, size_t* pos, size_t* len) {
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\'' && !in_double) in_single = !in_single;
        if (s[i] == '"' && !in_single) in_double = !in_double;
        if (!in_single && !in_double && s[i] == '=' && s[i + 1] && s[i + 1] != '=') {
            *pos = i;
            *len = strlen(s) - i - 1;
            return true;
        }
    }
    return false;
}

static bool transpile_line(const char *raw_line, StrBuf *out) {
    char *copy = (char *)malloc(strlen(raw_line) + 1);
    if (!copy) {
        return false;
    }
    strcpy(copy, raw_line);

    char *trimmed = trim_whitespace(copy);
    if (trimmed[0] == '\0') {
        free(copy);
        return sb_append_char(out, '\n');
    }

    /* Comentario de linha em NG (# ...) -> Lua (-- ...) */
    if (trimmed[0] == '#') {
        bool ok = sb_append(out, "--") &&
                  sb_append(out, trimmed + 1) &&
                  sb_append_char(out, '\n');
        free(copy);
        return ok;
    }

    /* Diretiva Importar @modulo */
    if (starts_with_word(trimmed, "Importar")) {
        const char *rest = trim_whitespace(trimmed + strlen("Importar"));
        bool ok = emit_module_import(out, rest);
        free(copy);
        return ok;
    }

    /* Enquanto <cond> [ -> while <cond> do */
    if (starts_with_word(trimmed, "Enquanto")) {
        char* cond = trimmed + strlen("Enquanto");
        size_t cl = strlen(cond);
        while (cl > 0 && (cond[cl - 1] == ' ' || cond[cl - 1] == '\t' || cond[cl - 1] == '[' || cond[cl - 1] == ']')) {
            cl--;
        }
        cond[cl] = '\0';
        StrBuf expr;
        if (!sb_init(&expr, 64)) {
            free(copy);
            return false;
        }
        bool ok = translate_expr(cond, &expr) &&
                  sb_append(out, "while ") &&
                  sb_append(out, expr.data) &&
                  sb_append(out, " do\n");
        free(expr.data);
        free(copy);
        if (ok) ng_lua_transpiler_push("while");
        return ok;
    }

    /* Se <cond> [ -> if <cond> then */
    if (starts_with_word(trimmed, "Se")) {
        char* cond = trimmed + strlen("Se");
        size_t cl = strlen(cond);
        while (cl > 0 && (cond[cl - 1] == ' ' || cond[cl - 1] == '\t' || cond[cl - 1] == '[' || cond[cl - 1] == ']')) {
            cl--;
        }
        cond[cl] = '\0';
        StrBuf expr;
        if (!sb_init(&expr, 64)) {
            free(copy);
            return false;
        }
        bool ok = translate_expr(cond, &expr) &&
                  sb_append(out, "if ") &&
                  sb_append(out, expr.data) &&
                  sb_append(out, " then\n");
        free(expr.data);
        free(copy);
        if (ok) ng_lua_transpiler_push("if");
        return ok;
    }

    /* Loop <var> in <lista> [ -> for <var> in <lista> do */
    if (starts_with_word(trimmed, "Loop")) {
        const char* p = trimmed + strlen("Loop");
        const char* in_pos = strstr(p, " in ");
        if (!in_pos) {
            free(copy);
            return false;
        }
        const char* var = trim_whitespace((char*)p);
        const char* lend = in_pos;
        char var_buf[128];
        size_t var_len = (size_t)(lend - var);
        if (var_len > sizeof(var_buf) - 1) var_len = sizeof(var_buf) - 1;
        memcpy(var_buf, var, var_len);
        var_buf[var_len] = '\0';
        const char* list = trim_whitespace((char*)in_pos + 4);
        size_t ll = strlen(list);
        while (ll > 0 && (list[ll - 1] == ' ' || list[ll - 1] == '\t' || list[ll - 1] == '[' || list[ll - 1] == ']')) {
            ll--;
        }
        char list_buf[256];
        if (ll > sizeof(list_buf) - 1) ll = sizeof(list_buf) - 1;
        memcpy(list_buf, list, ll);
        list_buf[ll] = '\0';
        bool ok = sb_append(out, "for ") &&
                  sb_append(out, var_buf) &&
                  sb_append(out, " in ipairs(") &&
                  sb_append(out, list_buf) &&
                  sb_append(out, ") do\n");
        free(copy);
        if (ok) ng_lua_transpiler_push("for");
        return ok;
    }
    /* Funcao nome(params) [ -> function nome(params) */
    const char* sig_ptr = NULL;
    if (starts_with_funcao(trimmed, &sig_ptr)) {
        const char* sig = sig_ptr;
        const char* lp = strchr(sig, '(');
        const char* rp = lp ? strchr(lp, ')') : NULL;
        char fn_line[256];
        if (lp && rp) {
            const char* name = trim_whitespace((char*)sig);
            size_t nl = (size_t)(lp - name);
            char name_buf[128];
            if (nl > sizeof(name_buf) - 1) nl = sizeof(name_buf) - 1;
            memcpy(name_buf, name, nl);
            name_buf[nl] = '\0';
            const char* params = trim_whitespace((char*)lp + 1);
            size_t pl = (size_t)(rp - params);
            char params_buf[128];
            if (pl > sizeof(params_buf) - 1) pl = sizeof(params_buf) - 1;
            memcpy(params_buf, params, pl);
            params_buf[pl] = '\0';
            snprintf(fn_line, sizeof(fn_line), "function %s(%s)\n", name_buf, params_buf);
        } else {
            snprintf(fn_line, sizeof(fn_line), "function %s\n", trim_whitespace((char*)sig));
        }
        bool ok = sb_append(out, fn_line);
        free(copy);
        if (ok) ng_lua_transpiler_push("func");
        return ok;
    }

    /* Retornar <expr> */
    if (starts_with_word(trimmed, "Retornar")) {
        const char *val = trim_whitespace((char*)trimmed + strlen("Retornar"));
        StrBuf expr;
        if (!sb_init(&expr, 64)) {
            free(copy);
            return false;
        }
        bool ok = sb_append(out, "return");
        if (val[0] != '\0') {
            ok = ok && sb_append_char(out, ' ') && translate_expr(val, &expr) && sb_append(out, expr.data);
        }
        ok = ok && sb_append_char(out, '\n');
        free(expr.data);
        free(copy);
        return ok;
    }

    /* Print <expr> */
    if (starts_with_word(trimmed, "Print")) {
        const char *arg = trim_whitespace((char*)trimmed + strlen("Print"));
        StrBuf expr;
        if (!sb_init(&expr, 64)) {
            free(copy);
            return false;
        }
        bool ok = translate_expr(arg, &expr) &&
                  sb_append(out, "print(") &&
                  sb_append(out, expr.data) &&
                  sb_append(out, ")\n");
        free(expr.data);
        free(copy);
        return ok;
    }

    /* Comando G <variavel> -> print(<variavel>) */
    if (trimmed[0] == 'G' && (trimmed[1] == ' ' || trimmed[1] == '\0')) {
        StrBuf expr;
        if (!sb_init(&expr, 64)) {
            free(copy);
            return false;
        }
        bool ok = translate_expr(trimmed + 1, &expr) && sb_append(out, "print(") && sb_append(out, expr.data) && sb_append(out, ")\n");
        free(expr.data);
        free(copy);
        return ok;
    }

    /* Senao / SenaoSe <cond> */
    if (starts_with_word(trimmed, "Senao")) {
        if (!sb_append(out, "else\n")) {
            free(copy);
            return false;
        }
        if (g_stack_depth > 0) {
            snprintf(g_stack[g_stack_depth - 1], 16, "else");
        }
        free(copy);
        return true;
    }

    /* Fim de bloco */
    if (starts_with_word(trimmed, "Fim")) {
        if (!sb_append(out, "end\n")) {
            free(copy);
            return false;
        }
        if (g_stack_depth > 0) {
            const char* top = ng_lua_transpiler_top();
            if (strcmp(top, "func") == 0) {
                ng_lua_transpiler_pop();
            }
        }
        free(copy);
        return true;
    }

    /* Fechamento de bloco: "]" ou "] Senao [" */
    if (trimmed[0] == ']') {
        bool has_else = strstr(trimmed, "Senao") != NULL;
        const char* top = ng_lua_transpiler_top();
        if (strcmp(top, "else") == 0) {
            if (!sb_append(out, "end\n")) {
                free(copy);
                return false;
            }
            ng_lua_transpiler_pop();
        } else if (strcmp(top, "if") == 0) {
            if (has_else) {
                /* fecha o bloco then e abre o bloco else */
                if (!sb_append(out, "else\n")) {
                    free(copy);
                    return false;
                }
                snprintf(g_stack[g_stack_depth - 1], 16, "else");
            } else {
                if (!sb_append(out, "end\n")) {
                    free(copy);
                    return false;
                }
                ng_lua_transpiler_pop();
            }
        } else if (strcmp(top, "while") == 0 || strcmp(top, "for") == 0 || strcmp(top, "func") == 0) {
            if (!sb_append(out, "end\n")) {
                free(copy);
                return false;
            }
            ng_lua_transpiler_pop();
        } else {
            if (!sb_append(out, "end\n")) {
                free(copy);
                return false;
            }
        }
        free(copy);
        return true;
    }

    /* Atribuicao: <var> = <expr> */
    {
        size_t eq_pos = 0;
        size_t eq_len = 0;
        if (find_top_level_assign(trimmed, &eq_pos, &eq_len)) {
            char lhs[128];
            size_t ll = eq_pos;
            if (ll > sizeof(lhs) - 1) ll = sizeof(lhs) - 1;
            memcpy(lhs, trimmed, ll);
            lhs[ll] = '\0';
            char* lhs_trim = trim_whitespace(lhs);
            const char* rhs = trimmed + eq_pos + 1;
            StrBuf expr;
            if (!sb_init(&expr, 64)) {
                free(copy);
                return false;
            }
            bool ok = translate_expr(rhs, &expr);
            char line_buf[1024];
            if (ok) {
                snprintf(line_buf, sizeof(line_buf), "%s = %s\n", lhs_trim, expr.data);
                ok = sb_append(out, line_buf);
            }
            free(expr.data);
            free(copy);
            return ok;
        }
    }

    /* Linha geral / chamada */
    {
        StrBuf expr;
        if (!sb_init(&expr, 128)) {
            free(copy);
            return false;
        }
        bool ok = translate_expr(trimmed, &expr) && sb_append(out, expr.data) && sb_append_char(out, '\n');
        free(expr.data);
        free(copy);
        return ok;
    }
}

char *ng_transpile_to_lua(const char *ng_source, size_t *out_len) {
    if (!ng_source) {
        return NULL;
    }

    StrBuf out;
    if (!sb_init(&out, strlen(ng_source) + 256)) {
        return NULL;
    }

    g_stack_depth = 0;

    if (!sb_append(&out, "-- Gerado em tempo real de NG para Lua\n")) {
        free(out.data);
        return NULL;
    }

    const char *p = ng_source;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);

        char *line = (char *)malloc(line_len + 1);
        if (!line) {
            free(out.data);
            return NULL;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        if (!transpile_line(line, &out)) {
            free(line);
            free(out.data);
            return NULL;
        }
        free(line);

        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    if (out_len) {
        *out_len = out.len;
    }
    return out.data;
}
