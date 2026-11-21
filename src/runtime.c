#include "ng_runtime.h"
#include <stdlib.h>
#include <string.h>

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
    t->size = initial_size > 0 ? initial_size : 16;
    t->count = 0;
    t->buckets = (NGEntry**)calloc(t->size, sizeof(NGEntry*));
    return t;
}

void ng_table_free(NGTable* table) {
    if (!table) return;
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

bool ng_table_set(NGTable* table, const char* key, NGValue value) {
    if (!table || !key) return false;
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
    new_entry->key = strdup(key);
    new_entry->value = value;
    new_entry->next = table->buckets[h];
    table->buckets[h] = new_entry;
    table->count++;
    return true;
}

bool ng_table_get(NGTable* table, const char* key, NGValue* out_val) {
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

bool ng_table_unset(NGTable* table, const char* key) {
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
    return ng_table_set(&env->table, key, value);
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
