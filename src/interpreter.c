#include <math.h>
#include "ng_interpreter.h"
#include "ng_error.h"
#include "ng_lexer.h"
#include "ng_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ng_interp_init(NGInterpreter* interp, const char* filename, bool is_subinterpreter) {
    interp->globals = ng_env_new(NULL);
    interp->current_env = interp->globals;
    interp->reg_y = ng_val_null();
    interp->current_file = filename;
    interp->is_subinterpreter = is_subinterpreter;
    interp->receiver_table = NULL;
}

void ng_interp_cleanup(NGInterpreter* interp) {
    if (interp->globals) {
        ng_env_free(interp->globals);
        interp->globals = NULL;
    }
    ng_val_free(&interp->reg_y);
}

static NGValue eval_expression(NGInterpreter* interp, NGAstNode* node);

static bool ng_value_is_array(const NGValue* value) {
    NGValue flag;
    return value && value->type == NG_VAL_TABLE && value->as.table &&
           ng_table_get(value->as.table, "_is_array", &flag) &&
           flag.type == NG_VAL_BOOL && flag.as.boolean;
}

static bool ng_value_truthy(const NGValue* value) {
    if (!value) return false;
    switch (value->type) {
        case NG_VAL_NULL:    return false;
        case NG_VAL_BOOL:    return value->as.boolean;
        case NG_VAL_NUMBER:  return value->as.number != 0.0;
        case NG_VAL_STRING:  return value->as.string && value->as.string[0] != '\0';
        case NG_VAL_TABLE:   return value->as.table && value->as.table->count > 0;
        case NG_VAL_FUNCTION:
        case NG_VAL_NATIVE_FUNC:
            return true;
    }
    return false;
}

static size_t ng_array_length(const NGValue* array) {
    NGValue len_val;
    if (array && array->type == NG_VAL_TABLE && array->as.table &&
        ng_table_get(array->as.table, "length", &len_val) &&
        len_val.type == NG_VAL_NUMBER && len_val.as.number > 0) {
        return (size_t)len_val.as.number;
    }
    return 0;
}

NGValue ng_interp_call_func(NGInterpreter* interp, const NGValue* func, NGValue* args, size_t arg_count) {
    if (!interp || !func) return ng_val_null();

    if (func->type == NG_VAL_NATIVE_FUNC) {
        if (func->as.native_func.fn) {
            return func->as.native_func.fn(interp, args, arg_count);
        }
        return ng_val_null();
    }

    if (func->type == NG_VAL_FUNCTION) {
        NGEnv* local_env = ng_env_new(interp->globals);
        for (size_t i = 0; i < arg_count && i < func->as.func.param_count; i++) {
            ng_env_set(local_env, func->as.func.params[i], ng_val_clone(&args[i]));
        }
        NGEnv* prev_env = interp->current_env;
        interp->current_env = local_env;
        NGValue ret = ng_val_null();
        /* Retorno deterministico: uma funcao sem Y definido retorna null,
           em vez de herdar o valor de Y de uma chamada anterior. */
        ng_val_free(&interp->reg_y);
        interp->reg_y = ng_val_null();
        if (func->as.func.body) {
            ng_interp_run(interp, func->as.func.body);
            ret = ng_val_clone(&interp->reg_y);
        }
        interp->current_env = prev_env;
        ng_env_free(local_env);
        return ret;
    }

    return ng_val_null();
}

static bool execute_statement(NGInterpreter* interp, NGAstNode* node) {
    if (!node) return true;

    switch (node->type) {
        case NG_AST_STMT_PRINT: {
            NGValue v = eval_expression(interp, node->as.print_stmt.expr);
            ng_val_print(&v);
            ng_val_free(&v);
            return true;
        }

        case NG_AST_STMT_Y: {
            ng_val_free(&interp->reg_y);
            interp->reg_y = eval_expression(interp, node->as.y_stmt.expr);
            return true;
        }

        case NG_AST_STMT_G: {
            if (node->as.g_stmt.expr != NULL) {
                NGValue v = eval_expression(interp, node->as.g_stmt.expr);
                ng_val_print(&v);
                ng_val_free(&v);
            } else {
                ng_val_print(&interp->reg_y);
            }
            return true;
        }

        case NG_AST_STMT_ASSIGN: {
            NGValue v = eval_expression(interp, node->as.assign_stmt.value);
            /* Atualiza o escopo onde a variavel ja existe (ex.: global dentro
               de uma funcao); caso contrario, cria no escopo atual. */
            ng_env_assign(interp->current_env, node->as.assign_stmt.name, v);
            return true;
        }

        case NG_AST_STMT_TABLE_SET: {
            /* Resolve the table object first */
            NGValue tbl_val;
            bool tbl_found = false;
            bool owns_table = false;
            if (node->as.table_set.table &&
                node->as.table_set.table->type == NG_AST_EXPR_IDENTIFIER) {
                tbl_found = ng_env_get(interp->current_env,
                                       node->as.table_set.table->as.id_expr.name,
                                       &tbl_val);
            } else {
                tbl_val = eval_expression(interp, node->as.table_set.table);
                tbl_found = true;
                owns_table = true;
            }
            if (!tbl_found || tbl_val.type != NG_VAL_TABLE || !tbl_val.as.table) {
                if (owns_table) ng_val_free(&tbl_val);
                return true;
            }
            NGValue field_val = eval_expression(interp, node->as.table_set.value);
            ng_table_set(tbl_val.as.table, node->as.table_set.field, field_val);
            if (owns_table) ng_val_free(&tbl_val);
            return true;
        }

        case NG_AST_STMT_IF: {
            NGValue cond = eval_expression(interp, node->as.if_stmt.condition);
            bool is_true = ng_value_truthy(&cond);
            ng_val_free(&cond);

            if (is_true) {
                if (node->as.if_stmt.then_block) {
                    return ng_interp_run(interp, node->as.if_stmt.then_block);
                }
            } else if (node->as.if_stmt.else_block) {
                return ng_interp_run(interp, node->as.if_stmt.else_block);
            }
            if (!is_true && getenv("NG_DIAG")) {
                fprintf(stderr, "[NG-407] Diagnostico: condicao falsa; bloco Se nao executado.\n"
                                "Sugestao: verifique a condicao ou use Senao.\n");
            }
            return true;
        }

        case NG_AST_STMT_WHILE: {
            while (true) {
                NGValue cond = eval_expression(interp, node->as.while_stmt.condition);
                bool is_true = ng_value_truthy(&cond);
                ng_val_free(&cond);
                
                if (!is_true) break;

                if (!ng_interp_run(interp, node->as.while_stmt.block)) {
                    return false;
                }
            }
            return true;
        }

        case NG_AST_STMT_LOOP: {
            NGValue collection = eval_expression(interp, node->as.loop_stmt.iterable);

            if (!ng_value_is_array(&collection)) {
                ng_error_print(NG_ERR_TYPE_MISMATCH, "Colecao Invalida no Loop",
                               interp->current_file, node->line, node->column,
                               "O comando 'Loop' espera uma lista/criacao de colecao com '[' e ']'.",
                               "Use uma lista, por exemplo: Nomes = [\"Ana\", \"Joao\"].");
                ng_val_free(&collection);
                return true;
            }

            const char* var_name = node->as.loop_stmt.var_name;
            NGValue previous_value;
            bool had_previous = ng_env_get(interp->current_env, var_name, &previous_value);
            NGValue saved_value = had_previous ? ng_val_clone(&previous_value) : ng_val_null();

            size_t length = ng_array_length(&collection);
            if (length == 0 && getenv("NG_DIAG")) {
                fprintf(stderr, "[NG-407] Diagnostico: lista vazia; bloco Loop nao sera executado.\n"
                                "Sugestao: verifique a colecao antes de iterar.\n");
            }
            for (size_t i = 0; i < length; i++) {
                char key[32];
                snprintf(key, sizeof(key), "%zu", i);

                NGValue item;
                if (!ng_table_get(collection.as.table, key, &item)) {
                    item = ng_val_null();
                }
                ng_env_set(interp->current_env, var_name, ng_val_clone(&item));

                if (!ng_interp_run(interp, node->as.loop_stmt.body)) {
                    if (had_previous) {
                        ng_env_set(interp->current_env, var_name, saved_value);
                        saved_value = ng_val_null();
                    } else {
                        ng_env_unset(interp->current_env, var_name);
                    }
                    ng_val_free(&saved_value);
                    ng_val_free(&collection);
                    return false;
                }
            }

            if (had_previous) {
                ng_env_set(interp->current_env, var_name, saved_value);
                saved_value = ng_val_null(); /* propriedade transferida para o env */
            } else {
                ng_env_unset(interp->current_env, var_name);
            }
            ng_val_free(&saved_value);
            ng_val_free(&collection);
            return true;
        }

        case NG_AST_STMT_FUNC: {
            NGValue f;
            f.type = NG_VAL_FUNCTION;
            f.as.func.name = strdup(node->as.func_stmt.name);
            f.as.func.param_count = node->as.func_stmt.param_count;
            if (f.as.func.param_count > 0) {
                f.as.func.params = (char**)malloc(sizeof(char*) * f.as.func.param_count);
                for (size_t i = 0; i < f.as.func.param_count; i++) {
                    f.as.func.params[i] = strdup(node->as.func_stmt.params[i]);
                }
            } else {
                f.as.func.params = NULL;
            }
            f.as.func.body = node->as.func_stmt.body;
            ng_env_set(interp->globals, node->as.func_stmt.name, f);
            return true;
        }

        case NG_AST_STMT_IMPORT: {
            return ng_module_load_into_env(node->as.import_stmt.module_name, interp);
        }

        case NG_AST_PROGRAM: {
            for (size_t i = 0; i < node->as.program.count; i++) {
                if (!execute_statement(interp, node->as.program.statements[i])) {
                    return false;
                }
            }
            return true;
        }

        default:
            eval_expression(interp, node);
            return true;
    }
}

static NGValue eval_expression(NGInterpreter* interp, NGAstNode* node) {
    if (!node) return ng_val_null();

    switch (node->type) {
        case NG_AST_EXPR_LITERAL:
            return ng_val_clone(&node->as.literal_expr.value);

        case NG_AST_EXPR_ARRAY: {
            size_t count = node->as.array_expr.count;
            NGTable* array = ng_table_new(count > 0 ? count : 8);
            ng_table_set(array, "_is_array", ng_val_bool(true));

            for (size_t i = 0; i < count; i++) {
                NGValue element = eval_expression(interp, node->as.array_expr.elements[i]);
                char key[32];
                snprintf(key, sizeof(key), "%zu", i);
                ng_table_set(array, key, element);
            }
            ng_table_set(array, "length", ng_val_number((double)count));
            return ng_val_table(array);
        }

        case NG_AST_EXPR_INDEX: {
            NGValue target = eval_expression(interp, node->as.index_expr.target);
            NGValue index = eval_expression(interp, node->as.index_expr.index);
            NGValue result = ng_val_null();

            if (target.type == NG_VAL_TABLE && target.as.table) {
                NGValue field;
                if (index.type == NG_VAL_NUMBER) {
                    char key[32];
                    snprintf(key, sizeof(key), "%lld", (long long)index.as.number);
                    if (ng_table_get(target.as.table, key, &field)) result = ng_val_clone(&field);
                } else if (index.type == NG_VAL_STRING && index.as.string) {
                    if (ng_table_get(target.as.table, index.as.string, &field)) result = ng_val_clone(&field);
                }
            } else if (target.type == NG_VAL_STRING && target.as.string &&
                       index.type == NG_VAL_NUMBER) {
                long long idx = (long long)index.as.number;
                size_t len = strlen(target.as.string);
                if (idx >= 0 && (size_t)idx < len) {
                    char ch[2] = { target.as.string[idx], '\0' };
                    result = ng_val_string(ch);
                }
            }
            ng_val_free(&target);
            ng_val_free(&index);
            return result;
        }

        case NG_AST_EXPR_IDENTIFIER: {
            if (strcmp(node->as.id_expr.name, "Y") == 0) {
                return ng_val_clone(&interp->reg_y);
            }
            NGValue out;
            if (ng_env_get(interp->current_env, node->as.id_expr.name, &out)) {
                return ng_val_clone(&out);
            }
            ng_error_print(NG_ERR_UNDEFINED_VAR, "Variavel Nao Definida", interp->current_file, node->line, node->column,
                           "A variavel referenciada nao foi declarada anteriormente.", "Certifique-se de atribuir um valor antes do uso.");
            return ng_val_null();
        }

        case NG_AST_EXPR_BINARY: {
            NGValue l = eval_expression(interp, node->as.binary_expr.left);
            NGValue r = eval_expression(interp, node->as.binary_expr.right);
            NGValue res = ng_val_null();

            if (l.type == NG_VAL_NUMBER && r.type == NG_VAL_NUMBER) {
                switch (node->as.binary_expr.op) {
                    case NG_TOK_PLUS:     res = ng_val_number(l.as.number + r.as.number); break;
                    case NG_TOK_MINUS:    res = ng_val_number(l.as.number - r.as.number); break;
                    case NG_TOK_STAR:     res = ng_val_number(l.as.number * r.as.number); break;
                    case NG_TOK_PERCENT:
                        if (r.as.number == 0) {
                            if (getenv("NG_DIAG")) {
                                fprintf(stderr, "[NG-408] Diagnostico: modulo por zero; resultado substituido por 0.\n");
                            }
                            res = ng_val_number(0);
                        } else {
                            res = ng_val_number(fmod(l.as.number, r.as.number));
                        }
                        break;
                    case NG_TOK_SLASH:
                        if (r.as.number == 0) {
                            if (getenv("NG_DIAG")) {
                                fprintf(stderr, "[NG-407] Diagnostico: divisao por zero; resultado substituido por 0.\n"
                                                "Sugestao: verifique o denominador antes de dividir.\n");
                            }
                            res = ng_val_number(0);
                        } else {
                            res = ng_val_number(l.as.number / r.as.number);
                        }
                        break;
                    case NG_TOK_EQ:       res = ng_val_bool(l.as.number == r.as.number); break;
                    case NG_TOK_NEQ:      res = ng_val_bool(l.as.number != r.as.number); break;
                    case NG_TOK_LT:       res = ng_val_bool(l.as.number < r.as.number); break;
                    case NG_TOK_LTE:      res = ng_val_bool(l.as.number <= r.as.number); break;
                    case NG_TOK_GT:       res = ng_val_bool(l.as.number > r.as.number); break;
                    case NG_TOK_GTE:      res = ng_val_bool(l.as.number >= r.as.number); break;
                    default: break;
                }
            } else if (l.type == NG_VAL_STRING && r.type == NG_VAL_STRING && node->as.binary_expr.op == NG_TOK_PLUS) {
                size_t len = strlen(l.as.string) + strlen(r.as.string);
                char* concat = (char*)malloc(len + 1);
                strcpy(concat, l.as.string);
                strcat(concat, r.as.string);
                res = ng_val_string(concat);
                free(concat);
            } else if (node->as.binary_expr.op == NG_TOK_EQ) {
                /* Compare mixed types: null == null, bool == bool, string == string */
                if (l.type == NG_VAL_NULL && r.type == NG_VAL_NULL) {
                    res = ng_val_bool(true);
                } else if (l.type == NG_VAL_BOOL && r.type == NG_VAL_BOOL) {
                    res = ng_val_bool(l.as.boolean == r.as.boolean);
                } else if (l.type == NG_VAL_STRING && r.type == NG_VAL_STRING) {
                    res = ng_val_bool(strcmp(l.as.string, r.as.string) == 0);
                } else {
                    res = ng_val_bool(false);
                }
            } else if (node->as.binary_expr.op == NG_TOK_NEQ) {
                if (l.type == NG_VAL_NULL && r.type == NG_VAL_NULL) {
                    res = ng_val_bool(false);
                } else if (l.type == NG_VAL_BOOL && r.type == NG_VAL_BOOL) {
                    res = ng_val_bool(l.as.boolean != r.as.boolean);
                } else if (l.type == NG_VAL_STRING && r.type == NG_VAL_STRING) {
                    res = ng_val_bool(strcmp(l.as.string, r.as.string) != 0);
                } else {
                    res = ng_val_bool(true);
                }
            }
            ng_val_free(&l);
            ng_val_free(&r);
            return res;
        }

        case NG_AST_EXPR_TABLE_GET: {
            NGValue tbl = eval_expression(interp, node->as.table_get.table);
            NGValue result = ng_val_null();
            if (tbl.type == NG_VAL_TABLE && tbl.as.table) {
                NGValue field_val;
                if (ng_table_get(tbl.as.table, node->as.table_get.field, &field_val)) {
                    result = ng_val_clone(&field_val);
                }
            }
            ng_val_free(&tbl);
            return result;
        }

        case NG_AST_EXPR_CALL: {
            NGTable* prev_receiver = interp->receiver_table;
            NGAstNode* callee_node = node->as.call_expr.callee;

            /* Para chamadas de metodo (obj.metodo(...)), avalia o receptor
               uma unica vez e mantem vivo enquanto o metodo executa. */
            NGValue receiver = ng_val_null();
            NGValue callee = ng_val_null();
            if (callee_node && callee_node->type == NG_AST_EXPR_TABLE_GET) {
                receiver = eval_expression(interp, callee_node->as.table_get.table);
                if (receiver.type == NG_VAL_TABLE && receiver.as.table) {
                    interp->receiver_table = receiver.as.table;
                    NGValue field_val;
                    if (ng_table_get(receiver.as.table,
                                     callee_node->as.table_get.field, &field_val)) {
                        callee = ng_val_clone(&field_val);
                    }
                }
            } else {
                callee = eval_expression(interp, callee_node);
            }

            size_t arg_count = node->as.call_expr.arg_count;
            NGValue* args = NULL;
            if (arg_count > 0) {
                args = (NGValue*)malloc(sizeof(NGValue) * arg_count);
                if (!args) {
                    ng_val_free(&callee);
                    ng_val_free(&receiver);
                    interp->receiver_table = prev_receiver;
                    return ng_val_null();
                }
                for (size_t i = 0; i < arg_count; i++) {
                    args[i] = eval_expression(interp, node->as.call_expr.args[i]);
                }
            }

            NGValue result = ng_val_null();
            bool called = false;

            /* Tabela chamavel: __call / novo / Novo */
            if (callee.type == NG_VAL_TABLE && callee.as.table) {
                NGValue call_method;
                if (ng_table_get(callee.as.table, "__call", &call_method) ||
                    ng_table_get(callee.as.table, "novo", &call_method) ||
                    ng_table_get(callee.as.table, "Novo", &call_method)) {
                    interp->receiver_table = callee.as.table;
                    result = ng_interp_call_func(interp, &call_method, args, arg_count);
                    called = true;
                }
            }

            if (!called && (callee.type == NG_VAL_NATIVE_FUNC || callee.type == NG_VAL_FUNCTION)) {
                result = ng_interp_call_func(interp, &callee, args, arg_count);
                called = true;
            }

            if (!called) {
                ng_error_print(NG_ERR_UNDEFINED_FUNC, "Funcao Nao Encontrada", interp->current_file, node->line, node->column,
                               "Tentativa de chamar um identificador que nao e uma funcao valida.", "Verifique o nome da funcao.");
            }

            for (size_t i = 0; i < arg_count; i++) {
                ng_val_free(&args[i]);
            }
            free(args);
            ng_val_free(&callee);
            ng_val_free(&receiver);
            interp->receiver_table = prev_receiver;
            return result;
        }

        case NG_AST_EXPR_FUNC: {
            NGValue f;
            f.type = NG_VAL_FUNCTION;
            f.as.func.name = node->as.func_expr.name ? strdup(node->as.func_expr.name) : strdup("<anonima>");
            f.as.func.param_count = node->as.func_expr.param_count;
            if (f.as.func.param_count > 0) {
                f.as.func.params = (char**)malloc(sizeof(char*) * f.as.func.param_count);
                for (size_t i = 0; i < f.as.func.param_count; i++) {
                    f.as.func.params[i] = strdup(node->as.func_expr.params[i]);
                }
            } else {
                f.as.func.params = NULL;
            }
            f.as.func.body = node->as.func_expr.body;
            return f;
        }

        default:
            return ng_val_null();
    }
}

bool ng_interp_run(NGInterpreter* interp, NGAstNode* program) {
    if (!program) return false;
    return execute_statement(interp, program);
}
