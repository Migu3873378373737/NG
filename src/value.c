#include "ng_value.h"
#include "ng_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NGValue ng_val_null(void) {
    NGValue v;
    v.type = NG_VAL_NULL;
    return v;
}

NGValue ng_val_bool(bool b) {
    NGValue v;
    v.type = NG_VAL_BOOL;
    v.as.boolean = b;
    return v;
}

NGValue ng_val_number(double n) {
    NGValue v;
    v.type = NG_VAL_NUMBER;
    v.as.number = n;
    return v;
}

NGValue ng_val_string(const char* s) {
    NGValue v;
    v.type = NG_VAL_STRING;
    v.as.string = s ? strdup(s) : strdup("");
    return v;
}

NGValue ng_val_table(struct NGTable* t) {
    NGValue v;
    v.type = NG_VAL_TABLE;
    v.as.table = t;
    return v;
}

NGValue ng_val_native_func(const char* name, NGValue (*fn)(struct NGInterpreter*, NGValue*, size_t)) {
    NGValue v;
    v.type = NG_VAL_NATIVE_FUNC;
    v.as.native_func.name = name ? strdup(name) : strdup("<anonima>");
    v.as.native_func.fn = fn;
    return v;
}

NGValue ng_val_clone(const NGValue* val) {
    if (!val) return ng_val_null();
    NGValue copy = *val;
    if (val->type == NG_VAL_STRING && val->as.string) {
        copy.as.string = strdup(val->as.string);
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

void ng_val_free(NGValue* val) {
    if (!val) return;
    if (val->type == NG_VAL_STRING && val->as.string) {
        free(val->as.string);
        val->as.string = NULL;
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

void ng_val_print(const NGValue* val) {
    if (!val) {
        printf("null\n");
        return;
    }
    switch (val->type) {
        case NG_VAL_NULL:
            printf("null\n");
            break;
        case NG_VAL_BOOL:
            printf("%s\n", val->as.boolean ? "true" : "false");
            break;
        case NG_VAL_NUMBER:
            if (val->as.number == (long long)val->as.number) {
                printf("%lld\n", (long long)val->as.number);
            } else {
                printf("%g\n", val->as.number);
            }
            break;
        case NG_VAL_STRING:
            printf("%s\n", val->as.string ? val->as.string : "");
            break;
        case NG_VAL_TABLE: {
            NGTable* table = val->as.table;
            if (!table) {
                printf("<tabela>\n");
                break;
            }

            NGValue flag;
            bool is_array = ng_table_get(table, "_is_array", &flag) &&
                            flag.type == NG_VAL_BOOL && flag.as.boolean;

            if (is_array) {
                NGValue len_val;
                size_t length = 0;
                if (ng_table_get(table, "length", &len_val) && len_val.type == NG_VAL_NUMBER) {
                    length = (size_t)len_val.as.number;
                }
                for (size_t i = 0; i < length; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "%zu", i);
                    NGValue item;
                    if (ng_table_get(table, key, &item)) {
                        ng_val_print(&item);
                    } else {
                        printf("null\n");
                    }
                }
                break;
            }

            printf("{\n");
            for (size_t i = 0; i < table->size; i++) {
                NGEntry* entry = table->buckets[i];
                while (entry) {
                    if (entry->key && entry->key[0] != '_') {
                        printf("  %s = ", entry->key);
                        ng_val_print(&entry->value);
                    }
                    entry = entry->next;
                }
            }
            printf("}\n");
            break;
        }
        case NG_VAL_FUNCTION:
            printf("<funcao %s>\n", val->as.func.name ? val->as.func.name : "anonima");
            break;
        case NG_VAL_NATIVE_FUNC:
            printf("<funcao_nativa %s>\n", val->as.native_func.name ? val->as.native_func.name : "anonima");
            break;
    }
}

char* ng_val_to_string(const NGValue* val) {
    if (!val) return strdup("null");
    char buf[128];
    switch (val->type) {
        case NG_VAL_NULL:
            return strdup("null");
        case NG_VAL_BOOL:
            return strdup(val->as.boolean ? "true" : "false");
        case NG_VAL_NUMBER:
            if (val->as.number == (long long)val->as.number) {
                snprintf(buf, sizeof(buf), "%lld", (long long)val->as.number);
            } else {
                snprintf(buf, sizeof(buf), "%g", val->as.number);
            }
            return strdup(buf);
        case NG_VAL_STRING:
            return strdup(val->as.string ? val->as.string : "");
        case NG_VAL_TABLE:
            return strdup("<tabela>");
        case NG_VAL_FUNCTION:
            snprintf(buf, sizeof(buf), "<funcao %s>", val->as.func.name ? val->as.func.name : "anonima");
            return strdup(buf);
        case NG_VAL_NATIVE_FUNC:
            snprintf(buf, sizeof(buf), "<funcao_nativa %s>", val->as.native_func.name ? val->as.native_func.name : "anonima");
            return strdup(buf);
        default:
            return strdup("?");
    }
}
