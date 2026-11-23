#ifndef NG_JSON_H
#define NG_JSON_H

#include "ng_value.h"
#include "ng_interpreter.h"

/* Converte uma string JSON em um valor NG (Tabela, String, Numero, Bool, Nulo) */
NGValue ng_json_parse(const char* json_str);

/* Serializa um valor NG em uma string JSON formatada (deve ser liberada com free) */
char* ng_json_stringify(const NGValue* val);

/* Formata uma string JSON com indentacao (deve ser liberada com free). */
char* ng_json_pretty(const char* json);

/* Carrega o modulo nativo JSON no interpretador (@json e objeto global JSON) */
void ng_json_module_load(NGInterpreter* interp);

#endif /* NG_JSON_H */
