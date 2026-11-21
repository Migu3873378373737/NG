#ifndef NG_AST_H
#define NG_AST_H

#include "ng_value.h"

typedef enum {
    NG_AST_PROGRAM,
    NG_AST_STMT_PRINT,
    NG_AST_STMT_G,
    NG_AST_STMT_Y,
    NG_AST_STMT_ASSIGN,
    NG_AST_STMT_IF,
    NG_AST_STMT_WHILE,
    NG_AST_STMT_FUNC,
    NG_AST_STMT_IMPORT,
    NG_AST_EXPR_BINARY,
    NG_AST_EXPR_LITERAL,
    NG_AST_EXPR_IDENTIFIER,
    NG_AST_EXPR_CALL,
    NG_AST_EXPR_TABLE_GET,
    NG_AST_STMT_TABLE_SET,
    NG_AST_EXPR_FUNC,
    NG_AST_STMT_LOOP,
    NG_AST_EXPR_ARRAY,
    NG_AST_EXPR_INDEX
} NGAstNodeType;

typedef struct NGAstNode {
    NGAstNodeType type;
    int line;
    int column;
    union {
        struct {
            struct NGAstNode** statements;
            size_t count;
            size_t capacity;
        } program;
        
        struct {
            struct NGAstNode* expr;
        } print_stmt;
        
        struct {
            struct NGAstNode* expr; // se NULL, exibe o conteúdo de Y
        } g_stmt;
        
        struct {
            struct NGAstNode* expr;
        } y_stmt;
        
        struct {
            char* name;
            struct NGAstNode* value;
        } assign_stmt;
        
        struct {
            struct NGAstNode* condition;
            struct NGAstNode* then_block;
            struct NGAstNode* else_block;
        } if_stmt;
        
        struct {
            struct NGAstNode* condition;
            struct NGAstNode* block;
        } while_stmt;
        
        struct {
            char* name;
            char** params;
            size_t param_count;
            struct NGAstNode* body;
        } func_stmt;

        struct {
            char* module_name;
            char* alias;
        } import_stmt;
        
        struct {
            int op; // NGTokenType
            struct NGAstNode* left;
            struct NGAstNode* right;
        } binary_expr;
        
        struct {
            NGValue value;
        } literal_expr;
        
        struct {
            char* name;
        } id_expr;
        
        struct {
            struct NGAstNode* callee;
            struct NGAstNode** args;
            size_t arg_count;
        } call_expr;

        struct {
            struct NGAstNode* table;
            char* field;
        } table_get;

        struct {
            struct NGAstNode* table;
            char* field;
            struct NGAstNode* value;
        } table_set;

        struct {
            char* name;
            char** params;
            size_t param_count;
            struct NGAstNode* body;
        } func_expr;

        struct {
            char* var_name;
            struct NGAstNode* iterable;
            struct NGAstNode* body;
        } loop_stmt;

        struct {
            struct NGAstNode** elements;
            size_t count;
            size_t capacity;
        } array_expr;

        struct {
            struct NGAstNode* target;
            struct NGAstNode* index;
        } index_expr;
    } as;
} NGAstNode;

NGAstNode* ng_ast_new(NGAstNodeType type, int line, int column);
void ng_ast_free(NGAstNode* node);

#endif // NG_AST_H
