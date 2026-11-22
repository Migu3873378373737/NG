#ifndef NG_RUNTIME_H
#define NG_RUNTIME_H

#include "ng_value.h"

typedef struct NGEntry {
    char* key;
    NGValue value;
    struct NGEntry* next;
} NGEntry;

typedef struct NGTable {
    NGEntry** buckets;
    size_t size;
    size_t count;
    /* Compartilhamento seguro: valores de tabela sao referencias. */
    size_t ref_count;
} NGTable;

typedef struct NGEnv {
    NGTable table;
    struct NGEnv* parent;
} NGEnv;

NGTable* ng_table_new(size_t initial_size);
/* Libera uma referencia. A tabela e destruida quando a ultima sai. */
void ng_table_free(NGTable* table);
void ng_table_retain(NGTable* table);
void ng_table_release(NGTable* table);
bool ng_table_set(NGTable* table, const char* key, NGValue value);
bool ng_table_get(NGTable* table, const char* key, NGValue* out_val);
bool ng_table_unset(NGTable* table, const char* key);

NGEnv* ng_env_new(NGEnv* parent);
void ng_env_free(NGEnv* env);
bool ng_env_set(NGEnv* env, const char* key, NGValue value);
/* Atualiza a variavel no escopo onde ela ja existe; se nao existir,
   cria no escopo atual. */
void ng_env_assign(NGEnv* env, const char* key, NGValue value);
bool ng_env_get(NGEnv* env, const char* key, NGValue* out_val);
bool ng_env_unset(NGEnv* env, const char* key);

#endif // NG_RUNTIME_H
