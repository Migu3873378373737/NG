#include "ng_ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

NGLIRNode* ng_ir_new(int type, int line, int column) {
    NGLIRNode* n = (NGLIRNode*)calloc(1, sizeof(NGLIRNode));
    n->type = type;
    n->line = line;
    n->column = column;
    return n;
}

static void ir_push_kid(NGLIRNode* parent, NGLIRNode* kid) {
    NGLIRNode** p = (NGLIRNode**)realloc(parent->kids, (parent->kid_count + 1) * sizeof(NGLIRNode*));
    if (!p) { if (kid) ng_ir_free(kid); return; }
    parent->kids = p;
    parent->kids[parent->kid_count++] = kid;
}

void ng_ir_free(NGLIRNode* node) {
    if (!node) return;
    for (size_t i = 0; i < node->kid_count; i++) ng_ir_free(node->kids[i]);
    free(node->kids);
    ng_ir_free(node->a);
    ng_ir_free(node->b);
    free(node->name);
    for (size_t i = 0; i < node->param_count; i++) free(node->params[i]);
    free(node->params);
    if (node->type == NGLIR_EXPR_LITERAL || node->type == NGLIR_STMT_ASSIGN ||
        node->type == NGLIR_EXPR_CALL || node->type == NGLIR_EXPR_TABLE_GET ||
        node->type == NGLIR_STMT_TABLE_SET) {
        ng_val_free(&node->value);
    }
    free(node);
}

static NGLIRNode* ir_from_ast_stmt(const NGAstNode* ast);
static NGLIRNode* ir_from_ast_expr(const NGAstNode* ast);

static NGLIRNode* ir_from_ast_expr(const NGAstNode* ast) {
    if (!ast) return NULL;
    switch (ast->type) {
        case NG_AST_EXPR_LITERAL: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_LITERAL, ast->line, ast->column);
            n->value = ast->as.literal_expr.value;
            if (n->value.type == NG_VAL_STRING && n->value.as.string)
                n->value.as.string = strdup(n->value.as.string);
            return n;
        }
        case NG_AST_EXPR_IDENTIFIER: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_IDENT, ast->line, ast->column);
            n->name = strdup(ast->as.id_expr.name);
            return n;
        }
        case NG_AST_EXPR_BINARY: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_BINARY, ast->line, ast->column);
            n->op = ast->as.binary_expr.op;
            n->a = ir_from_ast_expr(ast->as.binary_expr.left);
            n->b = ir_from_ast_expr(ast->as.binary_expr.right);
            return n;
        }
        case NG_AST_EXPR_CALL: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_CALL, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.call_expr.callee);
            for (size_t i = 0; i < ast->as.call_expr.arg_count; i++)
                ir_push_kid(n, ir_from_ast_expr(ast->as.call_expr.args[i]));
            return n;
        }
        case NG_AST_EXPR_TABLE_GET: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_TABLE_GET, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.table_get.table);
            n->name = strdup(ast->as.table_get.field);
            return n;
        }
        case NG_AST_EXPR_FUNC: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_FUNC, ast->line, ast->column);
            if (ast->as.func_expr.name) n->name = strdup(ast->as.func_expr.name);
            n->param_count = ast->as.func_expr.param_count;
            if (n->param_count) {
                n->params = (char**)calloc(n->param_count, sizeof(char*));
                for (size_t i = 0; i < n->param_count; i++)
                    n->params[i] = ast->as.func_expr.params[i] ? strdup(ast->as.func_expr.params[i]) : NULL;
            }
            n->a = ir_from_ast_expr(ast->as.func_expr.body);
            return n;
        }
        case NG_AST_EXPR_ARRAY: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_ARRAY, ast->line, ast->column);
            for (size_t i = 0; i < ast->as.array_expr.count; i++)
                ir_push_kid(n, ir_from_ast_expr(ast->as.array_expr.elements[i]));
            return n;
        }
        case NG_AST_EXPR_INDEX: {
            NGLIRNode* n = ng_ir_new(NGLIR_EXPR_INDEX, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.index_expr.target);
            n->b = ir_from_ast_expr(ast->as.index_expr.index);
            return n;
        }
        default:
            return NULL;
    }
}

static void ir_from_ast_params(NGLIRNode* n, size_t count, char** params) {
    n->param_count = count;
    if (count) {
        n->params = (char**)calloc(count, sizeof(char*));
        for (size_t i = 0; i < count; i++)
            n->params[i] = params[i] ? strdup(params[i]) : NULL;
    }
}

static NGLIRNode* ir_from_ast_stmt(const NGAstNode* ast) {
    if (!ast) return NULL;
    switch (ast->type) {
        case NG_AST_PROGRAM: {
            NGLIRNode* n = ng_ir_new(NGLIR_PROGRAM, ast->line, ast->column);
            for (size_t i = 0; i < ast->as.program.count; i++)
                ir_push_kid(n, ir_from_ast_stmt(ast->as.program.statements[i]));
            return n;
        }
        case NG_AST_STMT_PRINT: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_PRINT, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.print_stmt.expr);
            return n;
        }
        case NG_AST_STMT_G: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_G, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.g_stmt.expr);
            return n;
        }
        case NG_AST_STMT_Y: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_Y, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.y_stmt.expr);
            return n;
        }
        case NG_AST_STMT_ASSIGN: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_ASSIGN, ast->line, ast->column);
            n->name = strdup(ast->as.assign_stmt.name);
            n->a = ir_from_ast_expr(ast->as.assign_stmt.value);
            return n;
        }
        case NG_AST_STMT_IF: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_IF, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.if_stmt.condition);
            ir_push_kid(n, ir_from_ast_stmt(ast->as.if_stmt.then_block));
            ir_push_kid(n, ir_from_ast_stmt(ast->as.if_stmt.else_block));
            return n;
        }
        case NG_AST_STMT_WHILE: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_WHILE, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.while_stmt.condition);
            ir_push_kid(n, ir_from_ast_stmt(ast->as.while_stmt.block));
            return n;
        }
        case NG_AST_STMT_FUNC: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_FUNC, ast->line, ast->column);
            n->name = strdup(ast->as.func_stmt.name);
            ir_from_ast_params(n, ast->as.func_stmt.param_count, ast->as.func_stmt.params);
            ir_push_kid(n, ir_from_ast_stmt(ast->as.func_stmt.body));
            return n;
        }
        case NG_AST_STMT_IMPORT: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_IMPORT, ast->line, ast->column);
            n->name = strdup(ast->as.import_stmt.module_name);
            return n;
        }
        case NG_AST_STMT_TABLE_SET: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_TABLE_SET, ast->line, ast->column);
            n->a = ir_from_ast_expr(ast->as.table_set.table);
            n->name = strdup(ast->as.table_set.field);
            n->b = ir_from_ast_expr(ast->as.table_set.value);
            return n;
        }
        case NG_AST_STMT_LOOP: {
            NGLIRNode* n = ng_ir_new(NGLIR_STMT_LOOP, ast->line, ast->column);
            n->name = strdup(ast->as.loop_stmt.var_name);
            n->a = ir_from_ast_expr(ast->as.loop_stmt.iterable);
            ir_push_kid(n, ir_from_ast_stmt(ast->as.loop_stmt.body));
            return n;
        }
        default:
            return NULL;
    }
}

NGLIRNode* ng_ir_from_ast(const NGAstNode* ast) {
    return ir_from_ast_stmt(ast);
}

static void ir_count(const NGLIRNode* node, size_t* count) {
    if (!node) return;
    (*count)++;
    for (size_t i = 0; i < node->kid_count; i++) ir_count(node->kids[i], count);
    if (node->a) ir_count(node->a, count);
    if (node->b) ir_count(node->b, count);
}

size_t ng_ir_node_count(const NGLIRNode* ir) {
    size_t count = 0;
    ir_count(ir, &count);
    return count;
}

/* ---------------- Serializador de bytecode (NIRB1) ---------------- */

typedef struct {
    unsigned char* data;
    size_t len;
    size_t cap;
} IRBuf;

static void irb_reserve(IRBuf* b, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 1024;
        while (ncap < b->len + n) ncap *= 2;
        unsigned char* p = (unsigned char*)realloc(b->data, ncap);
        if (!p) return; /* best effort; serialize stops on NULL */
        b->data = p;
        b->cap = ncap;
    }
}

static void irb_raw(IRBuf* b, const void* p, size_t n) {
    irb_reserve(b, n);
    if (!b->data) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void irb_u32(IRBuf* b, uint32_t v) { irb_raw(b, &v, 4); }

static void irb_str(IRBuf* b, const char* s) {
    if (!s) { irb_u32(b, 0xFFFFFFFF); return; }
    uint32_t len = (uint32_t)strlen(s);
    irb_u32(b, len);
    irb_raw(b, s, len);
}

static void irb_literal(IRBuf* b, const NGValue* v) {
    irb_u32(b, (uint32_t)v->type);
    switch (v->type) {
        case NG_VAL_NUMBER: {
            double d = v->as.number;
            irb_raw(b, &d, 8);
            break;
        }
        case NG_VAL_STRING: irb_str(b, v->as.string); break;
        case NG_VAL_BOOL: irb_u32(b, v->as.boolean ? 1 : 0); break;
        default: break;
    }
}

static void ir_ser(IRBuf* b, const NGLIRNode* n) {
    if (!n) { irb_u32(b, 0xFFFFFFFF); return; }
    irb_u32(b, (uint32_t)n->type);
    irb_u32(b, (uint32_t)n->line);
    irb_u32(b, (uint32_t)n->column);
    if (n->type == NGLIR_EXPR_LITERAL) {
        irb_literal(b, &n->value);
    } else if (n->type == NGLIR_EXPR_BINARY) {
        irb_u32(b, (uint32_t)n->op);
        ir_ser(b, n->a);
        ir_ser(b, n->b);
    } else if (n->type == NGLIR_EXPR_IDENT || n->type == NGLIR_STMT_ASSIGN ||
               n->type == NGLIR_STMT_IMPORT || n->type == NGLIR_EXPR_TABLE_GET ||
               n->type == NGLIR_STMT_TABLE_SET) {
        irb_str(b, n->name);
    }
    if (n->type == NGLIR_EXPR_CALL || n->type == NGLIR_EXPR_FUNC || n->type == NGLIR_STMT_IF ||
        n->type == NGLIR_STMT_WHILE || n->type == NGLIR_EXPR_ARRAY || n->type == NGLIR_EXPR_INDEX) {
        ir_ser(b, n->a);
        ir_ser(b, n->b);
    }
    irb_u32(b, (uint32_t)n->kid_count);
    for (size_t i = 0; i < n->kid_count; i++) ir_ser(b, n->kids[i]);
    if (n->type == NGLIR_STMT_FUNC || n->type == NGLIR_EXPR_FUNC) {
        irb_u32(b, (uint32_t)n->param_count);
        for (size_t i = 0; i < n->param_count; i++) irb_str(b, n->params[i]);
    }
}

uint8_t* ng_ir_to_bytecode(const NGLIRNode* ir, size_t* out_len) {
    IRBuf b;
    memset(&b, 0, sizeof(b));
    irb_raw(&b, "NIRB", 4);
    ir_ser(&b, ir);
    if (out_len) *out_len = b.len;
    if (!b.data) return NULL;
    return b.data;
}
