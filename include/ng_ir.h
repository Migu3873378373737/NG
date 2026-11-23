#ifndef NG_IR_H
#define NG_IR_H

#include "ng_ast.h"
#include "ng_value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IR intermediário (Core 1 -> Core 0): árvore espelho da AST otimizada,
   serializável em bytecode. */

typedef struct NGLIRNode NGLIRNode;

typedef enum {
    NGLIR_PROGRAM = 0,
    NGLIR_STMT_PRINT,
    NGLIR_STMT_G,
    NGLIR_STMT_Y,
    NGLIR_STMT_ASSIGN,
    NGLIR_STMT_IF,
    NGLIR_STMT_WHILE,
    NGLIR_STMT_FUNC,
    NGLIR_STMT_IMPORT,
    NGLIR_STMT_TABLE_SET,
    NGLIR_STMT_LOOP,
    NGLIR_EXPR_BINARY,
    NGLIR_EXPR_LITERAL,
    NGLIR_EXPR_IDENT,
    NGLIR_EXPR_CALL,
    NGLIR_EXPR_TABLE_GET,
    NGLIR_EXPR_FUNC,
    NGLIR_EXPR_ARRAY,
    NGLIR_EXPR_INDEX
} NGLIRNodeType;

struct NGLIRNode {
    int type;
    int line;
    int column;
    int op;                   /* NGTokenType p/ BINARY */
    NGValue value;            /* literal */
    char* name;               /* ident, função, campo da tabela */
    char** params;            /* FUNC: parâmetros */
    size_t param_count;
    NGLIRNode** kids;         /* listas de stmts/exprs */
    size_t kid_count;
    NGLIRNode* a;             /* expressão ternária: cond/esquerda/valor */
    NGLIRNode* b;             /* expressão ternária: else/direita */
};

NGLIRNode* ng_ir_new(int type, int line, int column);
void ng_ir_free(NGLIRNode* node);
NGLIRNode* ng_ir_from_ast(const NGAstNode* ast);
size_t ng_ir_node_count(const NGLIRNode* ir);

/* Serializa a árvore IR em bytecode (formato NIRB1: opcodes, strings, literais).
   Retorna buffer alocado com malloc ou NULL; o tamanho vai para out_len. */
uint8_t* ng_ir_to_bytecode(const NGLIRNode* ir, size_t* out_len);

#endif /* NG_IR_H */
