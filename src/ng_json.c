#include "ng_json.h"
#include "ng_runtime.h"
#include "ng_collection.h"
#include "ng_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Parser de JSON em C11 Puro - Zero Dependencias Externas */

typedef struct {
    const char* src;
    size_t pos;
    size_t len;
} JsonParser;

static void skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char peek_char(JsonParser* p) {
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static char next_char(JsonParser* p) {
    skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static NGValue parse_json_value(JsonParser* p);

static char* parse_json_string(JsonParser* p) {
    skip_ws(p);
    if (p->pos >= p->len || p->src[p->pos] != '"') return NULL;
    p->pos++; /* pula abre aspas */

    size_t cap = 64;
    size_t len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;

    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            buf[len] = '\0';
            return buf;
        }
        if (c == '\\') {
            if (p->pos >= p->len) break;
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    /* Suporte basico a unicode \uXXXX (converte 4 hex para ASCII se possivel) */
                    int code = 0;
                    for (int i = 0; i < 4 && p->pos < p->len; i++) {
                        char h = p->src[p->pos++];
                        int v = 0;
                        if (h >= '0' && h <= '9') v = h - '0';
                        else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                        code = (code << 4) | v;
                    }
                    if (code < 128) {
                        c = (char)code;
                    } else {
                        c = '?';
                    }
                    break;
                }
                default: c = esc; break;
            }
        }
        if (len + 2 >= cap) {
            cap *= 2;
            buf = (char*)realloc(buf, cap);
        }
        buf[len++] = c;
    }

    buf[len] = '\0';
    return buf;
}

static NGValue parse_json_number(JsonParser* p) {
    skip_ws(p);
    size_t start = p->pos;
    if (p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && (isdigit(p->src[p->pos]) || p->src[p->pos] == '.' ||
           p->src[p->pos] == 'e' || p->src[p->pos] == 'E' ||
           p->src[p->pos] == '+' || p->src[p->pos] == '-')) {
        p->pos++;
    }
    size_t num_len = p->pos - start;
    char num_buf[64];
    if (num_len >= sizeof(num_buf)) num_len = sizeof(num_buf) - 1;
    memcpy(num_buf, p->src + start, num_len);
    num_buf[num_len] = '\0';

    double val = strtod(num_buf, NULL);
    return ng_val_number(val);
}

static NGValue parse_json_array(JsonParser* p) {
    next_char(p); /* pula '[' */
    NGTable* tbl = ng_table_new(8);
    ng_table_set(tbl, "_is_array", ng_val_bool(true));

    skip_ws(p);
    if (peek_char(p) == ']') {
        next_char(p);
        ng_table_set(tbl, "length", ng_val_number(0));
        return ng_val_table(tbl);
    }

    size_t index = 0;
    while (p->pos < p->len) {
        NGValue val = parse_json_value(p);
        char idx_str[32];
        snprintf(idx_str, sizeof(idx_str), "%zu", index);
        ng_table_set(tbl, idx_str, val);
        index++;

        skip_ws(p);
        char c = peek_char(p);
        if (c == ',') {
            next_char(p);
        } else if (c == ']') {
            next_char(p);
            break;
        } else {
            break;
        }
    }

    ng_table_set(tbl, "length", ng_val_number((double)index));
    return ng_val_table(tbl);
}

static NGValue parse_json_object(JsonParser* p) {
    next_char(p); /* pula '{' */
    NGTable* tbl = ng_table_new(16);

    skip_ws(p);
    if (peek_char(p) == '}') {
        next_char(p);
        return ng_val_table(tbl);
    }

    while (p->pos < p->len) {
        skip_ws(p);
        if (peek_char(p) != '"') break;
        char* key = parse_json_string(p);
        if (!key) break;

        skip_ws(p);
        if (peek_char(p) == ':') {
            next_char(p);
        }

        NGValue val = parse_json_value(p);
        ng_table_set(tbl, key, val);
        free(key);

        skip_ws(p);
        char c = peek_char(p);
        if (c == ',') {
            next_char(p);
        } else if (c == '}') {
            next_char(p);
            break;
        } else {
            break;
        }
    }

    return ng_val_table(tbl);
}

static NGValue parse_json_value(JsonParser* p) {
    skip_ws(p);
    char c = peek_char(p);

    if (c == '"') {
        char* s = parse_json_string(p);
        NGValue v = ng_val_string(s ? s : "");
        if (s) free(s);
        return v;
    }
    if (c == '{') {
        return parse_json_object(p);
    }
    if (c == '[') {
        return parse_json_array(p);
    }
    if (isdigit(c) || c == '-') {
        return parse_json_number(p);
    }
    if (strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return ng_val_bool(true);
    }
    if (strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return ng_val_bool(false);
    }
    if (strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return ng_val_null();
    }

    /* Invalido, avanca caractere */
    if (p->pos < p->len) p->pos++;
    return ng_val_null();
}

NGValue ng_json_parse(const char* json_str) {
    if (!json_str || strlen(json_str) == 0) return ng_val_null();
    JsonParser p;
    p.src = json_str;
    p.pos = 0;
    p.len = strlen(json_str);
    return parse_json_value(&p);
}

/* Serializador de JSON em C11 */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} StrBuf;

static void buf_init(StrBuf* b) {
    b->cap = 128;
    b->len = 0;
    b->buf = (char*)malloc(b->cap);
    b->buf[0] = '\0';
}

static void buf_append(StrBuf* b, const char* str) {
    if (!str) return;
    size_t slen = strlen(str);
    if (b->len + slen + 1 >= b->cap) {
        while (b->len + slen + 1 >= b->cap) b->cap *= 2;
        b->buf = (char*)realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, str, slen);
    b->len += slen;
    b->buf[b->len] = '\0';
}

static void buf_append_escaped_string(StrBuf* b, const char* str) {
    buf_append(b, "\"");
    if (str) {
        for (size_t i = 0; str[i] != '\0'; i++) {
            char c = str[i];
            switch (c) {
                case '"':  buf_append(b, "\\\""); break;
                case '\\': buf_append(b, "\\\\"); break;
                case '\b': buf_append(b, "\\b"); break;
                case '\f': buf_append(b, "\\f"); break;
                case '\n': buf_append(b, "\\n"); break;
                case '\r': buf_append(b, "\\r"); break;
                case '\t': buf_append(b, "\\t"); break;
                default: {
                    char temp[2] = { c, '\0' };
                    buf_append(b, temp);
                    break;
                }
            }
        }
    }
    buf_append(b, "\"");
}

static void stringify_value(StrBuf* b, const NGValue* val) {
    if (!val) {
        buf_append(b, "null");
        return;
    }
    switch (val->type) {
        case NG_VAL_NULL:
            buf_append(b, "null");
            break;
        case NG_VAL_BOOL:
            buf_append(b, val->as.boolean ? "true" : "false");
            break;
        case NG_VAL_NUMBER: {
            char num_str[64];
            if (val->as.number == (long long)val->as.number) {
                snprintf(num_str, sizeof(num_str), "%lld", (long long)val->as.number);
            } else {
                snprintf(num_str, sizeof(num_str), "%g", val->as.number);
            }
            buf_append(b, num_str);
            break;
        }
        case NG_VAL_STRING:
            buf_append_escaped_string(b, val->as.string);
            break;
        case NG_VAL_TABLE: {
            NGTable* tbl = val->as.table;
            if (!tbl) {
                buf_append(b, "{}");
                break;
            }
            /* Verifica se e array */
            NGValue is_arr;
            bool is_array = false;
            if (ng_table_get(tbl, "_is_array", &is_arr) && is_arr.type == NG_VAL_BOOL && is_arr.as.boolean) {
                is_array = true;
            }

            if (is_array) {
                buf_append(b, "[");
                NGValue len_val;
                size_t arr_len = 0;
                if (ng_table_get(tbl, "length", &len_val) && len_val.type == NG_VAL_NUMBER) {
                    arr_len = (size_t)len_val.as.number;
                }
                for (size_t i = 0; i < arr_len; i++) {
                    if (i > 0) buf_append(b, ",");
                    char k[32];
                    snprintf(k, sizeof(k), "%zu", i);
                    NGValue item;
                    if (ng_table_get(tbl, k, &item)) {
                        stringify_value(b, &item);
                    } else {
                        buf_append(b, "null");
                    }
                }
                buf_append(b, "]");
            } else {
                buf_append(b, "{");
                bool first = true;
                for (size_t i = 0; i < tbl->size; i++) {
                    NGEntry* entry = tbl->buckets[i];
                    while (entry) {
                        /* Ignora campos internos que comecam com _ */
                        if (entry->key && entry->key[0] != '_') {
                            if (!first) buf_append(b, ",");
                            first = false;
                            buf_append_escaped_string(b, entry->key);
                            buf_append(b, ":");
                            stringify_value(b, &entry->value);
                        }
                        entry = entry->next;
                    }
                }
                buf_append(b, "}");
            }
            break;
        }
        default:
            buf_append(b, "null");
            break;
    }
}

char* ng_json_stringify(const NGValue* val) {
    StrBuf b;
    buf_init(&b);
    stringify_value(&b, val);
    return b.buf;
}

char* ng_json_pretty(const char* json) {
    if (!json) return strdup("");
    StrBuf out;
    buf_init(&out);

    int indent = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char* p = json; *p; p++) {
        char c = *p;

        if (in_string) {
            char temp[2] = { c, '\0' };
            buf_append(&out, temp);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            buf_append(&out, "\"");
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

        if (c == '{' || c == '[') {
            buf_append(&out, c == '{' ? "{" : "[");
            indent++;
            buf_append(&out, "\n");
            for (int i = 0; i < indent; i++) buf_append(&out, "  ");
        } else if (c == '}' || c == ']') {
            indent--;
            if (indent < 0) indent = 0;
            buf_append(&out, "\n");
            for (int i = 0; i < indent; i++) buf_append(&out, "  ");
            buf_append(&out, c == '}' ? "}" : "]");
        } else if (c == ',') {
            buf_append(&out, ",\n");
            for (int i = 0; i < indent; i++) buf_append(&out, "  ");
        } else if (c == ':') {
            buf_append(&out, ": ");
        } else {
            char temp[2] = { c, '\0' };
            buf_append(&out, temp);
        }
    }
    return out.buf;
}

/* Modulo nativo JSON para a NG */

static NGValue native_json_parse(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1 || args[0].type != NG_VAL_STRING) {
        return ng_val_null();
    }
    return ng_json_parse(args[0].as.string);
}

static NGValue native_json_stringify(NGInterpreter* interp, NGValue* args, size_t arg_count) {
    (void)interp;
    if (arg_count < 1) {
        return ng_val_string("null");
    }
    char* str = ng_json_stringify(&args[0]);
    NGValue res = ng_val_string(str ? str : "null");
    if (str) free(str);
    return res;
}

static NGValue native_json_read_file(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_null();
    FILE* f = fopen(args[0].as.string, "rb");
    if (!f) {
        ng_error_print(NG_ERR_FILE_NOT_FOUND, "Arquivo JSON Inexistente",
                       args[0].as.string, 0, 0,
                       "Nao foi possivel abrir o arquivo JSON informado.",
                       "Verifique o caminho e as permissoes de leitura.");
        return ng_val_null();
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;
    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return ng_val_null();
    }
    size_t read = fread(buffer, 1, (size_t)size, f);
    buffer[read] = '\0';
    fclose(f);
    NGValue result = ng_json_parse(buffer);
    free(buffer);
    return result;
}

static NGValue native_json_write_file(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    char* json = ng_json_stringify(&args[1]);
    FILE* f = fopen(args[0].as.string, "wb");
    if (!f) {
        free(json);
        ng_error_print(NG_ERR_FILE_NOT_FOUND, "Falha ao Gravar JSON",
                       args[0].as.string, 0, 0,
                       "Nao foi possivel criar o arquivo de saida.",
                       "Verifique se o diretorio existe e e gravavel.");
        return ng_val_bool(false);
    }
    fwrite(json ? json : "null", 1, strlen(json ? json : "null"), f);
    fclose(f);
    free(json);
    return ng_val_bool(true);
}

static NGValue native_json_validate(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_bool(false);
    const char* text = args[0].as.string;
    while (*text && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')) text++;
    if (!*text) return ng_val_bool(false);
    NGValue parsed = ng_json_parse(text);
    bool valid = !(parsed.type == NG_VAL_NULL && strcmp(text, "null") != 0);
    ng_val_free(&parsed);
    return ng_val_bool(valid);
}

static NGValue native_json_get(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[1].type != NG_VAL_STRING) return ng_val_null();
    NGValue value;
    if (args[0].type == NG_VAL_TABLE && args[0].as.table &&
        ng_table_get(args[0].as.table, args[1].as.string, &value)) {
        return ng_val_clone(&value);
    }
    return ng_val_null();
}

static NGValue native_json_set(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 3 || args[1].type != NG_VAL_STRING ||
        args[0].type != NG_VAL_TABLE || !args[0].as.table) {
        return ng_val_null();
    }
    ng_table_set(args[0].as.table, args[1].as.string, ng_val_clone(&args[2]));
    return ng_val_clone(&args[0]);
}

static NGValue native_json_remove(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 2 || args[1].type != NG_VAL_STRING ||
        args[0].type != NG_VAL_TABLE || !args[0].as.table) {
        return ng_val_bool(false);
    }
    return ng_val_bool(ng_table_unset(args[0].as.table, args[1].as.string));
}

static NGValue native_json_keys(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    NGTable* keys = ng_array_new();
    if (argc < 1 || args[0].type != NG_VAL_TABLE || !args[0].as.table) {
        return ng_val_table(keys);
    }
    NGTable* table = args[0].as.table;
    for (size_t i = 0; i < table->size; i++) {
        NGEntry* entry = table->buckets[i];
        while (entry) {
            if (entry->key && entry->key[0] != '_') {
                ng_array_push(keys, ng_val_string(entry->key));
            }
            entry = entry->next;
        }
    }
    return ng_val_table(keys);
}

static NGValue native_json_type(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1 || args[0].type != NG_VAL_STRING) return ng_val_string("invalido");
    const char* text = args[0].as.string;
    while (*text && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')) text++;
    if (*text == '{') return ng_val_string("objeto");
    if (*text == '[') return ng_val_string("array");
    if (*text == '"') return ng_val_string("string");
    if (*text == 't' || *text == 'f') return ng_val_string("booleano");
    if (*text == 'n') return ng_val_string("nulo");
    if (*text == '-' || (*text >= '0' && *text <= '9')) return ng_val_string("numero");
    return ng_val_string("invalido");
}

static NGValue native_json_pretty(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    char* compact = (argc >= 1) ? ng_json_stringify(&args[0]) : strdup("null");
    char* pretty = ng_json_pretty(compact ? compact : "null");
    NGValue result = ng_val_string(pretty ? pretty : "");
    free(compact);
    free(pretty);
    return result;
}

static NGValue native_json_size(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1) return ng_val_number(0);
    if (ng_value_is_array(&args[0])) return ng_val_number((double)ng_array_length(&args[0]));
    if (args[0].type == NG_VAL_TABLE && args[0].as.table) {
        return ng_val_number((double)args[0].as.table->count);
    }
    return ng_val_number(0);
}

static NGValue native_json_copy(NGInterpreter* interp, NGValue* args, size_t argc) {
    (void)interp;
    if (argc < 1) return ng_val_null();
    return ng_val_clone(&args[0]);
}

void ng_json_module_load(NGInterpreter* interp) {
    if (!interp || !interp->globals) return;
    NGTable* json_tbl = ng_table_new(32);
    ng_table_set(json_tbl, "Parse", ng_val_native_func("Parse", native_json_parse));
    ng_table_set(json_tbl, "Stringify", ng_val_native_func("Stringify", native_json_stringify));
    ng_table_set(json_tbl, "parse", ng_val_native_func("parse", native_json_parse));
    ng_table_set(json_tbl, "stringify", ng_val_native_func("stringify", native_json_stringify));
    ng_table_set(json_tbl, "Ler", ng_val_native_func("Ler", native_json_read_file));
    ng_table_set(json_tbl, "Gravar", ng_val_native_func("Gravar", native_json_write_file));
    ng_table_set(json_tbl, "Validar", ng_val_native_func("Validar", native_json_validate));
    ng_table_set(json_tbl, "Obter", ng_val_native_func("Obter", native_json_get));
    ng_table_set(json_tbl, "Definir", ng_val_native_func("Definir", native_json_set));
    ng_table_set(json_tbl, "Remover", ng_val_native_func("Remover", native_json_remove));
    ng_table_set(json_tbl, "Chaves", ng_val_native_func("Chaves", native_json_keys));
    ng_table_set(json_tbl, "Tipo", ng_val_native_func("Tipo", native_json_type));
    ng_table_set(json_tbl, "Formatar", ng_val_native_func("Formatar", native_json_pretty));
    ng_table_set(json_tbl, "Tamanho", ng_val_native_func("Tamanho", native_json_size));
    ng_table_set(json_tbl, "Copiar", ng_val_native_func("Copiar", native_json_copy));

    ng_env_set(interp->globals, "JSON", ng_val_table(json_tbl));
}
