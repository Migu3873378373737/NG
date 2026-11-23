#include "ng_ast.h"
#include <stdlib.h>
#include <string.h>

NGAstNode* ng_ast_new(NGAstNodeType type, int line, int column) {
    NGAstNode* node = (NGAstNode*)calloc(1, sizeof(NGAstNode));
    node->type = type;
    node->line = line;
    node->column = column;
    return node;
}

void ng_ast_free(NGAstNode* node) {
    if (!node) return;
    switch (node->type) {
        case NG_AST_PROGRAM:
            for (size_t i = 0; i < node->as.program.count; i++) {
                ng_ast_free(node->as.program.statements[i]);
            }
            free(node->as.program.statements);
            break;
        case NG_AST_STMT_PRINT:
            ng_ast_free(node->as.print_stmt.expr);
            break;
        case NG_AST_STMT_G:
            ng_ast_free(node->as.g_stmt.expr);
            break;
        case NG_AST_STMT_Y:
            ng_ast_free(node->as.y_stmt.expr);
            break;
        case NG_AST_STMT_ASSIGN:
            free(node->as.assign_stmt.name);
            ng_ast_free(node->as.assign_stmt.value);
            break;
        case NG_AST_STMT_IF:
            ng_ast_free(node->as.if_stmt.condition);
            ng_ast_free(node->as.if_stmt.then_block);
            ng_ast_free(node->as.if_stmt.else_block);
            break;
        case NG_AST_STMT_WHILE:
            ng_ast_free(node->as.while_stmt.condition);
            ng_ast_free(node->as.while_stmt.block);
            break;
        case NG_AST_STMT_FUNC:
            free(node->as.func_stmt.name);
            for (size_t i = 0; i < node->as.func_stmt.param_count; i++) {
                free(node->as.func_stmt.params[i]);
            }
            free(node->as.func_stmt.params);
            ng_ast_free(node->as.func_stmt.body);
            break;
        case NG_AST_STMT_IMPORT:
            free(node->as.import_stmt.module_name);
            if (node->as.import_stmt.alias) free(node->as.import_stmt.alias);
            break;
        case NG_AST_EXPR_BINARY:
            ng_ast_free(node->as.binary_expr.left);
            ng_ast_free(node->as.binary_expr.right);
            break;
        case NG_AST_EXPR_LITERAL:
            ng_val_free(&node->as.literal_expr.value);
            break;
        case NG_AST_EXPR_IDENTIFIER:
            free(node->as.id_expr.name);
            break;
        case NG_AST_EXPR_CALL:
            ng_ast_free(node->as.call_expr.callee);
            for (size_t i = 0; i < node->as.call_expr.arg_count; i++) {
                ng_ast_free(node->as.call_expr.args[i]);
            }
            free(node->as.call_expr.args);
            break;
        case NG_AST_EXPR_TABLE_GET:
            ng_ast_free(node->as.table_get.table);
            free(node->as.table_get.field);
            break;
        case NG_AST_STMT_TABLE_SET:
            ng_ast_free(node->as.table_set.table);
            free(node->as.table_set.field);
            ng_ast_free(node->as.table_set.value);
            break;
        case NG_AST_EXPR_FUNC:
            if (node->as.func_expr.name) free(node->as.func_expr.name);
            for (size_t i = 0; i < node->as.func_expr.param_count; i++) {
                free(node->as.func_expr.params[i]);
            }
            free(node->as.func_expr.params);
            ng_ast_free(node->as.func_expr.body);
            break;
        case NG_AST_STMT_LOOP:
            free(node->as.loop_stmt.var_name);
            ng_ast_free(node->as.loop_stmt.iterable);
            ng_ast_free(node->as.loop_stmt.body);
            break;
        case NG_AST_EXPR_ARRAY:
            for (size_t i = 0; i < node->as.array_expr.count; i++) {
                ng_ast_free(node->as.array_expr.elements[i]);
            }
            free(node->as.array_expr.elements);
            break;
        case NG_AST_EXPR_INDEX:
            ng_ast_free(node->as.index_expr.target);
            ng_ast_free(node->as.index_expr.index);
            break;
    }
    free(node);
}
