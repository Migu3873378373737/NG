#ifndef NG_VALUE_H
#define NG_VALUE_H

#include <stdbool.h>
#include <stddef.h>

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

NGValue ng_val_null(void);
NGValue ng_val_bool(bool b);
NGValue ng_val_number(double n);
NGValue ng_val_string(const char* s);
/* Assume a posse de s (que pode ser NULL). Evita uma copia adicional. */
NGValue ng_val_string_take(char* s);
NGValue ng_val_table(struct NGTable* t);
NGValue ng_val_native_func(const char* name, NGValue (*fn)(struct NGInterpreter*, NGValue*, size_t));
NGValue ng_val_clone(const NGValue* val);
void ng_val_free(NGValue* val);
void ng_val_print(const NGValue* val);
char* ng_val_to_string(const NGValue* val);

#endif // NG_VALUE_H
