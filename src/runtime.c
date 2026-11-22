#include "ng_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const g_small_index_keys[10] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
};

const char* ng_index_key(long long index, char* buf, size_t buf_size) {
    if (index >= 0 && index < 10) {
        return g_small_index_keys[index];
    }
    snprintf(buf, buf_size, "%lld", index);
    return buf;
}

static unsigned long hash_str(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

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

void ng_table_free(NGTable* table) {
    ng_table_release(table);
}

bool ng_table_set(NGTable* restrict table, const char* key, NGValue value) {
    if (!table || !key) return false;
    if (table->size == 0 || !table->buckets) return false;
    unsigned long h = hash_str(key) % table->size;
    NGEntry* entry = table->buckets[h];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            ng_val_free(&entry->value);
            entry->value = value;
            return true;
        }
        entry = entry->next;
    }
    NGEntry* new_entry = (NGEntry*)malloc(sizeof(NGEntry));
    if (!new_entry) return false;
    new_entry->key = strdup(key);
    if (!new_entry->key) {
        free(new_entry);
        return false;
    }
    new_entry->value = value;
    new_entry->next = table->buckets[h];
    table->buckets[h] = new_entry;
    table->count++;
    return true;
}

bool ng_table_get(NGTable* restrict table, const char* key, NGValue* out_val) {
    if (!table || !key) return false;
    unsigned long h = hash_str(key) % table->size;
    NGEntry* entry = table->buckets[h];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            if (out_val) *out_val = entry->value;
            return true;
        }
        entry = entry->next;
    }
    return false;
}

bool ng_table_unset(NGTable* restrict table, const char* key) {
    if (!table || !key) return false;
    unsigned long h = hash_str(key) % table->size;
    NGEntry** link = &table->buckets[h];
    while (*link) {
        NGEntry* entry = *link;
        if (strcmp(entry->key, key) == 0) {
            *link = entry->next;
            free(entry->key);
            ng_val_free(&entry->value);
            free(entry);
            table->count--;
            return true;
        }
        link = &entry->next;
    }
    return false;
}

NGEnv* ng_env_new(NGEnv* parent) {
    NGEnv* env = (NGEnv*)malloc(sizeof(NGEnv));
    if (!env) return NULL;
    /* Evita alocar um NGTable separado so para copi-lo para dentro do env. */
    env->table.size = 16;
    env->table.count = 0;
    env->table.ref_count = 1;
    env->table.buckets = (NGEntry**)calloc(env->table.size, sizeof(NGEntry*));
    if (!env->table.buckets) {
        free(env);
        return NULL;
    }
    env->parent = parent;
    return env;
}

void ng_env_free(NGEnv* env) {
    if (!env) return;
    for (size_t i = 0; i < env->table.size; i++) {
        NGEntry* entry = env->table.buckets[i];
        while (entry) {
            NGEntry* next = entry->next;
            free(entry->key);
            ng_val_free(&entry->value);
            free(entry);
            entry = next;
        }
    }
    free(env->table.buckets);
    free(env);
}

bool ng_env_set(NGEnv* env, const char* key, NGValue value) {
    if (!env) return false;
    return ng_table_set(&env->table, key, value);
}

void ng_env_assign(NGEnv* env, const char* key, NGValue value) {
    if (!env || !key) {
        ng_val_free(&value);
        return;
    }
    /* Uma unica passagem pela cadeia de escopos: no acerto substitui direto,
       evitando duplo hash/strcmp de ng_env_has + ng_env_set. */
    for (NGEnv* scope = env; scope; scope = scope->parent) {
        unsigned long h = hash_str(key) % scope->table.size;
        for (NGEntry* entry = scope->table.buckets[h]; entry; entry = entry->next) {
            if (strcmp(entry->key, key) == 0) {
                ng_val_free(&entry->value);
                entry->value = value;
                return;
            }
        }
    }
    ng_table_set(&env->table, key, value);
}

bool ng_env_get(NGEnv* env, const char* key, NGValue* out_val) {
    NGEnv* curr = env;
    while (curr) {
        if (ng_table_get(&curr->table, key, out_val)) {
            return true;
        }
        curr = curr->parent;
    }
    return false;
}

bool ng_env_unset(NGEnv* env, const char* key) {
    if (!env || !key) return false;
    unsigned long h = hash_str(key) % env->table.size;
    NGEntry** link = &env->table.buckets[h];
    while (*link) {
        NGEntry* entry = *link;
        if (strcmp(entry->key, key) == 0) {
            *link = entry->next;
            free(entry->key);
            ng_val_free(&entry->value);
            free(entry);
            env->table.count--;
            return true;
        }
        link = &entry->next;
    }
    return false;
}
