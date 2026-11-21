#include "ng_collection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NGTable* ng_array_new(void) {
    NGTable* array = ng_table_new(8);
    if (!array) return NULL;
    ng_table_set(array, "_is_array", ng_val_bool(true));
    ng_table_set(array, "length", ng_val_number(0));
    return array;
}

void ng_array_push(NGTable* array, NGValue value) {
    if (!array) {
        ng_val_free(&value);
        return;
    }
    NGValue len_val;
    size_t length = 0;
    if (ng_table_get(array, "length", &len_val) && len_val.type == NG_VAL_NUMBER) {
        length = (size_t)len_val.as.number;
    }
    char key[32];
    snprintf(key, sizeof(key), "%zu", length);
    ng_table_set(array, key, value); /* ng_table_set assume a propriedade */
    ng_table_set(array, "length", ng_val_number((double)(length + 1)));
}

size_t ng_array_length(const NGValue* array) {
    NGValue len_val;
    if (ng_value_is_array(array) &&
        ng_table_get(array->as.table, "length", &len_val) &&
        len_val.type == NG_VAL_NUMBER) {
        return (size_t)len_val.as.number;
    }
    return 0;
}

bool ng_value_is_array(const NGValue* value) {
    NGValue flag;
    return value && value->type == NG_VAL_TABLE && value->as.table &&
           ng_table_get(value->as.table, "_is_array", &flag) &&
           flag.type == NG_VAL_BOOL && flag.as.boolean;
}

NGValue ng_array_get(const NGValue* array, size_t index) {
    if (!ng_value_is_array(array)) return ng_val_null();
    char key[32];
    snprintf(key, sizeof(key), "%zu", index);
    NGValue item;
    if (ng_table_get(array->as.table, key, &item)) {
        return ng_val_clone(&item);
    }
    return ng_val_null();
}

NGTable* ng_row_new(void) {
    return ng_table_new(8);
}
