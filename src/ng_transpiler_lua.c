#include "ng_transpiler_lua.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static bool sb_init(StrBuf *sb, size_t cap) {
    sb->cap = cap > 64 ? cap : 64;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
    return sb->data != NULL;
}

static bool sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (!s || n == 0) return true;
    if (sb->len + n + 1 > sb->cap) {
        size_t next = sb->cap * 2;
        while (next < sb->len + n + 1) next *= 2;
        char *p = (char *)realloc(sb->data, next);
        if (!p) return false;
        sb->data = p;
        sb->cap = next;
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
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

static char *strip_block_end(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (isspace((unsigned char)s[len - 1]) || s[len - 1] == '[' || s[len - 1] == ']')) {
        s[--len] = '\0';
    }
    return s;
}

static const char *match_prefix(const char *s, const char *word) {
    size_t len = strlen(word);
    if (strncmp(s, word, len) != 0) return NULL;
    char next = s[len];
    if (next == '\0' || isspace((unsigned char)next) || next == '(' || next == '[') {
        return s + len;
    }
    return NULL;
}

/* Traduz expressoes simples de NG para Lua sem deixar '@' solto:
   - listas literais ["a", "b"] -> Lua {"a", "b"}
   - acesso por indice lista[0] permanece lista[0]
   - Modulo.Funcao (Time.Now) -> time.agora */
static bool translate_expr(const char *expr, StrBuf *out) {
    char quote = 0;
    bool index_mode = false;
    size_t n = strlen(expr);

    for (size_t i = 0; i < n; ) {
        char c = expr[i];

        if (quote) {
            if (!sb_append_char(out, c)) return false;
            if (c == quote && (i == 0 || expr[i - 1] != '\\')) quote = 0;
            i++;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            if (!sb_append_char(out, c)) return false;
            i++;
            continue;
        }

        if (c == '@') {
            i++;
            continue;
        }

        if (c == '[') {
            index_mode = (expr[i + 1] != '"' && expr[i + 1] != '\'');
            if (!sb_append_char(out, index_mode ? '[' : '{')) return false;
            i++;
            continue;
        }
        if (c == ']') {
            if (!sb_append_char(out, index_mode ? ']' : '}')) return false;
            index_mode = false;
            i++;
            continue;
        }

        if (c == '!' && expr[i + 1] == '=') {
            if (!sb_append(out, "~=")) return false;
            i += 2;
            continue;
        }

        if (c == '+') {
            size_t k = i + 1;
            while (k < n && isspace((unsigned char)expr[k])) k++;
            const char *op = (k < n && (expr[k] == '"' || expr[k] == '\'')) ? ".." : "+";
            if (!sb_append(out, op)) return false;
            i++;
            continue;
        }

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

        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < n && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) i++;
            size_t wlen = i - start;

            if (i < n && expr[i] == '.') {
                char mod[32];
                size_t ml = wlen < sizeof(mod) - 1 ? wlen : sizeof(mod) - 1;
                for (size_t k = 0; k < ml; k++) {
                    mod[k] = (char)tolower((unsigned char)expr[start + k]);
                }
                mod[ml] = '\0';
                if (!sb_append(out, mod) || !sb_append_char(out, '.')) return false;
                i++;

                size_t fstart = i;
                while (i < n && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) i++;
                size_t flen = i - fstart;
                const char *fn = expr + fstart;
                if (strcmp(mod, "time") == 0) {
                    if (flen == 3 && memcmp(fn, "Now", 3) == 0) { fn = "agora"; flen = 5; }
                    else if (flen == 5 && memcmp(fn, "Epoch", 5) == 0) { fn = "epoch"; flen = 5; }
                    else if (flen == 6 && memcmp(fn, "Dormir", 6) == 0) { fn = "dormir"; flen = 6; }
                }
                if (!sb_append_n(out, fn, flen)) return false;
                continue;
            }

            const char *src = expr + start;
            if (wlen == 1 && *src == 'E') { if (!sb_append(out, "and")) return false; }
            else if (wlen == 2 && memcmp(src, "OU", 2) == 0) { if (!sb_append(out, "or")) return false; }
            else if (wlen == 3 && memcmp(src, "Nao", 3) == 0) { if (!sb_append(out, "not ")) return false; }
            else if (wlen == 9 && memcmp(src, "Verdadeiro", 9) == 0) { if (!sb_append(out, "true")) return false; }
            else if (wlen == 5 && memcmp(src, "Falso", 5) == 0) { if (!sb_append(out, "false")) return false; }
            else if (wlen == 4 && memcmp(src, "Nulo", 4) == 0) { if (!sb_append(out, "nil")) return false; }
            else { if (!sb_append_n(out, src, wlen)) return false; }
            continue;
        }

        if (!sb_append_char(out, c)) return false;
        i++;
    }
    return true;
}

static bool emit_module_import(StrBuf *out, const char *raw) {
    while (*raw && isspace((unsigned char)*raw)) raw++;
    if (*raw == '@') raw++;
    char mod[64];
    size_t j = 0;
    while (*raw && !isspace((unsigned char)*raw) && j + 1 < sizeof(mod)) mod[j++] = *raw++;
    mod[j] = '\0';
    if (!j) return true;

    if (strcmp(mod, "time") == 0) {
        return sb_append(out,
            "local time = {\n"
            "  agora = function() return os.clock() end,\n"
            "  epoch = function() return os.time() end,\n"
            "  dormir = function(seg) local t0 = os.clock(); while os.clock() - t0 < (seg or 0) do end end\n"
            "}\n");
    }
    return sb_append(out, "local ") && sb_append(out, mod) &&
           sb_append(out, " = rawget(_G, \"") && sb_append(out, mod) &&
           sb_append(out, "\") or {}\n");
}

typedef enum {
    BLK_NONE = 0,
    BLK_IF,
    BLK_ELSE,
    BLK_LOOP,
    BLK_FUNC
} BlockKind;

#define STACK_MAX 64
static BlockKind g_stack[STACK_MAX];
static int g_depth = 0;

static void stack_push(BlockKind k) {
    if (g_depth < STACK_MAX) g_stack[g_depth++] = k;
}

static bool find_top_level_assign(const char *s, size_t *pos) {
    char quote = 0;
    for (size_t i = 0; s[i]; i++) {
        if (quote) {
            if (s[i] == quote && (i == 0 || s[i - 1] != '\\')) quote = 0;
        } else if (s[i] == '\'' || s[i] == '"') {
            quote = s[i];
        } else if (s[i] == '=' && s[i + 1] != '=' &&
                   (i == 0 || (s[i - 1] != '=' && s[i - 1] != '!' && s[i - 1] != '<' && s[i - 1] != '>'))) {
            *pos = i;
            return true;
        }
    }
    return false;
}

static bool transpile_line(char *line, StrBuf *out) {
    char *trimmed = trim_whitespace(line);
    if (*trimmed == '\0') {
        return sb_append_char(out, '\n');
    }

    if (*trimmed == '#') {
        return sb_append(out, "--") && sb_append(out, trimmed + 1) && sb_append_char(out, '\n');
    }

    const char *p;
    if ((p = match_prefix(trimmed, "Importar"))) {
        return emit_module_import(out, p);
    }

    if ((p = match_prefix(trimmed, "Enquanto"))) {
        char *cond = strip_block_end(trim_whitespace((char *)p));
        bool ok = sb_append(out, "while ") && translate_expr(cond, out) && sb_append(out, " do\n");
        if (ok) stack_push(BLK_LOOP);
        return ok;
    }

    if ((p = match_prefix(trimmed, "Se"))) {
        char *cond = strip_block_end(trim_whitespace((char *)p));
        bool ok = sb_append(out, "if ") && translate_expr(cond, out) && sb_append(out, " then\n");
        if (ok) stack_push(BLK_IF);
        return ok;
    }

    if ((p = match_prefix(trimmed, "Loop"))) {
        const char *in_pos = strstr(p, " in ");
        if (!in_pos) return false;
        char var_buf[64];
        size_t vlen = (size_t)(in_pos - p);
        if (vlen >= sizeof(var_buf)) vlen = sizeof(var_buf) - 1;
        memcpy(var_buf, p, vlen);
        var_buf[vlen] = '\0';
        char *var = trim_whitespace(var_buf);
        char *list = strip_block_end(trim_whitespace((char *)in_pos + 4));
        bool ok = sb_append(out, "for ") && sb_append(out, var) &&
                  sb_append(out, " in ipairs(") && sb_append(out, list) &&
                  sb_append(out, ") do\n");
        if (ok) stack_push(BLK_LOOP);
        return ok;
    }

    if ((p = match_prefix(trimmed, "Funcao")) || (p = match_prefix(trimmed, "Função"))) {
        strip_block_end((char *)p);
        p = trim_whitespace((char *)p);
        bool ok = sb_append(out, "function ") && sb_append(out, p) && sb_append_char(out, '\n');
        if (ok) stack_push(BLK_FUNC);
        return ok;
    }

    if ((p = match_prefix(trimmed, "Retornar"))) {
        char *val = trim_whitespace((char *)p);
        if (*val == '\0') return sb_append(out, "return\n");
        return sb_append(out, "return ") && translate_expr(val, out) && sb_append_char(out, '\n');
    }

    if ((p = match_prefix(trimmed, "Print"))) {
        char *arg = trim_whitespace((char *)p);
        return sb_append(out, "print(") && translate_expr(arg, out) && sb_append(out, ")\n");
    }

    if (trimmed[0] == 'G' && (trimmed[1] == ' ' || trimmed[1] == '\0')) {
        char *arg = trim_whitespace(trimmed + 1);
        return sb_append(out, "print(") && translate_expr(arg, out) && sb_append(out, ")\n");
    }

    if (match_prefix(trimmed, "Senao")) {
        if (g_depth > 0) g_stack[g_depth - 1] = BLK_ELSE;
        return sb_append(out, "else\n");
    }

    if (match_prefix(trimmed, "Fim")) {
        if (g_depth > 0 && g_stack[g_depth - 1] == BLK_FUNC) g_depth--;
        return sb_append(out, "end\n");
    }

    if (trimmed[0] == ']') {
        if (g_depth > 0 && g_stack[g_depth - 1] == BLK_IF && strstr(trimmed, "Senao")) {
            g_stack[g_depth - 1] = BLK_ELSE;
            return sb_append(out, "else\n");
        }
        if (g_depth > 0) g_depth--;
        return sb_append(out, "end\n");
    }

    size_t eq_pos = 0;
    if (find_top_level_assign(trimmed, &eq_pos)) {
        trimmed[eq_pos] = '\0';
        char *lhs = trim_whitespace(trimmed);
        char *rhs = trim_whitespace(trimmed + eq_pos + 1);
        return sb_append(out, lhs) && sb_append(out, " = ") &&
               translate_expr(rhs, out) && sb_append_char(out, '\n');
    }

    return translate_expr(trimmed, out) && sb_append_char(out, '\n');
}

char *ng_transpile_to_lua(const char *ng_source, size_t *out_len) {
    if (!ng_source) return NULL;

    StrBuf out;
    if (!sb_init(&out, strlen(ng_source) + 256)) return NULL;

    g_depth = 0;

    if (!sb_append(&out, "-- Gerado em tempo real de NG para Lua\n")) {
        free(out.data);
        return NULL;
    }

    const char *p = ng_source;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        char *line = (char *)malloc(len + 1);
        if (!line) {
            free(out.data);
            return NULL;
        }
        memcpy(line, p, len);
        line[len] = '\0';

        if (!transpile_line(line, &out)) {
            free(line);
            free(out.data);
            return NULL;
        }
        free(line);

        if (!nl) break;
        p = nl + 1;
    }

    if (out_len) *out_len = out.len;
    return out.data;
}
