#include "ng_parser.h"
#include "ng_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void advance(NGParser* p) {
    /* O texto do token anterior nao e mais necessario: libera para nao vazar
       uma string por token em programas grandes. */
    ng_token_free(&p->previous);
    p->previous = p->current;
    p->current = ng_lexer_next_token(p->lexer);
}

void ng_parser_cleanup(NGParser* parser) {
    if (!parser) return;
    ng_token_free(&parser->current);
    ng_token_free(&parser->previous);
}

static bool check(NGParser* p, NGTokenType type) {
    return p->current.type == type;
}

static bool match(NGParser* p, NGTokenType type) {
    if (check(p, type)) {
        advance(p);
        return true;
    }
    return false;
}

/* Transfere a propriedade do texto do token para a AST, evitando strdup. */
static char* take_token_text(NGParser* p) {
    char* text = p->previous.text;
    p->previous.text = NULL;
    return text ? text : strdup("");
}

static void error_at_current(NGParser* p, const char* code, const char* title, const char* desc, const char* sugg) {
    p->has_error = true;
    ng_error_print(code, title, p->filename, p->current.line, p->current.column, desc, sugg);
}

void ng_parser_init(NGParser* parser, NGLexer* lexer, const char* filename) {
    parser->lexer = lexer;
    parser->filename = filename;
    parser->has_error = false;
    parser->disable_index = false;
    parser->current.text = NULL;
    parser->previous.text = NULL;
    advance(parser);
}

static NGAstNode* parse_expression(NGParser* p);
static NGAstNode* parse_statement(NGParser* p);
static NGAstNode* parse_block(NGParser* p);

/* --- builders com capacidade (evitam realloc por elemento/argumento) --- */

static void ast_program_push(NGAstNode* block, NGAstNode* stmt) {
    if (block->as.program.count == block->as.program.capacity) {
        size_t cap = block->as.program.capacity ? block->as.program.capacity * 2 : 8;
        block->as.program.statements = (NGAstNode**)realloc(
            block->as.program.statements, sizeof(NGAstNode*) * cap);
        block->as.program.capacity = cap;
    }
    block->as.program.statements[block->as.program.count++] = stmt;
}

static void ast_array_push(NGAstNode* array, NGAstNode* element) {
    if (array->as.array_expr.count == array->as.array_expr.capacity) {
        size_t cap = array->as.array_expr.capacity ? array->as.array_expr.capacity * 2 : 4;
        array->as.array_expr.elements = (NGAstNode**)realloc(
            array->as.array_expr.elements, sizeof(NGAstNode*) * cap);
        array->as.array_expr.capacity = cap;
    }
    array->as.array_expr.elements[array->as.array_expr.count++] = element;
}

static void ast_call_push(NGAstNode* call, NGAstNode* arg) {
    if (call->as.call_expr.arg_count == call->as.call_expr.arg_capacity) {
        size_t cap = call->as.call_expr.arg_capacity ? call->as.call_expr.arg_capacity * 2 : 4;
        call->as.call_expr.args = (NGAstNode**)realloc(
            call->as.call_expr.args, sizeof(NGAstNode*) * cap);
        call->as.call_expr.arg_capacity = cap;
    }
    call->as.call_expr.args[call->as.call_expr.arg_count++] = arg;
}

static void ast_params_push(char*** params, size_t* count, size_t* capacity, char* name) {
    if (*count == *capacity) {
        size_t cap = *capacity ? *capacity * 2 : 4;
        *params = (char**)realloc(*params, sizeof(char*) * cap);
        *capacity = cap;
    }
    (*params)[(*count)++] = name;
}

static void parse_call_arguments(NGParser* p, NGAstNode* call) {
    if (!check(p, NG_TOK_RPAREN)) {
        do {
            ast_call_push(call, parse_expression(p));
        } while (match(p, NG_TOK_COMMA));
    }
    if (!match(p, NG_TOK_RPAREN)) {
        error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                         "Esperado ')' apos os argumentos da funcao.",
                         "Feche os parenteses da chamada.");
    }
}

static NGAstNode* parse_primary_base(NGParser* p) {
    int line = p->current.line;
    int col = p->current.column;

    if (match(p, NG_TOK_NUMBER)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        node->as.literal_expr.value = ng_val_number(p->previous.number_value);
        return node;
    }

    if (match(p, NG_TOK_STRING)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        NGValue value = ng_val_null();
        value.type = NG_VAL_STRING;
        value.as.string = take_token_text(p);
        node->as.literal_expr.value = value;
        return node;
    }

    if (match(p, NG_TOK_KW_TRUE)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        node->as.literal_expr.value = ng_val_bool(true);
        return node;
    }

    if (match(p, NG_TOK_KW_FALSE)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        node->as.literal_expr.value = ng_val_bool(false);
        return node;
    }

    if (match(p, NG_TOK_KW_NULL)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        node->as.literal_expr.value = ng_val_null();
        return node;
    }

    /* Literal de lista: [expr, expr, ...] (também aceita lista vazia []). */
    if (match(p, NG_TOK_LBRACKET)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_ARRAY, line, col);
        node->as.array_expr.elements = NULL;
        node->as.array_expr.count = 0;
        node->as.array_expr.capacity = 0;

        while (!check(p, NG_TOK_RBRACKET) && !check(p, NG_TOK_EOF)) {
            NGAstNode* element = parse_expression(p);
            if (element) ast_array_push(node, element);
            if (!match(p, NG_TOK_COMMA)) break;
        }
        if (!match(p, NG_TOK_RBRACKET)) {
            error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                             "Esperado ']' apos os elementos da lista.",
                             "Feche a lista com ']'.");
        }
        return node;
    }

    if (match(p, NG_TOK_KW_Y)) {
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_IDENTIFIER, line, col);
        node->as.id_expr.name = strdup("Y");
        return node;
    }

    if (match(p, NG_TOK_KW_FUNCAO)) {
        char* fname = NULL;
        if (match(p, NG_TOK_IDENTIFIER)) {
            fname = take_token_text(p);
        }
        NGAstNode* fnode = ng_ast_new(NG_AST_EXPR_FUNC, line, col);
        fnode->as.func_expr.name = fname;
        fnode->as.func_expr.params = NULL;
        fnode->as.func_expr.param_count = 0;
        fnode->as.func_expr.param_capacity = 0;

        if (match(p, NG_TOK_LPAREN)) {
            if (!check(p, NG_TOK_RPAREN)) {
                do {
                    if (match(p, NG_TOK_IDENTIFIER)) {
                        ast_params_push(&fnode->as.func_expr.params,
                                        &fnode->as.func_expr.param_count,
                                        &fnode->as.func_expr.param_capacity,
                                        take_token_text(p));
                    }
                } while (match(p, NG_TOK_COMMA));
            }
            if (!match(p, NG_TOK_RPAREN)) {
                error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe", "Esperado ')' na lista de parametros da funcao.", "Feche os parenteses.");
            }
        }
        fnode->as.func_expr.body = parse_block(p);
        return fnode;
    }

    if (match(p, NG_TOK_IDENTIFIER)) {
        char* id_name = take_token_text(p);
        NGAstNode* node = ng_ast_new(NG_AST_EXPR_IDENTIFIER, line, col);
        node->as.id_expr.name = id_name;

        if (match(p, NG_TOK_LPAREN)) {
            NGAstNode* call = ng_ast_new(NG_AST_EXPR_CALL, line, col);
            call->as.call_expr.callee = node;
            parse_call_arguments(p, call);
            return call;
        }
        /* Campos, metodos e indices sao resolvidos pelo parse_postfix. */
        return node;
    }

    if (match(p, NG_TOK_LPAREN)) {
        NGAstNode* expr = parse_expression(p);
        if (!match(p, NG_TOK_RPAREN)) {
            error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe", "Esperado ')' apos a expressao.", "Feche os parenteses.");
        }
        return expr;
    }

    error_at_current(p, NG_ERR_INVALID_TOKEN, "Token Invalido", "Expressao inesperada ou nao reconhecida.", "Verifique a expressao informada.");
    advance(p);
    return NULL;
}

/* Pos-fixos encadeados: lista[0], registro["campo"], valor.campo,
   valor.metodo(...), inclusive combinacoes como lista[0].campo. */
static NGAstNode* parse_postfix(NGParser* p, NGAstNode* node) {
    while (node) {
        /* Em condicoes (Se/Enquanto/Loop) o '[' tambem abre bloco. Um indice
           valido vem colado ao valor (sem espaco), enquanto o bloco costuma
           ser separado por espaco: Se Lista[0] == 1 [ ... ]. */
        if (check(p, NG_TOK_LBRACKET) &&
            (!p->disable_index || !p->current.space_before)) {
            advance(p);  /* consome '[' */
            NGAstNode* index = parse_expression(p);
            NGAstNode* access = ng_ast_new(NG_AST_EXPR_INDEX, node->line, node->column);
            access->as.index_expr.target = node;
            access->as.index_expr.index = index;
            if (!match(p, NG_TOK_RBRACKET)) {
                error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                                 "Esperado ']' apos o indice.", "Feche o indice com ']'.");
            }
            node = access;
            continue;
        }

        if (match(p, NG_TOK_DOT)) {
            if (!match(p, NG_TOK_IDENTIFIER)) {
                error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                                 "Esperado identificador de campo apos '.'.",
                                 "Informe o nome do campo da tabela.");
                break;
            }
            NGAstNode* access = ng_ast_new(NG_AST_EXPR_TABLE_GET,
                                           node->line, node->column);
            access->as.table_get.table = node;
            access->as.table_get.field = take_token_text(p);
            if (match(p, NG_TOK_LPAREN)) {
                NGAstNode* call = ng_ast_new(NG_AST_EXPR_CALL,
                                             node->line, node->column);
                call->as.call_expr.callee = access;
                parse_call_arguments(p, call);
                node = call;
            } else {
                node = access;
            }
            continue;
        }

        break;
    }
    return node;
}

static NGAstNode* parse_primary(NGParser* p) {
    return parse_postfix(p, parse_primary_base(p));
}

/* Unarios: -expr e +expr. O menos e traduzido para (0 - expr), evitando
   um novo tipo de no na AST (e mantendo o cache N3 compativel). */
static NGAstNode* parse_unary(NGParser* p) {
    if (match(p, NG_TOK_MINUS)) {
        int line = p->previous.line;
        int col = p->previous.column;
        NGAstNode* operand = parse_unary(p);
        if (!operand) return NULL;
        NGAstNode* zero = ng_ast_new(NG_AST_EXPR_LITERAL, line, col);
        zero->as.literal_expr.value = ng_val_number(0);
        NGAstNode* bin = ng_ast_new(NG_AST_EXPR_BINARY, line, col);
        bin->as.binary_expr.op = NG_TOK_MINUS;
        bin->as.binary_expr.left = zero;
        bin->as.binary_expr.right = operand;
        return bin;
    }
    if (match(p, NG_TOK_PLUS)) {
        return parse_unary(p);
    }
    return parse_primary(p);
}

static NGAstNode* parse_multiplicative(NGParser* p) {
    NGAstNode* left = parse_unary(p);
    if (!left) return NULL;
    while (match(p, NG_TOK_STAR) || match(p, NG_TOK_SLASH) || match(p, NG_TOK_PERCENT)) {
        int op = p->previous.type;
        NGAstNode* right = parse_unary(p);
        NGAstNode* bin = ng_ast_new(NG_AST_EXPR_BINARY, left->line, left->column);
        bin->as.binary_expr.op = op;
        bin->as.binary_expr.left = left;
        bin->as.binary_expr.right = right;
        left = bin;
    }
    return left;
}

static NGAstNode* parse_additive(NGParser* p) {
    NGAstNode* left = parse_multiplicative(p);
    if (!left) return NULL;
    while (match(p, NG_TOK_PLUS) || match(p, NG_TOK_MINUS)) {
        int op = p->previous.type;
        NGAstNode* right = parse_multiplicative(p);
        NGAstNode* bin = ng_ast_new(NG_AST_EXPR_BINARY, left->line, left->column);
        bin->as.binary_expr.op = op;
        bin->as.binary_expr.left = left;
        bin->as.binary_expr.right = right;
        left = bin;
    }
    return left;
}

static NGAstNode* parse_relational(NGParser* p) {
    NGAstNode* left = parse_additive(p);
    if (!left) return NULL;
    while (match(p, NG_TOK_EQ) || match(p, NG_TOK_NEQ) ||
           match(p, NG_TOK_LT) || match(p, NG_TOK_LTE) ||
           match(p, NG_TOK_GT) || match(p, NG_TOK_GTE)) {
        int op = p->previous.type;
        NGAstNode* right = parse_additive(p);
        NGAstNode* bin = ng_ast_new(NG_AST_EXPR_BINARY, left->line, left->column);
        bin->as.binary_expr.op = op;
        bin->as.binary_expr.left = left;
        bin->as.binary_expr.right = right;
        left = bin;
    }
    return left;
}

static NGAstNode* parse_expression(NGParser* p) {
    return parse_relational(p);
}

static NGAstNode* parse_block(NGParser* p) {
    int line = p->current.line;
    int col = p->current.column;

    bool is_bracket = match(p, NG_TOK_LBRACKET);
    if (!is_bracket && !check(p, NG_TOK_KW_FIM) && !check(p, NG_TOK_EOF) && !check(p, NG_TOK_RPAREN)) {
        /* Bloco sem colchetes - fechado por 'fim' */
    } else if (!is_bracket) {
        error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe", "Esperado '[' ou instrucoes finalizadas por 'fim'.", "Abra o bloco utilizando '[' ou finalize com 'fim'.");
        return NULL;
    }

    NGAstNode* block = ng_ast_new(NG_AST_PROGRAM, line, col);
    block->as.program.statements = NULL;
    block->as.program.count = 0;
    block->as.program.capacity = 0;

    if (is_bracket) {
        while (!check(p, NG_TOK_RBRACKET) && !check(p, NG_TOK_EOF)) {
            NGAstNode* stmt = parse_statement(p);
            if (stmt) ast_program_push(block, stmt);
        }
        if (!match(p, NG_TOK_RBRACKET)) {
            error_at_current(p, NG_ERR_UNCLOSED_BLOCK, "Bloco Nao Fechado", "Bloco aberto com '[' nao foi fechado com ']'.", "Insira ']' para fechar o bloco correspondente.");
        }
    } else {
        while (!check(p, NG_TOK_KW_FIM) && !check(p, NG_TOK_EOF) && !check(p, NG_TOK_RPAREN)) {
            NGAstNode* stmt = parse_statement(p);
            if (stmt) ast_program_push(block, stmt);
        }
        match(p, NG_TOK_KW_FIM);
    }

    return block;
}

static NGAstNode* parse_statement(NGParser* p) {
    int line = p->current.line;
    int col = p->current.column;

    /* Print <expr> */
    if (match(p, NG_TOK_KW_PRINT)) {
        NGAstNode* node = ng_ast_new(NG_AST_STMT_PRINT, line, col);
        node->as.print_stmt.expr = parse_expression(p);
        return node;
    }

    /* Y = <expr> */
    if (match(p, NG_TOK_KW_Y)) {
        if (match(p, NG_TOK_ASSIGN)) {
            NGAstNode* node = ng_ast_new(NG_AST_STMT_Y, line, col);
            node->as.y_stmt.expr = parse_expression(p);
            return node;
        } else {
            NGAstNode* node = ng_ast_new(NG_AST_EXPR_IDENTIFIER, line, col);
            node->as.id_expr.name = strdup("Y");
            return node;
        }
    }

    /* G [expr] ou G Y */
    if (match(p, NG_TOK_KW_G)) {
        NGAstNode* node = ng_ast_new(NG_AST_STMT_G, line, col);
        if (!check(p, NG_TOK_EOF) && !check(p, NG_TOK_RBRACKET) &&
            !check(p, NG_TOK_KW_PRINT) && !check(p, NG_TOK_KW_G) && !check(p, NG_TOK_KW_SE)) {
            node->as.g_stmt.expr = parse_expression(p);
        } else {
            node->as.g_stmt.expr = NULL;
        }
        return node;
    }

    /* Se <cond> [ ... ] Senao [ ... ] */
    if (match(p, NG_TOK_KW_SE)) {
        NGAstNode* node = ng_ast_new(NG_AST_STMT_IF, line, col);
        bool previous_index = p->disable_index;
        p->disable_index = true;
        node->as.if_stmt.condition = parse_expression(p);
        p->disable_index = previous_index;
        node->as.if_stmt.then_block = parse_block(p);
        node->as.if_stmt.else_block = NULL;

        if (match(p, NG_TOK_KW_SENAO)) {
            if (check(p, NG_TOK_KW_SE)) {
                node->as.if_stmt.else_block = parse_statement(p);
            } else {
                node->as.if_stmt.else_block = parse_block(p);
            }
        }
        return node;
    }

    /* Enquanto <cond> [ ... ] */
    if (match(p, NG_TOK_KW_ENQUANTO)) {
        NGAstNode* node = ng_ast_new(NG_AST_STMT_WHILE, line, col);
        bool previous_index = p->disable_index;
        p->disable_index = true;
        node->as.while_stmt.condition = parse_expression(p);
        p->disable_index = previous_index;
        node->as.while_stmt.block = parse_block(p);
        return node;
    }

    /* Loop Variavel in Colecao [ ... ] */
    if (match(p, NG_TOK_KW_LOOP)) {
        if (!match(p, NG_TOK_IDENTIFIER)) {
            error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                             "Esperado nome de variavel apos 'Loop'.",
                             "Exemplo: Loop Nome in Nomes [ ... ]");
            return NULL;
        }
        char* var_name = take_token_text(p);

        if (!match(p, NG_TOK_KW_IN)) {
            error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                             "Esperado 'in' apos a variavel do Loop.",
                             "Exemplo: Loop Nome in Nomes [ ... ]");
            free(var_name);
            return NULL;
        }

        NGAstNode* node = ng_ast_new(NG_AST_STMT_LOOP, line, col);
        node->as.loop_stmt.var_name = var_name;
        bool previous_index = p->disable_index;
        p->disable_index = true;
        node->as.loop_stmt.iterable = parse_expression(p);
        p->disable_index = previous_index;
        node->as.loop_stmt.body = parse_block(p);
        return node;
    }

    /* Funcao Nome(param1, param2) [ ... ] */
    if (match(p, NG_TOK_KW_FUNCAO)) {
        if (!match(p, NG_TOK_IDENTIFIER)) {
            error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe", "Esperado nome apos 'Funcao'.", "Defina o nome da funcao.");
            return NULL;
        }
        NGAstNode* node = ng_ast_new(NG_AST_STMT_FUNC, line, col);
        node->as.func_stmt.name = take_token_text(p);
        node->as.func_stmt.params = NULL;
        node->as.func_stmt.param_count = 0;
        node->as.func_stmt.param_capacity = 0;

        if (match(p, NG_TOK_LPAREN)) {
            if (!check(p, NG_TOK_RPAREN)) {
                do {
                    if (match(p, NG_TOK_IDENTIFIER)) {
                        ast_params_push(&node->as.func_stmt.params,
                                        &node->as.func_stmt.param_count,
                                        &node->as.func_stmt.param_capacity,
                                        take_token_text(p));
                    }
                } while (match(p, NG_TOK_COMMA));
            }
            if (!match(p, NG_TOK_RPAREN)) {
                error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe", "Esperado ')' na lista de parametros da funcao.", "Feche os parenteses.");
            }
        }
        node->as.func_stmt.body = parse_block(p);
        return node;
    }

    /* Importar [@]<modulo> [como Alias] ou Importar "<caminho>" */
    if (match(p, NG_TOK_KW_IMPORTAR)) {
        char mod_buf[512] = {0};
        if (match(p, NG_TOK_STRING)) {
            strncpy(mod_buf, p->previous.text, sizeof(mod_buf) - 1);
        } else {
            match(p, NG_TOK_AT);
            if (match(p, NG_TOK_IDENTIFIER)) {
                strncat(mod_buf, p->previous.text, sizeof(mod_buf) - strlen(mod_buf) - 1);
                while (match(p, NG_TOK_SLASH) || match(p, NG_TOK_MINUS) || match(p, NG_TOK_DOT)) {
                    const char* sep = (p->previous.type == NG_TOK_SLASH) ? "/" :
                                      (p->previous.type == NG_TOK_MINUS) ? "-" : ".";
                    strncat(mod_buf, sep, sizeof(mod_buf) - strlen(mod_buf) - 1);
                    if (match(p, NG_TOK_IDENTIFIER)) {
                        strncat(mod_buf, p->previous.text, sizeof(mod_buf) - strlen(mod_buf) - 1);
                    } else if (match(p, NG_TOK_NUMBER)) {
                        char num[32];
                        snprintf(num, sizeof(num), "%lld",
                                 (long long)p->previous.number_value);
                        strncat(mod_buf, num, sizeof(mod_buf) - strlen(mod_buf) - 1);
                    }
                }
            }
        }
        if (mod_buf[0] != '\0') {
            NGAstNode* node = ng_ast_new(NG_AST_STMT_IMPORT, line, col);
            node->as.import_stmt.module_name = strdup(mod_buf);
            node->as.import_stmt.alias = NULL;
            if (match(p, NG_TOK_KW_COMO)) {
                if (match(p, NG_TOK_IDENTIFIER)) {
                    node->as.import_stmt.alias = take_token_text(p);
                }
            }
            return node;
        }
    }

    /*
     * Atribuicao ou chamada comecanado por identificador.
     * Casos:
     *   Nome = <expr>            -> STMT_ASSIGN
     *   Nome.campo = <expr>      -> STMT_TABLE_SET
     *   Nome(...)                -> EXPR_CALL (usado como statement)
     *   Nome.campo(...)          -> TABLE_GET + CALL (method call statement)
     *   Nome.campo.sub(...)      -> chained method call statement
     */
    if (check(p, NG_TOK_IDENTIFIER)) {
        advance(p);
        char* name = take_token_text(p);

        /* Nome = expr */
        if (match(p, NG_TOK_ASSIGN)) {
            NGAstNode* node = ng_ast_new(NG_AST_STMT_ASSIGN, line, col);
            node->as.assign_stmt.name = name;
            node->as.assign_stmt.value = parse_expression(p);
            return node;
        }

        NGAstNode* id_node = ng_ast_new(NG_AST_EXPR_IDENTIFIER, line, col);
        id_node->as.id_expr.name = name;

        /* Nome(...) */
        if (match(p, NG_TOK_LPAREN)) {
            NGAstNode* call = ng_ast_new(NG_AST_EXPR_CALL, line, col);
            call->as.call_expr.callee = id_node;
            parse_call_arguments(p, call);
            return parse_postfix(p, call);
        }

        /* Nome.campo... (com atribuicao em campo ou chamada de metodo) */
        if (match(p, NG_TOK_DOT)) {
            if (!match(p, NG_TOK_IDENTIFIER)) {
                error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                                 "Esperado identificador de campo apos '.'.",
                                 "Informe o campo.");
                ng_ast_free(id_node);
                return NULL;
            }
            char* field = take_token_text(p);

            if (match(p, NG_TOK_ASSIGN)) {
                NGAstNode* tset = ng_ast_new(NG_AST_STMT_TABLE_SET, line, col);
                tset->as.table_set.table = id_node;
                tset->as.table_set.field = field;
                tset->as.table_set.value = parse_expression(p);
                return tset;
            }

            NGAstNode* node = ng_ast_new(NG_AST_EXPR_TABLE_GET, line, col);
            node->as.table_get.table = id_node;
            node->as.table_get.field = field;

            /* Nome.campo(...) — metodo chamado sobre o campo: TABLE_GET + CALL */
            if (match(p, NG_TOK_LPAREN)) {
                NGAstNode* call = ng_ast_new(NG_AST_EXPR_CALL, line, col);
                call->as.call_expr.callee = node;
                parse_call_arguments(p, call);
                return parse_postfix(p, call);
            }

            while (match(p, NG_TOK_DOT)) {
                if (!match(p, NG_TOK_IDENTIFIER)) {
                    error_at_current(p, NG_ERR_SYNTAX, "Erro de Sintaxe",
                                     "Esperado identificador de campo apos '.'.",
                                     "Informe o campo.");
                    break;
                }
                char* sub = take_token_text(p);

                if (match(p, NG_TOK_ASSIGN)) {
                    NGAstNode* tset = ng_ast_new(NG_AST_STMT_TABLE_SET, line, col);
                    tset->as.table_set.table = node;
                    tset->as.table_set.field = sub;
                    tset->as.table_set.value = parse_expression(p);
                    return tset;
                }

                NGAstNode* tg = ng_ast_new(NG_AST_EXPR_TABLE_GET, line, col);
                tg->as.table_get.table = node;
                tg->as.table_get.field = sub;

                if (match(p, NG_TOK_LPAREN)) {
                    NGAstNode* call = ng_ast_new(NG_AST_EXPR_CALL, line, col);
                    call->as.call_expr.callee = tg;
                    parse_call_arguments(p, call);
                    return parse_postfix(p, call);
                }
                node = tg;
            }
            return node;
        }

        return id_node;
    }

    return parse_expression(p);
}

NGAstNode* ng_parser_parse_program(NGParser* p) {
    NGAstNode* prog = ng_ast_new(NG_AST_PROGRAM, 1, 1);
    prog->as.program.statements = NULL;
    prog->as.program.count = 0;
    prog->as.program.capacity = 0;

    while (!check(p, NG_TOK_EOF)) {
        NGAstNode* stmt = parse_statement(p);
        if (stmt) ast_program_push(prog, stmt);
    }
    return prog;
}
