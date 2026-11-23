#include "ng_value.h"
#include "ng_runtime.h"
#include "ng_outbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ng_val_print(const NGValue* val) {
    if (!val) {
        ng_outbuf_printf("null\n");
        return;
    }
    switch (val->type) {
        case NG_VAL_NULL:
            ng_outbuf_printf("null\n");
            break;
        case NG_VAL_BOOL:
            ng_outbuf_printf("%s\n", val->as.boolean ? "true" : "false");
            break;
        case NG_VAL_NUMBER:
            if (val->as.number == (long long)val->as.number) {
                ng_outbuf_printf("%lld\n", (long long)val->as.number);
            } else {
                ng_outbuf_printf("%g\n", val->as.number);
            }
            break;
        case NG_VAL_STRING:
            ng_outbuf_printf("%s\n", val->as.string ? val->as.string : "");
            break;
        case NG_VAL_TABLE: {
            NGTable* table = val->as.table;
            if (!table) {
                ng_outbuf_printf("<tabela>\n");
                break;
            }

            NGValue flag;
            bool is_array = ng_table_get(table, "_is_array", &flag) &&
                            flag.type == NG_VAL_BOOL && flag.as.boolean;

            if (is_array) {
                NGValue len_val;
                size_t length = 0;
                if (ng_table_get(table, "length", &len_val) && len_val.type == NG_VAL_NUMBER &&
                    len_val.as.number > 0) {
                    length = (size_t)len_val.as.number;
                }
                for (size_t i = 0; i < length; i++) {
                    char key[32];
                    NGValue item;
                    if (ng_table_get(table, ng_index_key((long long)i, key, sizeof(key)), &item)) {
                        ng_val_print(&item);
                    } else {
                        ng_outbuf_printf("null\n");
                    }
                }
                break;
            }

            ng_outbuf_printf("{\n");
            for (size_t i = 0; i < table->size; i++) {
                NGEntry* entry = table->buckets[i];
                while (entry) {
                    if (entry->key && entry->key[0] != '_') {
                        ng_outbuf_printf("  %s = ", entry->key);
                        ng_val_print(&entry->value);
                    }
                    entry = entry->next;
                }
            }
            ng_outbuf_printf("}\n");
            break;
        }
        case NG_VAL_FUNCTION:
            ng_outbuf_printf("<funcao %s>\n", val->as.func.name ? val->as.func.name : "anonima");
            break;
        case NG_VAL_NATIVE_FUNC:
            ng_outbuf_printf("<funcao_nativa %s>\n", val->as.native_func.name ? val->as.native_func.name : "anonima");
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
