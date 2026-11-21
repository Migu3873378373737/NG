#ifndef NG_COLLECTION_H
#define NG_COLLECTION_H

#include "ng_runtime.h"
#include <stdbool.h>
#include <stddef.h>

/* Cria um array NG compativel com Loop, Print e JSON. */
NGTable* ng_array_new(void);
/* Adiciona um elemento ao array (assume a propriedade de value). */
void ng_array_push(NGTable* array, NGValue value);
size_t ng_array_length(const NGValue* array);
bool ng_value_is_array(const NGValue* value);
/* Le um elemento do array (devolve copia). */
NGValue ng_array_get(const NGValue* array, size_t index);

/* Cria um registro/linha (tabela comum com campos nomeados). */
NGTable* ng_row_new(void);

#endif /* NG_COLLECTION_H */
