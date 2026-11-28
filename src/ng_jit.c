#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ng_jit.h"
#include "ng_lexer.h"
#include "ng_value.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "../compat/windows/mmap_compat.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <errno.h>

/* Dynamic V "Core 0b": JIT nativo (aarch64) para hot paths/funções/loops.
 * Compila IR -> código de máquina executável (estrategia W^X: Write XOR Execute).
 * Fallback seguro: interpretador quando RWX nao disponivel ou permissoes falharem.
 * NAO desativa JIT globalmente; recua automaticamente para o interpretador. */

#ifndef NG_JIT_BACKEND
#if defined(__aarch64__)
#define NG_JIT_BACKEND 1
#else
#define NG_JIT_BACKEND 0
#endif
#endif

#if NG_JIT_BACKEND != 1
bool ng_jit_run_program(const NGAstNode* program, NGInterpreter* interp) {
    (void)program; (void)interp;
    return false;
}
bool ng_jit_enabled(void) { return false; }
#else

#define NG_JIT_MAX_SLOTS 64
#define NG_JIT_MAX_FUNCS 64
#define NG_JIT_MAX_CODE (256u * 1024u)
#define NG_JIT_MAX_RELOCS (NG_JIT_MAX_FUNCS * 16)

typedef struct NGJitFrame {
    double ret;                      /* offset 0 (unused) */
    double y;                        /* offset 8 (slot Y) */
    double slots[NG_JIT_MAX_SLOTS];  /* slots 0..63, offsets 16 + i*8 */
} NGJitFrame;

static void ng_jit_helper_print_num(double v) {
    NGValue val = ng_val_number(v);
    ng_val_print(&val);
}
static void ng_jit_helper_print_bool(double v) {
    NGValue val = ng_val_bool(v != 0.0);
    ng_val_print(&val);
}

/* W^X strategy helpers: alocacao RW, apos gera cod, muda para RX.
 * Isso evita problemas de segurança e incompatibilidade em ambientes
 * onde mmap(PROT_EXEC) e/ou PROT_READ|PROT_WRITE|PROT_EXEC sao bloqueados.
 */

static inline size_t ng_jit_page_align(size_t sz) {
    /* Paginas de 4KB; suficiente para a maioria dos ambientes.
     * Em ARM64 Linux o page size e typically 4KB, em alguns Android
     * pode ser 16KB, mas 4KB funciona como minimo compativel. */
    const size_t page = 4096;
    return (sz + page - 1) & ~(page - 1);
}

static inline bool ng_jit_is_rwx_supported(void) {
    /* Probe: tentativa de mmap RWX. Se falhar, o ambiente nao suporta.
     * Isso nao desativa o JIT, apenas detecta capacidade. */
    size_t probe_sz = 4096;
    void* probe = mmap(NULL, probe_sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe != MAP_FAILED) {
        munmap(probe, probe_sz);
        return true;
    }
    return false;
}

static inline void* ng_jit_alloc_rw(size_t sz) {
    /* Aloca memoria so com leitura e escrita (sem execucao).
     * W^X: Write THEN later Execute apos geracao de codigo. */
    size_t aligned = ng_jit_page_align(sz);
    void* ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        errno = 0;
        return NULL;
    }
    return ptr;
}

static inline bool ng_jit_make_rx(void* ptr, size_t sz) {
    /* Muda permissoes de RW para RX (Read+Execute) usando mprotect.
     * This implements the W^X principle: write during generation,
     * then remove write permission before execution. */
    size_t aligned = ng_jit_page_align(sz);
    int ret = mprotect(ptr, aligned, PROT_READ | PROT_EXEC);
    if (ret != 0) {
        /* Falha ao mudar permissoes; nao tentamos executar.
         * A memoria permanece RW e sera liberada pelo chamador. */
        errno = EACCES;
        return false;
    }
    return true;
}

static inline void ng_jit_sync_cache(void* ptr, size_t sz) {
    /* Sincroniza o cache de instrucoes para ARM64/AArch64.
     * __builtin___clear_cache() garante que o processador veja o codigo
     * recem-gerado em vez de instrucoes cacheadas obsoletas. */
    const size_t page = 4096;
    size_t aligned = ng_jit_page_align((uintptr_t)ptr + sz) - ng_jit_page_align((uintptr_t)ptr);
    /* Limpa cache de instrucoes na regiao [ptr, ptr+sz] */
    (void)__builtin___clear_cache((char*)ptr, (char*)ptr + sz);
}

static inline void ng_jit_free_and_null(void** ptr) {
    if (ptr && *ptr) {
        munmap(*ptr, ng_jit_page_align((size_t)*ptr));
        *ptr = NULL;
    }
}

typedef struct { uint8_t* data; size_t len, cap; } NGJitBuf;

static bool emit_raw(NGJitBuf* b, const void* src, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 8192;
        while (ncap < b->len + n) ncap *= 2;
        uint8_t* p = (uint8_t*)realloc(b->data, ncap);
        if (!p) return false;
        b->data = p; b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n); b->len += n;
    return true;
}
static bool emit_u32(NGJitBuf* b, uint32_t v) { return emit_raw(b, &v, 4); }

/* --- encoders (aarch64, ground truth via aarch64-linux-android-objdump) --- */

static bool emit_movz(NGJitBuf* b, int xd, uint32_t imm16, int hw) {
    return emit_u32(b, 0xD2800000u + ((uint32_t)imm16 << 5) + ((uint32_t)hw << 21) + ((uint32_t)xd & 31));
}
static bool emit_movk(NGJitBuf* b, int xd, uint32_t imm16, int hw) {
    return emit_u32(b, 0xF2800000u + ((uint32_t)imm16 << 5) + ((uint32_t)hw << 21) + ((uint32_t)xd & 31));
}
static bool emit_mov_const(NGJitBuf* b, int xd, uint64_t v) {
    uint32_t chunk[4];
    for (int i = 0; i < 4; i++) chunk[i] = (uint32_t)((v >> (16 * i)) & 0xFFFF);
    bool first = true;
    for (int i = 3; i >= 0; i--) {
        if (chunk[i] == 0 && !first) continue;
        if (first) { if (!emit_movz(b, xd, chunk[i], i)) return false; first = false; }
        else { if (!emit_movk(b, xd, chunk[i], i)) return false; }
    }
    if (first) if (!emit_movz(b, xd, 0, 0)) return false;
    return true;
}
static bool emit_fmov_to_d(NGJitBuf* b, int dt, int xn) {   /* FMOV Dd,Xn */
    return emit_u32(b, 0x9e670000u + ((uint32_t)(xn & 31) << 5) + ((uint32_t)(dt & 31)));
}
static bool emit_fmov_to_x(NGJitBuf* b, int xt, int dn) {   /* FMOV Xd,Dn */
    return emit_u32(b, 0x9e660000u + ((uint32_t)(dn & 31) << 5) + ((uint32_t)(xt & 31)));
}
static bool emit_ldr_x(NGJitBuf* b, int xt, int rn, long off) {
    if (off < 0 || off > 32760 || off % 8) return false;
    return emit_u32(b, 0xF9400000u + ((uint32_t)(off / 8) << 10) + ((uint32_t)(rn & 31) << 5) + ((uint32_t)(xt & 31)));
}
static bool emit_str_x(NGJitBuf* b, int xt, int rn, long off) {
    if (off < 0 || off > 32760 || off % 8) return false;
    return emit_u32(b, 0xF9000000u + ((uint32_t)(off / 8) << 10) + ((uint32_t)(rn & 31) << 5) + ((uint32_t)(xt & 31)));
}
static bool emit_fop(NGJitBuf* b, uint32_t base) {  /* FADD/FSUB/FMUL/FDIV d0,d0,d1 */
    return emit_u32(b, base);
}
static bool emit_fcmp(NGJitBuf* b, int rn, int rm) { /* FCMP Dn,Dm = 0x1e602000 + (Dn<<5) + (Dm<<16); d0,d1 = 0x1e612000 (verificado) */
    return emit_u32(b, 0x1e602000u + ((uint32_t)(rn & 31) << 5) + ((uint32_t)(rm & 31) << 16));
}
static bool emit_cset_w9(NGJitBuf* b, int cond) {    /* CSET W9,cond = enc(w0,cond)+9 */
    static const uint32_t enc[] = {
        [0]=0x1a9f17e0u,  /* EQ */
        [1]=0x1a9f07e0u,  /* NE */
        [4]=0x1a9f57e0u,  /* MI (float a<b) */
        [7]=0x1a9f87e0u,  /* LS (float a<=b) */
        [10]=0x1a9fb7e0u, /* GE */
        [12]=0x1a9fd7e0u, /* GT */
    };
    if (cond > 13 || enc[cond] == 0) return false;
    return emit_u32(b, enc[cond] + 9);
}
static bool emit_cmp_w9_zero(NGJitBuf* b) {
    /* CMP W9,#0 = 0x7100013f (rn=9) */
    return emit_u32(b, 0x71000000u + (9u << 5) + 31u);
}
static bool emit_b_cond(NGJitBuf* b, size_t pc, size_t target, int cond) {
    long d = (long)target - (long)pc;
    if (d % 4) return false;
    uint32_t imm19 = (uint32_t)((d / 4) & 0x7FFFF);
    return emit_u32(b, 0x54000000u + (imm19 << 5) + (uint32_t)cond);
}
static bool emit_b_uncond(NGJitBuf* b, size_t pc, size_t target) {
    long d = (long)target - (long)pc;
    if (d % 4) return false;
    return emit_u32(b, 0x14000000u + ((uint32_t)(d / 4) & 0x3FFFFFF));
}
static bool emit_blr_x9(NGJitBuf* b) { return emit_u32(b, 0xd63f0120u); }
static bool emit_ret(NGJitBuf* b) { return emit_u32(b, 0xd65f03c0u); }
/* Prólogo/epílogo com frame pointer em x19 (callee-saved).
 * O JIT chama helpers C via BLR, que destruiriam x30 (LR) e x0.
 * Guardar x19/x30 na pilha e usar x19 como base do frame evita
 * loop infinito ao retornar após um Print (BLR sobrescreve LR). */
static bool emit_mov_x19_x0(NGJitBuf* b) { return emit_u32(b, 0xaa0003f3u); } /* mov x19,x0 */
static bool emit_push_frame(NGJitBuf* b) { return emit_u32(b, 0xa9bf7bf3u); }  /* stp x19,x30,[sp,#-16]! */
static bool emit_pop_frame(NGJitBuf* b) { return emit_u32(b, 0xa8c17bf3u); }   /* ldp x19,x30,[sp],#16 */
static bool emit_push_lr(NGJitBuf* b) { return emit_u32(b, 0xa9bf7bfdu); }      /* stp x29,x30,[sp,#-16]! */
static bool emit_pop_lr(NGJitBuf* b) { return emit_u32(b, 0xa8c17bfdu); }       /* ldp x29,x30,[sp],#16 */

/* --- compilador --- */

typedef struct {
    char names[NG_JIT_MAX_SLOTS][32];
    size_t count;
} NGJitUnit;

typedef struct {
    const NGAstNode* program;
    NGJitUnit* cur_unit;
    NGJitBuf main_code;
    NGJitBuf func_codes[NG_JIT_MAX_FUNCS];
    char func_names[NG_JIT_MAX_FUNCS][32];
    NGJitUnit func_units[NG_JIT_MAX_FUNCS];
    size_t func_count;
    /* reloc: chamadas JIT -> patch com endereço absoluto */
    size_t reloc_off[NG_JIT_MAX_RELOCS];
    size_t reloc_func[NG_JIT_MAX_RELOCS];
    size_t reloc_buf[NG_JIT_MAX_RELOCS]; /* 0=main, 1..n=func i */
    size_t reloc_count;
    NGJitUnit main_unit;
} NGJitCompiler;

static int slot_of(NGJitUnit* u, const char* name, bool reserve) {
    for (size_t i = 0; i < u->count; i++)
        if (strcmp(u->names[i], name) == 0) return (int)i;
    if (!reserve || u->count >= NG_JIT_MAX_SLOTS) return -1;
    snprintf(u->names[u->count], 32, "%s", name);
    return (int)u->count++;
}
static long slot_off(int slot) { return 16 + slot * 8; } /* slots[0]@16 (ret,y@0..8) */

static bool is_arith(int op) {
    switch (op) {
        case NG_TOK_PLUS: case NG_TOK_MINUS: case NG_TOK_STAR:
        case NG_TOK_SLASH: case NG_TOK_PERCENT: return true;
        default: return false;
    }
}
static bool is_cmp(int op) {
    switch (op) {
        case NG_TOK_EQ: case NG_TOK_NEQ: case NG_TOK_LT:
        case NG_TOK_LTE: case NG_TOK_GT: case NG_TOK_GTE: return true;
        default: return false;
    }
}
static int cmp_cond(int op) { /* floats: MI(a<b), LS(a<=b), GT, GE, EQ, NE */
    switch (op) {
        case NG_TOK_EQ:  return 0;  /* EQ  */
        case NG_TOK_NEQ: return 1;  /* NE  */
        case NG_TOK_LT:  return 4;  /* MI  */
        case NG_TOK_LTE: return 7;  /* LS  */
        case NG_TOK_GT:  return 12; /* GT  */
        case NG_TOK_GTE: return 10; /* GE  */
        default: return -1;
    }
}
static int fop_base(int op) {
    switch (op) {
        case NG_TOK_PLUS:  return 0x1e612800; /* FADD d0,d0,d1 (verificado) */
        case NG_TOK_MINUS: return 0x1e613800; /* FSUB */
        case NG_TOK_STAR:  return 0x1e610800; /* FMUL */
        case NG_TOK_SLASH: return 0x1e611800; /* FDIV */
        default: return -1;
    }
}

static bool is_supported_expr(const NGAstNode* e);
static bool is_supported_stmt(const NGAstNode* s);
static bool is_supported_call(const NGAstNode* e);

static bool is_supported_expr(const NGAstNode* e) {
    if (!e) return true;
    switch (e->type) {
        case NG_AST_EXPR_LITERAL:
            return e->as.literal_expr.value.type == NG_VAL_NUMBER ||
                   e->as.literal_expr.value.type == NG_VAL_BOOL;
        case NG_AST_EXPR_IDENTIFIER:
            return true;
        case NG_AST_EXPR_BINARY:
            return is_arith(e->as.binary_expr.op) || is_cmp(e->as.binary_expr.op);
        case NG_AST_EXPR_CALL:
            return is_supported_call(e);
        default:
            return false;
    }
}
static bool is_supported_stmt(const NGAstNode* s) {
    if (!s) return true;
    if (s->type == NG_AST_PROGRAM) {
        for (size_t i = 0; i < s->as.program.count; i++)
            if (!is_supported_stmt(s->as.program.statements[i])) return false;
        return true;
    }
    switch (s->type) {
        case NG_AST_STMT_ASSIGN: return is_supported_expr(s->as.assign_stmt.value);
        case NG_AST_STMT_Y:      return is_supported_expr(s->as.y_stmt.expr);
        case NG_AST_STMT_PRINT:  return is_supported_expr(s->as.print_stmt.expr);
        case NG_AST_STMT_IF:
            return is_supported_expr(s->as.if_stmt.condition) &&
                   is_supported_stmt(s->as.if_stmt.then_block) &&
                   is_supported_stmt(s->as.if_stmt.else_block);
        case NG_AST_STMT_WHILE:
            return is_supported_expr(s->as.while_stmt.condition) &&
                   is_supported_stmt(s->as.while_stmt.block);
        default:
            return false;
    }
}
static bool is_supported_call(const NGAstNode* e) {
    if (!e || e->type != NG_AST_EXPR_CALL) return false;
    if (e->as.call_expr.callee->type != NG_AST_EXPR_IDENTIFIER) return false;
    if (e->as.call_expr.arg_count > 4) return false;
    for (size_t i = 0; i < e->as.call_expr.arg_count; i++)
        if (!is_supported_expr(e->as.call_expr.args[i])) return false;
    return true;
}
static bool can_compile_program(const NGAstNode* p) {
    if (!p || p->type != NG_AST_PROGRAM) return false;
    for (size_t i = 0; i < p->as.program.count; i++) {
        const NGAstNode* st = p->as.program.statements[i];
        if (st && st->type == NG_AST_STMT_FUNC) {
            if (st->as.func_stmt.param_count > 4) return false;
            if (!is_supported_stmt(st->as.func_stmt.body)) return false;
        }
    }
    for (size_t i = 0; i < p->as.program.count; i++) {
        const NGAstNode* st = p->as.program.statements[i];
        if (st && st->type == NG_AST_STMT_FUNC) continue;
        if (!is_supported_stmt(st)) return false;
    }
    return true;
}

/* --- emissão de expressões: resultado fica em d0 (ou d1 p/ rhs) --- */

static bool emit_expr(NGJitCompiler* c, NGJitBuf* b, const NGAstNode* e, int dst);
static bool emit_stmt(NGJitCompiler* c, NGJitBuf* b, NGJitUnit* u, const NGAstNode* s);

static bool emit_call(NGJitCompiler* c, NGJitBuf* b, const NGAstNode* e) {
    size_t n = e->as.call_expr.arg_count;
    for (size_t i = 0; i < n; i++)
        if (!emit_expr(c, b, e->as.call_expr.args[i], (int)i)) return false;
    const char* name = e->as.call_expr.callee->as.id_expr.name;
    size_t fi = 0;
    bool found = false;
    for (size_t i = 0; i < c->func_count; i++)
        if (strcmp(c->func_names[i], name) == 0) { fi = i; found = true; break; }
    if (!found) return false;
    if (c->reloc_count >= NG_JIT_MAX_RELOCS) return false;
    c->reloc_off[c->reloc_count] = b->len;
    c->reloc_func[c->reloc_count] = fi;
    c->reloc_buf[c->reloc_count] = (b == &c->main_code) ? 0 : 1 + fi;
    c->reloc_count++;
    /* movz x9,#0; movk x9,#0,lsl16; movk x9,#0,lsl32; movk x9,#0,lsl48; blr x9 */
    if (!emit_movz(b, 9, 0, 0)) return false;
    if (!emit_movk(b, 9, 0, 1)) return false;
    if (!emit_movk(b, 9, 0, 2)) return false;
    if (!emit_movk(b, 9, 0, 3)) return false;
    if (!emit_blr_x9(b)) return false;
    return true;
}

static bool emit_expr(NGJitCompiler* c, NGJitBuf* b, const NGAstNode* e, int dst) {
    if (!e || dst > 3) return false;
    switch (e->type) {
        case NG_AST_EXPR_LITERAL: {
            uint64_t bits = 0;
            if (e->as.literal_expr.value.type == NG_VAL_NUMBER)
                memcpy(&bits, &e->as.literal_expr.value.as.number, 8);
            else if (e->as.literal_expr.value.type == NG_VAL_BOOL)
                bits = e->as.literal_expr.value.as.boolean ? 1 : 0;
            if (!emit_mov_const(b, 9, bits)) return false;
            return emit_fmov_to_d(b, dst, 9);
        }
        case NG_AST_EXPR_IDENTIFIER: {
            if (strcmp(e->as.id_expr.name, "Y") == 0) {
                if (!emit_ldr_x(b, 9, 19, 8)) return false;
                return emit_fmov_to_d(b, dst, 9);
            }
            int slot = slot_of(c->cur_unit, e->as.id_expr.name, false);
            if (slot < 0) return false;
            if (!emit_ldr_x(b, 9, 19, slot_off(slot))) return false;
            return emit_fmov_to_d(b, dst, 9);
        }
        case NG_AST_EXPR_BINARY: {
            if (is_arith(e->as.binary_expr.op)) {
                int base = fop_base(e->as.binary_expr.op);
                if (base < 0) return false;
                if (!emit_expr(c, b, e->as.binary_expr.left, 0)) return false;
                if (!emit_expr(c, b, e->as.binary_expr.right, 1)) return false;
                return emit_fop(b, (uint32_t)base);
            }
            if (is_cmp(e->as.binary_expr.op)) {
                int cond = cmp_cond(e->as.binary_expr.op);
                if (cond < 0) return false;
                if (!emit_expr(c, b, e->as.binary_expr.left, 0)) return false;
                if (!emit_expr(c, b, e->as.binary_expr.right, 1)) return false;
                if (!emit_fcmp(b, 0, 1)) return false;
                if (!emit_cset_w9(b, cond)) return false;
                return emit_fmov_to_d(b, 0, 9);
            }
            return false;
        }
        case NG_AST_EXPR_CALL:
            return emit_call(c, b, e);
        default:
            return false;
    }
}

static bool emit_stmt(NGJitCompiler* c, NGJitBuf* b, NGJitUnit* u, const NGAstNode* s) {
    if (!s) return true;
    c->cur_unit = u;
    if (s->type == NG_AST_PROGRAM) {
        for (size_t i = 0; i < s->as.program.count; i++)
            if (!emit_stmt(c, b, u, s->as.program.statements[i])) return false;
        return true;
    }
    switch (s->type) {
        case NG_AST_STMT_ASSIGN: {
            c->cur_unit = u;
            if (!emit_expr(c, b, s->as.assign_stmt.value, 0)) return false;
            int slot = slot_of(u, s->as.assign_stmt.name, true);
            if (slot < 0) return false;
            if (!emit_fmov_to_x(b, 9, 0)) return false;
            return emit_str_x(b, 9, 19, slot_off(slot));
        }
        case NG_AST_STMT_Y: {
            if (!emit_expr(c, b, s->as.y_stmt.expr, 0)) return false;
            if (!emit_fmov_to_x(b, 9, 0)) return false;
            return emit_str_x(b, 9, 19, 8); /* frame->y */
        }
        case NG_AST_STMT_PRINT: {
            bool is_bool = s->as.print_stmt.expr &&
                ((s->as.print_stmt.expr->type == NG_AST_EXPR_BINARY &&
                  is_cmp(s->as.print_stmt.expr->as.binary_expr.op)) ||
                 (s->as.print_stmt.expr->type == NG_AST_EXPR_LITERAL &&
                  s->as.print_stmt.expr->as.literal_expr.value.type == NG_VAL_BOOL));
            if (!emit_expr(c, b, s->as.print_stmt.expr, 0)) return false;
            void* helper = is_bool ? (void*)ng_jit_helper_print_bool : (void*)ng_jit_helper_print_num;
            if (!emit_mov_const(b, 9, (uint64_t)(uintptr_t)helper)) return false;
            return emit_blr_x9(b);
        }
        case NG_AST_STMT_IF: {
            if (!emit_expr(c, b, s->as.if_stmt.condition, 0)) return false;
            if (!emit_fmov_to_x(b, 9, 0)) return false;
            if (!emit_cmp_w9_zero(b)) return false;
            size_t pc_bne = b->len;
            if (!emit_b_cond(b, pc_bne, pc_bne, 1)) return false;
            if (!emit_stmt(c, b, u, s->as.if_stmt.else_block)) return false;
            size_t pc_bend = b->len;
            if (!emit_b_uncond(b, pc_bend, pc_bend)) return false;
            size_t l_then = b->len;
            uint32_t imm19n = (uint32_t)(((long)l_then - (long)pc_bne) / 4) & 0x7FFFF;
            uint32_t e1 = 0x54000000u + (imm19n << 5) + 1u;
            memcpy(b->data + pc_bne, &e1, 4);
            if (!emit_stmt(c, b, u, s->as.if_stmt.then_block)) return false;
            size_t l_end = b->len;
            uint32_t e2 = 0x14000000u + (((uint32_t)(((long)l_end - (long)pc_bend) / 4)) & 0x3FFFFFF);
            memcpy(b->data + pc_bend, &e2, 4);
            return true;
        }
        case NG_AST_STMT_WHILE: {
            size_t l_loop = b->len;
            if (!emit_expr(c, b, s->as.while_stmt.condition, 0)) return false;
            if (!emit_fmov_to_x(b, 9, 0)) return false;
            if (!emit_cmp_w9_zero(b)) return false;
            size_t pc_beq = b->len;
            if (!emit_b_cond(b, pc_beq, pc_beq, 0)) return false;
            if (!emit_stmt(c, b, u, s->as.while_stmt.block)) return false;
            if (!emit_b_uncond(b, b->len, l_loop)) return false;
            size_t l_exit = b->len;
            uint32_t imm19w = (uint32_t)(((long)l_exit - (long)pc_beq) / 4) & 0x7FFFF;
            uint32_t e = 0x54000000u + (imm19w << 5);
            memcpy(b->data + pc_beq, &e, 4);
            return true;
        }
        default:
            return false;
    }
}

/* Função compilada: entrada = parâmetros em d0..dN -> slots[0..N-1] */
static bool emit_func(NGJitCompiler* c, const NGAstNode* st, NGJitBuf* out, NGJitUnit* unit) {
    size_t n = st->as.func_stmt.param_count;
    /* Preserva LR (x30) durante BLRs aninhados; x19 (frame) já vem do chamador. */
    if (!emit_push_lr(out)) return false;
    for (size_t i = 0; i < n; i++) {
        snprintf(unit->names[i], 32, "%s", st->as.func_stmt.params[i]);
        /* movz x9,d_i ; str x9,[x19,#slot_off(i)] */
        if (!emit_fmov_to_x(out, 9, (int)i)) return false;
        if (!emit_str_x(out, 9, 19, slot_off((int)i))) return false;
    }
    unit->count = n;
    c->cur_unit = unit;
    if (!emit_stmt(c, out, unit, st->as.func_stmt.body)) return false;
    /* resultado da funcao: frame->y -> d0 */
    if (!emit_ldr_x(out, 9, 19, 8)) return false;
    if (!emit_fmov_to_d(out, 0, 9)) return false;
    if (!emit_pop_lr(out)) return false;
    return emit_ret(out);
}

/* --- execução com estrategia W^X --- */

typedef void (*NGJitMainFn)(NGJitFrame*);

bool ng_jit_run_program(const NGAstNode* program, NGInterpreter* interp) {
    (void)interp;
    static int disabled = -1;
    if (disabled < 0) {
        const char* e = getenv("NG_JIT_DISABLE");
        disabled = (e && (strcmp(e, "1") == 0 || strcmp(e, "true") == 0)) ? 1 : 0;
    }
    if (disabled || !can_compile_program(program)) return false;

    /* Probe: verifica se o ambiente suporta mmap RWX.
     * Isso nao desativa o JIT; apenas detecta capacidade para o fallback. */
    bool rwx_supported = ng_jit_is_rwx_supported();

    NGJitCompiler c;
    memset(&c, 0, sizeof(c));
    c.program = program;
    c.cur_unit = &c.main_unit;

    for (size_t i = 0; i < program->as.program.count; i++) {
        const NGAstNode* st = program->as.program.statements[i];
        if (st && st->type == NG_AST_STMT_FUNC) {
            if (c.func_count >= NG_JIT_MAX_FUNCS) return false;
            snprintf(c.func_names[c.func_count], 32, "%s", st->as.func_stmt.name);
            c.func_count++;
        }
    }
    for (size_t i = 0; i < c.func_count; i++) {
        const NGAstNode* st = NULL;
        for (size_t j = 0; j < program->as.program.count; j++) {
            const NGAstNode* cand = program->as.program.statements[j];
            if (cand && cand->type == NG_AST_STMT_FUNC &&
                strcmp(cand->as.func_stmt.name, c.func_names[i]) == 0) { st = cand; break; }
        }
        if (!st) return false;
        if (!emit_func(&c, st, &c.func_codes[i], &c.func_units[i])) return false;
    }

    c.cur_unit = &c.main_unit;
    /* Prólogo: preserva x19/x30, frame pointer -> x19. */
    if (!emit_push_frame(&c.main_code)) return false;
    if (!emit_mov_x19_x0(&c.main_code)) return false;
    for (size_t i = 0; i < program->as.program.count; i++) {
        if (!emit_stmt(&c, &c.main_code, &c.main_unit, program->as.program.statements[i])) return false;
    }
    if (!emit_pop_frame(&c.main_code)) return false;
    if (!emit_ret(&c.main_code)) return false;

    size_t total = c.main_code.len;
    for (size_t i = 0; i < c.func_count; i++) total += c.func_codes[i].len;
    if (total > NG_JIT_MAX_CODE) return false;

    /* W^X strategy: alocar memoria RW, gerar codigo, mudar para RX.
     * Isso evita problemas de segurança e incompatibilidade em ambientes
     * onde PROT_READ|PROT_WRITE|PROT_EXEC simultaneo e bloqueado. */
    uint8_t* code = (uint8_t*)ng_jit_alloc_rw(total);
    if (!code) {
        /* Falha na alocacao RW; tenta fallback para interpretador.
         * Se RWX for suportado, tenta novamente com estrategia antiga.
         * Se nao, recua para o interpretador. */
        if (rwx_supported) {
            /* Fallback: tenta alocacao RWX tradicional */
            code = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (code == MAP_FAILED) {
                if (getenv("NG_JIT_DEBUG")) {
                    fprintf(stderr, "[JIT] falha: mmap RWX nao disponivel\n");
                }
                goto jit_failure;
            }
        } else {
            if (getenv("NG_JIT_DEBUG")) {
                fprintf(stderr, "[JIT] falha: alocacao RW nao disponivel\n");
            }
            goto jit_failure;
        }
    }

    /* Patch relocacoes no codigo alocado */
    size_t func_start[NG_JIT_MAX_FUNCS];
    size_t off = 0;
    memcpy(code + off, c.main_code.data, c.main_code.len);
    off += c.main_code.len;
    for (size_t i = 0; i < c.func_count; i++) {
        func_start[i] = off;
        memcpy(code + off, c.func_codes[i].data, c.func_codes[i].len);
        off += c.func_codes[i].len;
    }

    /* patch: movz x9,#lo; movk x9,#hi1,lsl16; movk x9,#hi2,lsl32; movk x9,#hi3,lsl48 */
    for (size_t i = 0; i < c.reloc_count; i++) {
        uint8_t* base = (c.reloc_buf[i] == 0) ? code : code + func_start[c.reloc_buf[i] - 1];
        uint8_t* p = base + c.reloc_off[i];
        uint64_t addr = (uint64_t)(code + func_start[c.reloc_func[i]]);
        uint32_t e[5];
        e[0] = 0xD2800000u + ((uint32_t)(addr & 0xFFFF) << 5) + 9;
        e[1] = 0xF2800000u + ((1u << 21) + ((uint32_t)((addr >> 16) & 0xFFFF) << 5)) + 9;
        e[2] = 0xF2800000u + ((2u << 21) + ((uint32_t)((addr >> 32) & 0xFFFF) << 5)) + 9;
        e[3] = 0xF2800000u + ((3u << 21) + ((uint32_t)((addr >> 48) & 0xFFFF) << 5)) + 9;
        e[4] = 0xd63f0120u; /* blr x9 */
        memcpy(p, e, 20);
    }

    /* Mudar permissoes de RW para RX usando mprotect (principio W^X). */
    if (!ng_jit_make_rx(code, total)) {
        /* Falha ao mudar permissoes para RX.
         * A memoria permanece RW; nao a executemos com seguranca.
         * Liberamos e recuamos para o interpretador. */
        if (getenv("NG_JIT_DEBUG")) {
            fprintf(stderr, "[JIT] falha: mprotect RW->RX nao disponivel, recuando para interpretador\n");
        }
        ng_jit_free_and_null((void**)&code);
        goto jit_failure;
    }

    /* Sincronizar cache de instrucoes para ARM64/AArch64.
     * Necessario apos mprotect para garantir que o processador veja o codigo. */
    ng_jit_sync_cache(code, total);

    {
        const char* dpath = getenv("NG_JIT_DUMP");
        if (dpath) {
            FILE* f = fopen(dpath, "wb");
            if (f) { fwrite(code, 1, total, f); fclose(f); }
        }
    }
    NGJitFrame* frame = (NGJitFrame*)calloc(1, sizeof(NGJitFrame));
    if (frame) {
        ((NGJitMainFn)code)(frame);
        free(frame);
    }
    munmap(code, total);
    free(c.main_code.data);
    for (size_t i = 0; i < c.func_count; i++) free(c.func_codes[i].data);
    if (getenv("NG_JIT_DEBUG")) {
        fprintf(stderr, "[JIT] ok: %zu funcoes, %zu bytes de codigo\n", c.func_count, total);
    }
    return true;

jit_failure:
    /* Liberacao segura de memoria em todos os caminhos de erro. */
    if (code) {
        munmap(code, total);
    }
    free(c.main_code.data);
    for (size_t i = 0; i < c.func_count; i++) free(c.func_codes[i].data);
    if (getenv("NG_JIT_DEBUG")) {
        fprintf(stderr, "[JIT] fallback: recuando para interpretador C\n");
    }
    return false;
}

bool ng_jit_enabled(void) {
    const char* e = getenv("NG_JIT_DISABLE");
    return !(e && (strcmp(e, "1") == 0 || strcmp(e, "true") == 0));
}

#endif /* NG_JIT_BACKEND != 1 */
