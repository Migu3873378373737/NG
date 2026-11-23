#ifndef NG_DYNAMICV_H
#define NG_DYNAMICV_H

#include "ng_ast.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NG_HAS_EMBEDDED_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#endif

typedef enum {
    NG_DV_STATE_UNINITIALIZED = 0,
    NG_DV_STATE_IDLE,
    NG_DV_STATE_OPTIMIZING,
    NG_DV_STATE_EXECUTING,
    NG_DV_STATE_FALLBACK,
    NG_DV_STATE_STOPPED
} NGDynamicVState;

typedef struct {
    _Atomic uint64_t hits;
    _Atomic uint64_t misses;
    _Atomic uint64_t tasks_dispatched;
    _Atomic uint64_t tasks_completed;
    _Atomic uint64_t constant_folds;
    _Atomic uint64_t dead_code_eliminations;
    _Atomic uint64_t algebraic_simplifications;
    _Atomic uint64_t fallbacks_to_core0;
    _Atomic uint64_t optimizations_applied; /* total de otimizações aplicadas pelo Core 1 */
    _Atomic uint64_t ir_generated;          /* IRs geradas pelo Core 1 */
    _Atomic double core1_time_ms;
    size_t l2_current_bytes;
    size_t l2_peak_bytes;
} NGDynamicVStats;

/* Inicialização e finalização do ciclo de vida Dual-Core */
void ng_dynamicv_init(void);
void ng_dynamicv_cleanup(void);

/* Controle e verificação de ativação */
bool ng_dynamicv_is_enabled(void);
void ng_dynamicv_set_enabled(bool enabled);
NGDynamicVState ng_dynamicv_get_core0_state(void);
NGDynamicVState ng_dynamicv_get_core1_state(void);
const NGDynamicVStats* ng_dynamicv_get_stats(void);

/* Pipeline unificado Dual-Core (Miss -> Core1 opt -> N3 save / Hit -> N3 load) */
NGAstNode* ng_dynamicv_prepare_program(const char* filepath, const char* source,
                                      bool store_compiled, bool* out_error);

/* Otimizador semântico de AST / IR (executado no Core 1 com fallback seguro) */
NGAstNode* ng_dynamicv_optimize_ast(NGAstNode* ast);

/* Benchmark/diagnóstico: mede tempos de miss (N3/miss) e hit (N3), memória e
   sync/threads do barramento (imprime no stream aberto). */
void ng_dynamicv_bench(FILE* out);

/* Subcomandos do CLI: ng dynamicv status|cache|limpar|diagnostico */
void ng_dynamicv_status(FILE* out);
void ng_dynamicv_cache_info(FILE* out);
void ng_dynamicv_clean_cache(void);
void ng_dynamicv_diagnostico(FILE* out);

#endif // NG_DYNAMICV_H
