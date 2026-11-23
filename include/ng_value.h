#ifndef NG_VALUE_H
#define NG_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    NG_VAL_NULL,
    NG_VAL_BOOL,
    NG_VAL_NUMBER,
    NG_VAL_STRING,
    NG_VAL_TABLE,
    NG_VAL_FUNCTION,
    NG_VAL_NATIVE_FUNC
} NGValueType;

struct NGTable;
struct NGAstNode;
struct NGInterpreter;
typedef struct NGValue NGValue;

struct NGValue {
    NGValueType type;
    union {
        bool boolean;
        double number;
        char* string;
        struct NGTable* table;
        struct {
            char* name;
            char** params;
            size_t param_count;
            struct NGAstNode* body;
        } func;
        struct {
            char* name;
            NGValue (*fn)(struct NGInterpreter*, NGValue*, size_t);
        } native_func;
    } as;
};

/* Os metodos quentes ficam como 'static inline' nos headers (C11) para que
   interpretador/runtime/stdlib os utilizem com codigo especializado no proprio
   TU, evitando overhead de chamada cross-TU (o build nao usa LTO). */
void ng_table_retain(struct NGTable* table);
void ng_table_release(struct NGTable* table);

static inline NGValue ng_val_null(void) {
    NGValue v;
    v.type = NG_VAL_NULL;
    return v;
}

static inline NGValue ng_val_bool(bool b) {
    NGValue v;
    v.type = NG_VAL_BOOL;
    v.as.boolean = b;
    return v;
}

static inline NGValue ng_val_number(double n) {
    NGValue v;
    v.type = NG_VAL_NUMBER;
    v.as.number = n;
    return v;
}

static inline NGValue ng_val_string(const char* s) {
    NGValue v;
    v.type = NG_VAL_STRING;
    v.as.string = s ? strdup(s) : strdup("");
    return v;
}

static inline NGValue ng_val_string_take(char* s) {
    NGValue v;
    v.type = NG_VAL_STRING;
    v.as.string = s ? s : strdup("");
    return v;
}

static inline NGValue ng_val_table(struct NGTable* t) {
    NGValue v;
    v.type = NG_VAL_TABLE;
    v.as.table = t;
    return v;
}

static inline NGValue ng_val_native_func(const char* name, NGValue (*fn)(struct NGInterpreter*, NGValue*, size_t)) {
    NGValue v;
    v.type = NG_VAL_NATIVE_FUNC;
    v.as.native_func.name = name ? strdup(name) : strdup("<anonima>");
    v.as.native_func.fn = fn;
    return v;
}

static inline NGValue ng_val_clone(const NGValue* val) {
    if (!val) return ng_val_null();
    NGValue copy = *val;
    if (val->type == NG_VAL_STRING && val->as.string) {
        copy.as.string = strdup(val->as.string);
    } else if (val->type == NG_VAL_TABLE && val->as.table) {
        ng_table_retain(val->as.table);
    } else if (val->type == NG_VAL_FUNCTION) {
        copy.as.func.name = val->as.func.name ? strdup(val->as.func.name) : NULL;
        copy.as.func.param_count = val->as.func.param_count;
        if (val->as.func.param_count > 0) {
            copy.as.func.params = (char**)malloc(sizeof(char*) * val->as.func.param_count);
            for (size_t i = 0; i < val->as.func.param_count; i++) {
                copy.as.func.params[i] = strdup(val->as.func.params[i]);
            }
        } else {
            copy.as.func.params = NULL;
        }
        copy.as.func.body = val->as.func.body;
    } else if (val->type == NG_VAL_NATIVE_FUNC) {
        copy.as.native_func.name = val->as.native_func.name ? strdup(val->as.native_func.name) : NULL;
        copy.as.native_func.fn = val->as.native_func.fn;
    }
    return copy;
}

static inline void ng_val_free(NGValue* val) {
    if (!val) return;
    if (val->type == NG_VAL_STRING && val->as.string) {
        free(val->as.string);
        val->as.string = NULL;
    } else if (val->type == NG_VAL_TABLE) {
        if (val->as.table) {
            ng_table_release(val->as.table);
            val->as.table = NULL;
        }
    } else if (val->type == NG_VAL_FUNCTION) {
        if (val->as.func.name) {
            free(val->as.func.name);
            val->as.func.name = NULL;
        }
        if (val->as.func.params) {
            for (size_t i = 0; i < val->as.func.param_count; i++) {
                if (val->as.func.params[i]) {
                    free(val->as.func.params[i]);
                }
            }
            free(val->as.func.params);
            val->as.func.params = NULL;
        }
    } else if (val->type == NG_VAL_NATIVE_FUNC) {
        if (val->as.native_func.name) {
            free(val->as.native_func.name);
            val->as.native_func.name = NULL;
        }
    }
}

void ng_val_print(const NGValue* val);
char* ng_val_to_string(const NGValue* val);

#endif // NG_VALUE_H
