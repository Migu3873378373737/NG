#include "ng_runtime.h"
#include <stdlib.h>
#include <string.h>

NGTable* ng_table_new(size_t initial_size) {
    NGTable* t = (NGTable*)malloc(sizeof(NGTable));
    if (!t) return NULL;
    t->size = initial_size > 0 ? initial_size : 16;
    t->count = 0;
    t->ref_count = 1;
    t->buckets = (NGEntry**)calloc(t->size, sizeof(NGEntry*));
    if (!t->buckets) {
        free(t);
        return NULL;
    }
    return t;
}

void ng_table_free(NGTable* table) {
    ng_table_release(table);
}

void ng_table_retain(NGTable* table) {
    if (table) table->ref_count++;
}

void ng_table_release(NGTable* table) {
    if (!table || table->ref_count == 0) return;
    if (table->ref_count > 1) {
        table->ref_count--;
        return;
    }
    /* Marca como destruida antes de liberar os campos, protegendo contra
       ciclos acidentais (tabela contendo a si mesma). */
    table->ref_count = 0;
    for (size_t i = 0; i < table->size; i++) {
        NGEntry* entry = table->buckets[i];
        while (entry) {
            NGEntry* next = entry->next;
            free(entry->key);
            ng_val_free(&entry->value);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}
